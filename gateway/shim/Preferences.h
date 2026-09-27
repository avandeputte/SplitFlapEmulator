// Preferences.h -- NVS namespaces persisted as JSON files under <data>/nvs/<name>.json.
#pragma once
#include "Arduino.h"
enum PreferenceType { PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_I64, PT_U64, PT_STR, PT_BLOB, PT_INVALID };
class Preferences {
public:
  bool   begin(const char* name, bool readOnly = false, const char* partition = NULL);
  void   end();
  bool   clear();
  bool   remove(const char* key);
  bool   isKey(const char* key);
  size_t putChar(const char* k, int8_t v)      { return putNum(k, (double)v); }
  size_t putUChar(const char* k, uint8_t v)    { return putNum(k, (double)v); }
  size_t putShort(const char* k, int16_t v)    { return putNum(k, (double)v); }
  size_t putUShort(const char* k, uint16_t v)  { return putNum(k, (double)v); }
  size_t putInt(const char* k, int32_t v)      { return putNum(k, (double)v); }
  size_t putUInt(const char* k, uint32_t v)    { return putNum(k, (double)v); }
  size_t putLong(const char* k, int32_t v)     { return putNum(k, (double)v); }
  size_t putULong(const char* k, uint32_t v)   { return putNum(k, (double)v); }
  size_t putBool(const char* k, bool v)        { return putNum(k, v ? 1 : 0); }
  size_t putString(const char* k, const char* v);
  size_t putString(const char* k, String v)    { return putString(k, v.c_str()); }
  int8_t   getChar(const char* k, int8_t d = 0)     { return (int8_t)getNum(k, d); }
  uint8_t  getUChar(const char* k, uint8_t d = 0)   { return (uint8_t)getNum(k, d); }
  int16_t  getShort(const char* k, int16_t d = 0)   { return (int16_t)getNum(k, d); }
  uint16_t getUShort(const char* k, uint16_t d = 0) { return (uint16_t)getNum(k, d); }
  int32_t  getInt(const char* k, int32_t d = 0)     { return (int32_t)getNum(k, d); }
  uint32_t getUInt(const char* k, uint32_t d = 0)   { return (uint32_t)getNum(k, d); }
  int32_t  getLong(const char* k, int32_t d = 0)    { return (int32_t)getNum(k, d); }
  uint32_t getULong(const char* k, uint32_t d = 0)  { return (uint32_t)getNum(k, d); }
  bool     getBool(const char* k, bool d = false)   { return getNum(k, d ? 1 : 0) != 0; }
  size_t   getString(const char* k, char* out, size_t maxLen);
  String   getString(const char* k, String d = String());
private:
  size_t putNum(const char* k, double v);
  double getNum(const char* k, double d);
  void   save();
  String _name; bool _ro = false; bool _open = false; bool _dirty = false;
  void*  _doc = nullptr;   // ArduinoJson document (kept opaque here)
};
