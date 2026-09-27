// core.cpp -- time, FreeRTOS primitives, chip services, IPAddress, misc Arduino glue.
#include "Arduino.h"
#include "emu.h"
#include <mutex>
#include <thread>
#include <chrono>
#include <pthread.h>
#include <random>

GatewayEmu g_emu;
HWCDC      Serial;
EspClass   ESP;
const IPAddress INADDR_NONE_IP;

// ---- time ------------------------------------------------------------------------------------
unsigned long millis() { return (unsigned long)((sfemu::vclock().now_us() - g_emu.bootUs) / 1000); }
unsigned long micros() { return (unsigned long)(sfemu::vclock().now_us() - g_emu.bootUs); }
void delay(unsigned long ms) { sfemu::vclock().sleep_us((int64_t)ms * 1000); }
void delayMicroseconds(unsigned int us) { sfemu::vclock().sleep_us(us); }
void yield() { std::this_thread::yield(); }

static void setTimeZone(long offset, int daylight) {   // verbatim logic of esp32-hal-time.c
  char cst[21] = {0}; char cdt[21] = "DST"; char tz[41] = {0};
  if (offset % 3600) snprintf(cst, sizeof(cst), "UTC%ld:%02u:%02u", offset / 3600, (unsigned)abs((int)((offset % 3600) / 60)), (unsigned)abs((int)(offset % 60)));
  else               snprintf(cst, sizeof(cst), "UTC%ld", offset / 3600);
  if (daylight != 3600) {
    long tz_dst = offset - daylight;
    if (tz_dst % 3600) snprintf(cdt, sizeof(cdt), "DST%ld:%02u:%02u", tz_dst / 3600, (unsigned)abs((int)((tz_dst % 3600) / 60)), (unsigned)abs((int)(tz_dst % 60)));
    else               snprintf(cdt, sizeof(cdt), "DST%ld", tz_dst / 3600);
  }
  snprintf(tz, sizeof(tz), "%s%s", cst, cdt);
  setenv("TZ", tz, 1);
  tzset();
}
static std::string g_ntpServer;
void configTime(long gmtOffset_sec, int daylightOffset_sec, const char* server1, const char*, const char*) {
  g_ntpServer = server1 ? server1 : "";
  setTimeZone(-gmtOffset_sec, daylightOffset_sec);
}
bool getLocalTime(struct tm* info, uint32_t ms) {
  // NTP answers only with the network up; the host's clock is the time server.
  unsigned long start = millis();
  while (!g_emu.wifiUp && millis() - start < ms) delay(10);
  if (!g_emu.wifiUp) return false;
  time_t now = time(nullptr);
  localtime_r(&now, info);
  return true;
}
static std::mt19937 g_rand(42);
long random(long max) { return max > 0 ? (long)(g_rand() % (unsigned long)max) : 0; }
long random(long min, long max) { return min + random(max - min); }
void randomSeed(unsigned long s) { g_rand.seed((unsigned)s); }

#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38))
extern "C" size_t strlcpy(char* dst, const char* src, size_t size) {
  size_t n = strlen(src);
  if (size) { size_t c = n >= size ? size - 1 : n; memcpy(dst, src, c); dst[c] = 0; }
  return n;
}
extern "C" size_t strlcat(char* dst, const char* src, size_t size) {
  size_t d = strnlen(dst, size);
  if (d == size) return size + strlen(src);
  return d + strlcpy(dst + d, src, size - d);
}
#endif

