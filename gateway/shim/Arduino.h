// Arduino.h -- the emulator's stand-in for the Arduino-ESP32 core (3.x) as seen by the
// SplitFlapGateway firmware, which is compiled UNMODIFIED against it.
//
// Real core classes that are pure C++ (String, Print, Stream) are vendored verbatim from the
// ESP32 core; the ESP32 WebServer library is vendored too and runs on the POSIX network client
// and server defined in NetworkClient.h / NetworkServer.h. Everything that touched the chip --
// FreeRTOS tasks and mutexes, the heap and chip-info calls, the UART, WiFi, NVS, FATFS, the
// RTC over I2C, NTP, OTA -- is emulated in the *.cpp files next to this header.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <type_traits>
#include <stdarg.h>
#include <inttypes.h>
#include <unistd.h>
#include "pgmspace.h"
#include "esp32-hal-log.h"

typedef bool    boolean;
typedef uint8_t byte;
#define HIGH 1
#define LOW  0
#define INPUT 1
#define OUTPUT 3
#define INPUT_PULLUP 5
#define PI 3.1415926535897932384626433832795
#define IRAM_ATTR
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
#define ARDUINO 10819
#define ESP32 1
#define ARDUINO_ARCH_ESP32 1
#define ARDUINO_RUNNING_CORE 1

#ifndef _min
#define _min(a,b) ((a)<(b)?(a):(b))
#endif
// By value: a conditional of two lvalue parameters is an lvalue, and returning its decltype
// would hand the caller a reference to a parameter that no longer exists.
template <typename A, typename B> inline typename std::common_type<A, B>::type min(A a, B b) { return a < b ? a : b; }
template <typename A, typename B> inline typename std::common_type<A, B>::type max(A a, B b) { return a > b ? a : b; }
template <typename T> inline T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
inline bool isDigit(int c) { return c >= '0' && c <= '9'; }
inline bool isAlpha(int c) { return isalpha(c) != 0; }
inline bool isAlphaNumeric(int c) { return isalnum(c) != 0; }
inline bool isSpace(int c) { return isspace(c) != 0; }
inline bool isPrintable(int c) { return isprint(c) != 0; }
inline bool isHexadecimalDigit(int c) { return isxdigit(c) != 0; }

#include "stdlib_noniso.h"
#include "WString.h"
#include "Printable.h"
#include "Print.h"
#include "Stream.h"
#include "IPAddress.h"

// ---- time --------------------------------------------------------------------------------------
unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
void yield();
void configTime(long gmtOffset_sec, int daylightOffset_sec, const char* server1, const char* server2 = NULL, const char* server3 = NULL);
bool getLocalTime(struct tm* info, uint32_t ms = 5000);
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long);

// ---- strlcpy/strlcat (missing on older glibc) ------------------------------------------------
#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38))
extern "C" size_t strlcpy(char* dst, const char* src, size_t size);
extern "C" size_t strlcat(char* dst, const char* src, size_t size);
#endif

// ---- FreeRTOS (the subset used) ------------------------------------------------------------------
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void* TaskHandle_t;
typedef void* SemaphoreHandle_t;
struct StaticSemaphore_t { void* impl; };
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskNO_AFFINITY 0x7fffffff
typedef void (*TaskFunction_t)(void*);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* buf);
SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t h);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* arg, UBaseType_t prio, TaskHandle_t* out, BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* arg, UBaseType_t prio, TaskHandle_t* out);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t h);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h);
TickType_t xTaskGetTickCount();
#define taskYIELD() yield()

// ---- ESP chip services -----------------------------------------------------------------------------
typedef enum {
  ESP_RST_UNKNOWN = 0, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO
} esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
bool psramFound();
class EspClass {
public:
  uint32_t getFreeHeap();
  uint32_t getMinFreeHeap();
  uint32_t getMaxAllocHeap();
  uint32_t getPsramSize() { return 8 * 1024 * 1024; }
  uint32_t getFreePsram();
  uint32_t getFlashChipSize() { return 16 * 1024 * 1024; }
  uint32_t getCpuFreqMHz() { return 240; }
  const char* getSdkVersion() { return "v5.4.2-emu"; }
  uint64_t getEfuseMac();
  void restart();
};
extern EspClass ESP;
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"

// ---- USB CDC console ---------------------------------------------------------------------------------
class HWCDC : public Stream {
public:
  void begin(unsigned long = 0) {}
  void end() {}
  operator bool() const { return true; }
  size_t write(uint8_t b) override { return fwrite(&b, 1, 1, stdout); }
  size_t write(const uint8_t* b, size_t n) override { return fwrite(b, 1, n, stdout); }
  using Print::write;
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { fflush(stdout); }
};
extern HWCDC Serial;

#include "HardwareSerial.h"

void setup();
void loop();
