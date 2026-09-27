// ArduinoOTA.h -- the IDE/espota push path. Advertised but not served: nothing pushes images
// to an emulator. Callbacks are stored so the firmware's registrations compile and run.
#pragma once
#include "Arduino.h"
#include <functional>
typedef enum { OTA_AUTH_ERROR, OTA_BEGIN_ERROR, OTA_CONNECT_ERROR, OTA_RECEIVE_ERROR, OTA_END_ERROR } ota_error_t;
class ArduinoOTAClass {
public:
  typedef std::function<void(void)> THandlerFunction;
  typedef std::function<void(ota_error_t)> THandlerFunction_Error;
  typedef std::function<void(unsigned int, unsigned int)> THandlerFunction_Progress;
  ArduinoOTAClass& setPort(uint16_t) { return *this; }
  ArduinoOTAClass& setHostname(const char* h) { _host = h; return *this; }
  ArduinoOTAClass& setPassword(const char*) { return *this; }
  ArduinoOTAClass& onStart(THandlerFunction f) { _start = f; return *this; }
  ArduinoOTAClass& onEnd(THandlerFunction f) { _end = f; return *this; }
  ArduinoOTAClass& onProgress(THandlerFunction_Progress f) { _prog = f; return *this; }
  ArduinoOTAClass& onError(THandlerFunction_Error f) { _err = f; return *this; }
  void begin();
  void handle() {}
  int getCommand() { return 0; }
  String getHostname() { return _host; }
private:
  String _host; THandlerFunction _start, _end; THandlerFunction_Progress _prog; THandlerFunction_Error _err;
};
extern ArduinoOTAClass ArduinoOTA;