// ---- FreeRTOS ----------------------------------------------------------------------------------
struct TaskRec { std::string name; unsigned hwm; };
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* buf) { auto* m = new std::timed_mutex(); if (buf) buf->impl = m; return m; }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::timed_mutex(); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t ticks) {
  auto* m = (std::timed_mutex*)h;
  if (!m) return pdFALSE;
  if (ticks == portMAX_DELAY) { m->lock(); return pdTRUE; }
  // ticks are virtual milliseconds; convert to real time at the current speed
  double sp = sfemu::vclock().speed();
  auto real = std::chrono::microseconds((int64_t)(ticks * 1000.0 / sp));
  return m->try_lock_for(real) ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { auto* m = (std::timed_mutex*)h; if (m) m->unlock(); return pdTRUE; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack, void* arg, UBaseType_t, TaskHandle_t* out, BaseType_t) {
  auto* rec = new TaskRec{name ? name : "task", (unsigned)(stack / 4 + (stack % 977))};
  if (out) *out = rec;
  try {
    std::thread([fn, arg, rec] { fn(arg); }).detach();
  } catch (...) { return pdFAIL; }
  return pdPASS;
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* arg, UBaseType_t prio, TaskHandle_t* out) {
  return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, 0);
}
void vTaskDelay(TickType_t ticks) { delay(ticks); }
void vTaskDelete(TaskHandle_t h) { if (h == nullptr) pthread_exit(nullptr); }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h) { return h ? ((TaskRec*)h)->hwm : 0; }
TickType_t xTaskGetTickCount() { return (TickType_t)millis(); }

// ---- chip services -------------------------------------------------------------------------------
esp_reset_reason_t esp_reset_reason(void) { return (esp_reset_reason_t)g_emu.resetReason; }
bool psramFound() { return true; }
uint32_t EspClass::getFreeHeap()     { return 187000 + (uint32_t)(millis() / 1000 % 37) * 16; }
uint32_t EspClass::getMinFreeHeap()  { return 151200; }
uint32_t EspClass::getMaxAllocHeap() { return 110592; }
uint32_t EspClass::getFreePsram()    { return 8 * 1024 * 1024 - 70000; }
uint64_t EspClass::getEfuseMac() {
  uint64_t v = 0;
  for (int i = 0; i < 6; i++) v |= (uint64_t)g_emu.mac[i] << (8 * i);   // little-endian read of the network-order bytes, like the core
  return v;
}
void EspClass::restart() {
  printf("[emu] ESP.restart()\n");
  fflush(stdout);
  _exit(100 + ESP_RST_SW);
}
int esp_efuse_mac_get_default(uint8_t* mac) { memcpy(mac, g_emu.mac, 6); return 0; }
const esp_partition_t* esp_ota_get_running_partition(void) { static esp_partition_t p = {"ota_0"}; return &p; }

// ---- IPAddress -------------------------------------------------------------------------------------
bool IPAddress::fromString(const char* s) {
  unsigned a, b, c, d;
  if (!s || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
  if (a > 255 || b > 255 || c > 255 || d > 255) return false;
  _a.bytes[0] = a; _a.bytes[1] = b; _a.bytes[2] = c; _a.bytes[3] = d;
  return true;
}
String IPAddress::toString() const {
  char b[20]; snprintf(b, sizeof(b), "%u.%u.%u.%u", _a.bytes[0], _a.bytes[1], _a.bytes[2], _a.bytes[3]);
  return String(b);
}
size_t IPAddress::printTo(Print& p) const { return p.print(toString().c_str()); }

// ---- base64 (only reached by WebServer auth paths the gateway does not use) -------------------------
#include "base64.h"
#include "libb64/cdecode.h"
String base64::encode(const uint8_t* data, size_t len) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < len ? (uint32_t)data[i+1] << 8 : 0) | (i + 2 < len ? data[i+2] : 0);
    out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
    out += (i + 1 < len) ? T[(v >> 6) & 63] : '=';
    out += (i + 2 < len) ? T[v & 63] : '=';
  }
  return out;
}
int base64_decode_chars(const char* in, int inLen, char* out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A'; if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52; if (c == '+') return 62; if (c == '/') return 63; return -1; };
  int n = 0; uint32_t acc = 0; int bits = 0;
  for (int i = 0; i < inLen; i++) { int v = val(in[i]); if (v < 0) continue; acc = (acc << 6) | (uint32_t)v; bits += 6; if (bits >= 8) { bits -= 8; out[n++] = (char)((acc >> bits) & 0xFF); } }
  out[n] = 0; return n;
}

// itoa/utoa live in the ESP32 ROM; natively we provide them on top of the vendored ltoa/ultoa.
extern "C" char* itoa(int v, char* s, int radix) { return ltoa(v, s, radix); }
extern "C" char* utoa(unsigned int v, char* s, int radix) { return ultoa(v, s, radix); }
