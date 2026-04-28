#pragma once

// This header is only active when the protobuf protocol is selected.
// It provides encode functions called by Metrics, Logger, and Tracer
// instead of building a JsonDocument.
#if OTEL_EXPORTER_OTLP_PROTOCOL == OTEL_EXPORTER_OTLP_PROTOCOL_HTTP_PROTOBUF

#include <Arduino.h>
#include <map>
#include <vector>
#include <utility>

namespace OTel {
namespace Proto {

// Typed attribute mirroring Span::Attr — forward declared here so Tracer
// can pass its internal buffer without a circular dependency.
enum class AttrType { Str, Int, Dbl, Bool };
struct Attr {
  String    key;
  AttrType  type{AttrType::Str};
  String    s;
  int64_t   i{0};
  double    d{0.0};
  bool      b{false};
};
struct Event {
  String          name;
  uint64_t        t{0};
  std::vector<Attr> attrs;
};

// ── Signal encoders ──────────────────────────────────────────────────────────

// Encode and send a Gauge metric datapoint.
void sendGauge(const String& name, double value, const String& unit,
               const std::map<String,String>& labels);

// Encode and send a Sum metric datapoint.
// temporality: 1 = DELTA, 2 = CUMULATIVE
void sendSum(const String& name, double value, bool isMonotonic,
             int temporality, const String& unit,
             const std::map<String,String>& labels);

// Encode and send a LogRecord.
void sendLog(const String& severity, int severityNum, const String& message,
             const std::map<String,String>& callLabels,
             const std::map<String,String>& defaultLabels,
             const String& traceId, const String& spanId);

// Encode and send a Span.
// traceId / spanId / parentSpanId are 32/16/16 hex chars respectively.
void sendSpan(const String& name,
              const String& traceId,
              const String& spanId,
              const String& parentSpanId,
              uint64_t startNs,
              uint64_t endNs,
              const std::vector<Attr>& attrs,
              const std::vector<Event>& events);

} // namespace Proto
} // namespace OTel

#endif // OTEL_EXPORTER_OTLP_PROTOCOL_HTTP_PROTOBUF
