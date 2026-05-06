#include "OtelSender.h"
#include <vector>
#include <utility>

// --- HTTP + WiFi includes (portable) ---
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
  #include <WiFiClientSecure.h>
#elif defined(ESP32)
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#elif defined(ARDUINO_ARCH_RP2040)
  #include <WiFi.h>        // Earle Philhower core
  #include <HTTPClient.h>  // Arduino HTTPClient
  #include <WiFiClientSecure.h>
#else
  #error "Unsupported platform: need WiFi + HTTPClient"
#endif

#ifdef ARDUINO_ARCH_RP2040
  #include "pico/multicore.h"
#endif

// ===== statics =====
OTelQueuedItem OTelSender::q_[QCAP];
std::atomic<size_t> OTelSender::head_{0};
std::atomic<size_t> OTelSender::tail_{0};
std::atomic<uint32_t> OTelSender::drops_{0};
std::atomic<bool>    OTelSender::worker_started_{false};

// ---------- Header parsing ----------

// Append "key=value,…" pairs from raw into out. Existing entries are preserved
// so callers can layer global headers then per-signal headers.
static void appendParsedHeaders_(const char* raw,
                                 std::vector<std::pair<String,String>>& out) {
  if (!raw || !*raw) return;
  String s(raw);
  int start = 0;
  while (start <= (int)s.length()) {
    int comma = s.indexOf(',', start);
    if (comma < 0) comma = (int)s.length();
    String token = s.substring(start, comma);
    token.trim();
    int eq = token.indexOf('=');
    if (eq > 0) {
      String k = token.substring(0, eq);
      String v = token.substring(eq + 1);
      k.trim(); v.trim();
      if (k.length()) out.push_back({k, v});
    }
    start = comma + 1;
  }
}

// Returns the merged header list for the given OTLP path (/v1/logs etc.).
// Built once on first call and cached for the lifetime of the program.
static const std::vector<std::pair<String,String>>& headersForPath_(const char* path) {
  static bool init = false;
  static std::vector<std::pair<String,String>> logH, traceH, metricH;
  if (!init) {
    init = true;
    // Global headers first, then per-signal overrides
    appendParsedHeaders_(OTEL_EXPORTER_OTLP_HEADERS, logH);
    appendParsedHeaders_(OTEL_EXPORTER_OTLP_HEADERS, traceH);
    appendParsedHeaders_(OTEL_EXPORTER_OTLP_HEADERS, metricH);
    appendParsedHeaders_(OTEL_EXPORTER_OTLP_LOGS_HEADERS,    logH);
    appendParsedHeaders_(OTEL_EXPORTER_OTLP_TRACES_HEADERS,  traceH);
    appendParsedHeaders_(OTEL_EXPORTER_OTLP_METRICS_HEADERS, metricH);
  }
  if (path && strcmp(path, "/v1/logs")    == 0) return logH;
  if (path && strcmp(path, "/v1/traces")  == 0) return traceH;
  return metricH;
}

// ---------- URL resolution ----------

// Returns the full URL for the given OTLP signal path.
//
// Priority (highest to lowest):
//   1. Per-signal endpoint (OTEL_EXPORTER_OTLP_{LOGS,TRACES,METRICS}_ENDPOINT)
//      → used as-is, no path appended (spec requirement)
//   2. Standard base URL (OTEL_EXPORTER_OTLP_ENDPOINT)
//      → signal path appended automatically
//   3. Legacy base URL (OTEL_COLLECTOR_BASE_URL)
//      → signal path appended automatically
String OTelSender::fullUrl_(const char* path) {
  // 1. Per-signal overrides (used verbatim per spec)
  if (path) {
    if (strcmp(path, "/v1/logs") == 0 && strlen(OTEL_EXPORTER_OTLP_LOGS_ENDPOINT) > 0)
      return String(OTEL_EXPORTER_OTLP_LOGS_ENDPOINT);
    if (strcmp(path, "/v1/traces") == 0 && strlen(OTEL_EXPORTER_OTLP_TRACES_ENDPOINT) > 0)
      return String(OTEL_EXPORTER_OTLP_TRACES_ENDPOINT);
    if (strcmp(path, "/v1/metrics") == 0 && strlen(OTEL_EXPORTER_OTLP_METRICS_ENDPOINT) > 0)
      return String(OTEL_EXPORTER_OTLP_METRICS_ENDPOINT);
  }

  // 2/3. Base URL with signal path appended
  String base = strlen(OTEL_EXPORTER_OTLP_ENDPOINT) > 0
                ? String(OTEL_EXPORTER_OTLP_ENDPOINT)
                : String(OTEL_COLLECTOR_BASE_URL);
  if (base.endsWith("/")) base.remove(base.length() - 1);
  if (path && *path == '/') return base + String(path);
  return base + "/" + String(path ? path : "");
}

// ---------- HTTP/HTTPS POST ----------

