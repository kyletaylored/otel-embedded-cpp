#pragma once
// Minimal Arduino shim for native (Linux/macOS) unit tests.
// Provides just enough of the Arduino API surface that otel-embedded-cpp
// headers can be compiled on a host toolchain without modification.

#ifndef ARDUINO
#define ARDUINO 100
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <cstdio>

// ── String ────────────────────────────────────────────────────────────────────
// std::string subclass that adds the Arduino-compatible constructors and
// helpers used throughout the library (c_str, length, indexOf, etc.).
class String : public std::string {
public:
  String() = default;
  String(const char* s) : std::string(s ? s : "") {}
  String(const std::string& s) : std::string(s) {}
  String(char c) : std::string(1, c) {}
  explicit String(int v)           : std::string(std::to_string(v)) {}
  explicit String(long v)          : std::string(std::to_string(v)) {}
  explicit String(unsigned int v)  : std::string(std::to_string(v)) {}
  explicit String(unsigned long v) : std::string(std::to_string(v)) {}
  explicit String(float v)         : std::string(std::to_string(v)) {}
  explicit String(double v)        : std::string(std::to_string(v)) {}

  unsigned int length() const { return (unsigned int)size(); }

  int indexOf(char c, unsigned int from = 0) const {
    auto p = find(c, from);
    return p == npos ? -1 : (int)p;
  }
  String substring(unsigned int from, unsigned int to = 0) const {
    return to ? String(substr(from, to - from)) : String(substr(from));
  }
  int   toInt()   const { return empty() ? 0   : std::stoi(*this); }
  float toFloat() const { return empty() ? 0.f : std::stof(*this); }

  String& operator+=(const char*   s) { std::string::operator+=(s); return *this; }
  String& operator+=(const String& s) { std::string::operator+=(s); return *this; }
  String  operator+(const String& s) const { return String(std::string(*this) + std::string(s)); }
  String  operator+(const char*   s) const { return String(std::string(*this) + s); }
  bool    operator==(const char*   s) const { return compare(s) == 0; }
  bool    operator!=(const char*   s) const { return compare(s) != 0; }
};

inline String operator+(const char* a, const String& b) {
  return String(std::string(a) + std::string(b));
}

// ── Flash string ──────────────────────────────────────────────────────────────
#define F(x) (x)

// ── Serial stub ───────────────────────────────────────────────────────────────
struct HardwareSerial {
  void begin(unsigned long) {}
  template<typename T>         size_t print(T)           { return 0; }
  template<typename T>         size_t println(T)         { return 0; }
  template<typename T, int U>  size_t print(T, int)      { return 0; }
  template<typename T, int U>  size_t println(T, int)    { return 0; }
  size_t println()                                        { return 0; }
  void   printf(const char*, ...)                         {}
};
static HardwareSerial Serial;

// ── Timing stubs ──────────────────────────────────────────────────────────────
static inline unsigned long millis()  { return 0; }
static inline unsigned long micros()  { return 0; }
static inline void delay(unsigned long) {}

// ── PRNG ──────────────────────────────────────────────────────────────────────
static inline void randomSeed(unsigned long seed) { srand((unsigned int)seed); }
static inline long random(long max) { return (long)(rand() % max); }
static inline long random(long min, long max) { return min + (long)(rand() % (max - min)); }
