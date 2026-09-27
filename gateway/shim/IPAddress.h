// IPAddress.h -- IPv4 only, the subset the gateway, PubSubClient and WebServer use.
#pragma once
#include <stdint.h>
#include "Printable.h"
#include "WString.h"

class IPAddress : public Printable {
public:
  IPAddress() { _a.dword = 0; }
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) { _a.bytes[0] = a; _a.bytes[1] = b; _a.bytes[2] = c; _a.bytes[3] = d; }
  IPAddress(uint32_t addr) { _a.dword = addr; }
  IPAddress(const uint8_t* p) { for (int i = 0; i < 4; i++) _a.bytes[i] = p[i]; }
  bool fromString(const char* s);
  bool fromString(const String& s) { return fromString(s.c_str()); }
  operator uint32_t() const { return _a.dword; }
  bool operator==(const IPAddress& o) const { return _a.dword == o._a.dword; }
  bool operator!=(const IPAddress& o) const { return _a.dword != o._a.dword; }
  uint8_t operator[](int i) const { return _a.bytes[i]; }
  uint8_t& operator[](int i) { return _a.bytes[i]; }
  uint8_t* raw_address() { return _a.bytes; }
  String toString() const;
  virtual size_t printTo(Print& p) const;
private:
  union { uint8_t bytes[4]; uint32_t dword; } _a;
};
extern const IPAddress INADDR_NONE_IP;