// Executes a single POST. Handles plain HTTP and HTTPS transparently based on
// the URL scheme. Custom headers are applied after Content-Type.
static void doPost_(const String& url, const String& payload,
                    const char* path, const char* contentType) {
  HTTPClient http;
  const auto& hdrs = headersForPath_(path);

  // Lambda keeps the send logic in one place regardless of client type.
  auto fire = [&](bool ok) {
    if (!ok) return;
    http.addHeader("Content-Type", contentType);
    for (const auto& h : hdrs) http.addHeader(h.first, h.second);
    (void)http.POST(payload);
    http.end();
  };

  if (url.startsWith("https://")) {
    // WiFiClientSecure must remain in scope until after http.end() (fire()).
    WiFiClientSecure sc;
#if OTEL_TLS_INSECURE
    sc.setInsecure();
#endif
    fire(http.begin(sc, url));
  } else {
#if defined(ESP8266)
    WiFiClient wc;
    fire(http.begin(wc, url));
#else
    fire(http.begin(url));
#endif
  }
}

// ---------- Queue (SPSC) ----------

bool OTelSender::enqueue_(const char* path, const char* contentType, String&& payload) {
  size_t h = head_.load(std::memory_order_relaxed);
  size_t t = tail_.load(std::memory_order_acquire);
  size_t next = (h + 1) % QCAP;

  if (next == t) {
    // Full: drop oldest (advance tail)
    size_t new_t = (t + 1) % QCAP;
    tail_.store(new_t, std::memory_order_release);
    drops_.fetch_add(1, std::memory_order_relaxed);
  }

  q_[h].path        = path;
  q_[h].contentType = contentType;
  q_[h].payload     = std::move(payload);
  head_.store(next, std::memory_order_release);
  return true;
}

bool OTelSender::dequeue_(OTelQueuedItem& out) {
  size_t t = tail_.load(std::memory_order_relaxed);
  size_t h = head_.load(std::memory_order_acquire);
  if (t == h) return false;

  out = std::move(q_[t]);
  q_[t].payload = String();
  size_t next = (t + 1) % QCAP;
  tail_.store(next, std::memory_order_release);
  return true;
}

// ---------- Worker ----------

void OTelSender::pumpOnce_() {
  OTelQueuedItem it;
  if (!dequeue_(it)) return;
#if OTEL_SEND_ENABLE
  doPost_(fullUrl_(it.path), it.payload, it.path, it.contentType);
#endif
}

void OTelSender::workerLoop_() {
  for (;;) {
    for (int i = 0; i < OTEL_WORKER_BURST; ++i) {
      OTelQueuedItem it;
      if (!dequeue_(it)) break;
#if OTEL_SEND_ENABLE
      doPost_(fullUrl_(it.path), it.payload, it.path, it.contentType);
#endif
    }
    delay(OTEL_WORKER_SLEEP_MS);
  }
}

#ifdef ARDUINO_ARCH_RP2040
void otel_worker_entry() { OTelSender::workerLoop_(); }
#endif

void OTelSender::launchWorkerOnce_() {
#ifdef ARDUINO_ARCH_RP2040
  bool expected = false;
  if (worker_started_.compare_exchange_strong(expected, true)) {
    multicore_launch_core1(otel_worker_entry);
  }
#endif
}

void OTelSender::beginAsyncWorker() {
  launchWorkerOnce_();
}

uint32_t OTelSender::droppedCount() {
  return drops_.load(std::memory_order_relaxed);
}

bool OTelSender::queueIsHealthy() {
  return worker_started_.load(std::memory_order_relaxed);
}

// ---------- Public send API ----------

static constexpr const char* kContentTypeJson  = "application/json";
static constexpr const char* kContentTypeProto = "application/x-protobuf";

void OTelSender::sendJson(const char* path, JsonDocument& doc) {
#if !OTEL_SEND_ENABLE
  (void)path; (void)doc;
  return;
#else
  String payload;
  serializeJson(doc, payload);

#ifdef ARDUINO_ARCH_RP2040
  launchWorkerOnce_();
  enqueue_(path, kContentTypeJson, std::move(payload));
#else
  doPost_(fullUrl_(path), payload, path, kContentTypeJson);
#endif
#endif
}

void OTelSender::sendProto(const char* path, const uint8_t* buf, size_t len) {
#if !OTEL_SEND_ENABLE
  (void)path; (void)buf; (void)len;
  return;
#else
  // Copy raw bytes into a String to reuse the existing queue/send path.
  // The String is treated as an opaque byte container, not a text string.
  // Using the length-based constructor avoids the O(n) append loop and
  // correctly handles null bytes that are valid in protobuf payloads.
  String payload(reinterpret_cast<const char*>(buf), len);

#ifdef ARDUINO_ARCH_RP2040
  launchWorkerOnce_();
  enqueue_(path, kContentTypeProto, std::move(payload));
#else
  doPost_(fullUrl_(path), payload, path, kContentTypeProto);
#endif
#endif
}
