#pragma once
#include "Arduino.h"
class MDNSResponder {
public:
  bool begin(const char* host) { return true; }
  void end() {}
  bool addService(const char* service, const char* proto, uint16_t port);
  bool addService(String s, String p, uint16_t port) { return addService(s.c_str(), p.c_str(), port); }
};
extern MDNSResponder MDNS;
