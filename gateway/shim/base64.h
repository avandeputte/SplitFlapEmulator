#pragma once
#include "Arduino.h"
class base64 {
public:
  static String encode(const uint8_t* data, size_t len);
  static String encode(const String& s) { return encode((const uint8_t*)s.c_str(), s.length()); }
};
