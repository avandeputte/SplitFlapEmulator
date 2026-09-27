// Update.h -- the browser OTA upload: the image is received and validated for size, then the
// gateway reboots exactly as it would after a real flash (it comes back on the emulator's
// built-in firmware; the uploaded file is kept under the data volume for inspection).
#pragma once
#include "Arduino.h"
#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
#define U_FLASH  0
#define U_SPIFFS 100
class UpdateClass {
public:
  bool begin(size_t size = UPDATE_SIZE_UNKNOWN, int command = U_FLASH, int ledPin = -1, uint8_t ledOn = 0, const char* label = NULL);
  size_t write(uint8_t* data, size_t len);
  bool end(bool evenIfRemaining = false);
  void abort();
  bool isFinished() { return _finished; }
  bool isRunning() { return _running; }
  bool hasError() { return _error != 0; }
  const char* errorString();
  size_t progress() { return _written; }
private:
  FILE* _f = nullptr; size_t _written = 0; bool _running = false, _finished = false; int _error = 0;
};
extern UpdateClass Update;
