// MD5Builder / SHA1Builder / base64 -- reached only by WebServer authentication and ETag code
// paths the gateway never enables. Minimal stand-ins so the vendored source links.
#pragma once
#include "Arduino.h"
class HashBuilderStub {
public:
  void begin() { _s = ""; }
  void add(const uint8_t* d, size_t n) { for (size_t i = 0; i < n; i++) _s += (char)d[i]; }
  void add(const char* s) { _s += s; }
  void add(String s) { _s += s; }
  bool addStream(Stream&, const size_t) { return false; }
  void calculate() { uint32_t h = 2166136261u; for (size_t i = 0; i < _s.length(); i++) { h ^= (uint8_t)_s[i]; h *= 16777619u; } _h = h; }
  void getBytes(uint8_t* out) { for (int i = 0; i < 20; i++) out[i] = (uint8_t)(_h >> ((i % 4) * 8)); }
  void getChars(char* out) { snprintf(out, 41, "%08x%08x%08x%08x%08x", _h, _h, _h, _h, _h); }
  String toString() { char b[48]; getChars(b); return String(b); }
  void bytes2hex(char* out, size_t outLen, const uint8_t* in, size_t n) { size_t k = 0; for (size_t i = 0; i < n && k + 2 < outLen; i++) k += snprintf(out + k, outLen - k, "%02x", in[i]); }
private:
  String _s; uint32_t _h = 0;
};
class MD5Builder  : public HashBuilderStub {};
class SHA1Builder : public HashBuilderStub {};
