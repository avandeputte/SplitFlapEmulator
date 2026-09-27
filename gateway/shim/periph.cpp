// periph.cpp -- the RS-485 UART (bus link), the PCF85063 RTC on I2C, WiFi, Update, OTA, mDNS.
#include "Arduino.h"
#include "Wire.h"
#include "WiFi.h"
#include "Update.h"
#include "ArduinoOTA.h"
#include "ESPmDNS.h"
#include "emu.h"
#include <mutex>
#include <deque>
#include <vector>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/stat.h>
#include <ArduinoJson.h>

TwoWire Wire;
WiFiClass WiFi;
UpdateClass Update;
ArduinoOTAClass ArduinoOTA;
MDNSResponder MDNS;

// ---- RS-485 UART -------------------------------------------------------------------------------
struct PendingByte { int64_t end_us; uint8_t b; };
static std::mutex              g_rxMutex;
static std::deque<PendingByte> g_rxPending;
static std::deque<uint8_t>     g_rxRing;
static size_t                  g_rxRingSize = 256;
static size_t                  g_rxDropped = 0;
static int64_t                 g_lastTxEndUs = 0;
static uint32_t                g_baud = 9600;

void emuBusRx(int64_t end_us, uint32_t baud, const std::vector<uint8_t>& bytes) {
  double bt = sfemu::BusLink::byte_us(baud);
  std::lock_guard<std::mutex> g(g_rxMutex);
  for (size_t i = 0; i < bytes.size(); i++) {
    uint8_t b = bytes[i];
    if (baud != g_baud) {                          // framing errors at the wrong line speed
      if ((rand() & 0xFF) < 200) continue;
      b = (uint8_t)rand();
    }
    g_rxPending.push_back({end_us + (int64_t)(i * bt), b});
  }
}
static void rxPump() {
  int64_t now = sfemu::vclock().now_us();
  std::lock_guard<std::mutex> g(g_rxMutex);
  while (!g_rxPending.empty() && g_rxPending.front().end_us <= now) {
    if (g_rxRing.size() < g_rxRingSize) g_rxRing.push_back(g_rxPending.front().b);
    else g_rxDropped++;                           // driver ring full: bytes are silently lost
    g_rxPending.pop_front();
  }
}
size_t HardwareSerial::setRxBufferSize(size_t n) { g_rxRingSize = n; return n; }
void HardwareSerial::begin(unsigned long baud, uint32_t, int8_t, int8_t, bool, unsigned long, uint8_t) { _baud = baud; g_baud = (uint32_t)baud; }
void HardwareSerial::end() {}
bool HardwareSerial::setPins(int8_t, int8_t, int8_t, int8_t) { return true; }
bool HardwareSerial::setMode(SerialMode) { return true; }
int HardwareSerial::available() { rxPump(); std::lock_guard<std::mutex> g(g_rxMutex); return (int)g_rxRing.size(); }
int HardwareSerial::peek() { rxPump(); std::lock_guard<std::mutex> g(g_rxMutex); return g_rxRing.empty() ? -1 : g_rxRing.front(); }
int HardwareSerial::read() { rxPump(); std::lock_guard<std::mutex> g(g_rxMutex); if (g_rxRing.empty()) return -1; uint8_t b = g_rxRing.front(); g_rxRing.pop_front(); return b; }
size_t HardwareSerial::read(uint8_t* buf, size_t n) { size_t k = 0; while (k < n) { int c = read(); if (c < 0) break; buf[k++] = (uint8_t)c; } return k; }
size_t HardwareSerial::readBytes(char* buf, size_t n) { return read((uint8_t*)buf, n); }
size_t HardwareSerial::write(uint8_t b) { return write(&b, 1); }
size_t HardwareSerial::write(const uint8_t* b, size_t n) {
  if (!n) return 0;
  int64_t bt = (int64_t)sfemu::BusLink::byte_us(g_baud);
  int64_t now = sfemu::vclock().now_us();
  int64_t start = (now > g_lastTxEndUs + 400) ? now : g_lastTxEndUs;   // queued behind what is still going out
  g_emu.bus.tx(start, g_baud, b, n);
  g_lastTxEndUs = start + bt * (int64_t)n;
  return n;
}
void HardwareSerial::flush() { sfemu::vclock().sleep_until_us(g_lastTxEndUs); }   // wait for the last stop bit
void HardwareSerial::flush(bool) { flush(); }

// ---- PCF85063 RTC on I2C ---------------------------------------------------------------------------
// The chip keeps counting while the gateway is off (battery backed): its time is anchored to the
// host wall clock in a small file. While the gateway runs, it counts virtual seconds.
static uint8_t  g_rtcReg[0x12] = {0};
static uint8_t  g_rtcPtr = 0;
static int64_t  g_rtcEpochBase = 946684800;   // 2000-01-01 00:00:00 on a fresh chip
static int64_t  g_rtcVirtBase = 0;
static bool     g_rtcLoaded = false;
static String rtcPath() { return String(g_emu.dataDir.c_str()) + "/rtc.json"; }
static int64_t rtcEpochNow() { return g_rtcEpochBase + (sfemu::vclock().now_us() - g_rtcVirtBase) / 1000000; }
static void rtcSave() {
  FILE* f = fopen(rtcPath().c_str(), "wb");
  if (!f) return;
  fprintf(f, "{\"epoch\":%lld,\"host\":%lld}\n", (long long)rtcEpochNow(), (long long)time(nullptr));
  fclose(f);
}
static void rtcLoad() {
  g_rtcLoaded = true;
  g_rtcVirtBase = sfemu::vclock().now_us();
  FILE* f = fopen(rtcPath().c_str(), "rb");
  if (!f) return;
  long long epoch = 0, host = 0;
  if (fscanf(f, "{\"epoch\":%lld,\"host\":%lld}", &epoch, &host) == 2 && epoch > 0)
    g_rtcEpochBase = epoch + (time(nullptr) - host);   // it kept ticking while we were off
  fclose(f);
}
static uint8_t bcd(int v) { return (uint8_t)((v / 10 * 16) + (v % 10)); }
static int unbcd(uint8_t v) { return (v / 16 * 10) + (v % 16); }
static void rtcFillTimeRegs() {
  time_t t = (time_t)rtcEpochNow();
  struct tm tm; gmtime_r(&t, &tm);
  g_rtcReg[4] = bcd(tm.tm_sec); g_rtcReg[5] = bcd(tm.tm_min); g_rtcReg[6] = bcd(tm.tm_hour);
  g_rtcReg[7] = bcd(tm.tm_mday); g_rtcReg[8] = bcd(tm.tm_wday); g_rtcReg[9] = bcd(tm.tm_mon + 1);
  g_rtcReg[10] = bcd(tm.tm_year - 100);
}
static void rtcApplyTimeRegs() {
  struct tm tm; memset(&tm, 0, sizeof(tm));
  tm.tm_sec = unbcd(g_rtcReg[4] & 0x7F); tm.tm_min = unbcd(g_rtcReg[5] & 0x7F); tm.tm_hour = unbcd(g_rtcReg[6] & 0x3F);
  tm.tm_mday = unbcd(g_rtcReg[7] & 0x3F); tm.tm_mon = unbcd(g_rtcReg[9] & 0x1F) - 1; tm.tm_year = unbcd(g_rtcReg[10]) + 100;
  time_t t = timegm(&tm);
  g_rtcEpochBase = (int64_t)t; g_rtcVirtBase = sfemu::vclock().now_us();
  rtcSave();
}
bool TwoWire::begin(int, int, uint32_t) { if (!g_rtcLoaded) rtcLoad(); return true; }
void TwoWire::beginTransmission(uint8_t addr) { _addr = addr; _txlen = 0; }
size_t TwoWire::write(uint8_t b) { if (_txlen < (int)sizeof(_txbuf)) _txbuf[_txlen++] = b; return 1; }
uint8_t TwoWire::endTransmission(bool) {
  if (_addr != 0x51) return 2;                          // nobody else answers on this bus
  if (_txlen >= 1) g_rtcPtr = _txbuf[0];
  if (_txlen > 1) {
    bool timeTouched = false;
    for (int i = 1; i < _txlen; i++) { uint8_t r = (uint8_t)(g_rtcPtr + i - 1); if (r < sizeof(g_rtcReg)) { g_rtcReg[r] = _txbuf[i]; if (r >= 4 && r <= 10) timeTouched = true; } }
    if (timeTouched) rtcApplyTimeRegs();
  }
  return 0;
}
size_t TwoWire::requestFrom(uint8_t addr, size_t len, bool) {
  _rxlen = 0; _rxpos = 0;
  if (addr != 0x51) return 0;
  rtcFillTimeRegs();
  for (size_t i = 0; i < len && i < sizeof(_rxbuf); i++) { uint8_t r = (uint8_t)(g_rtcPtr + i); _rxbuf[_rxlen++] = r < sizeof(g_rtcReg) ? g_rtcReg[r] : 0; }
  static int64_t lastSave = 0;
  int64_t now = sfemu::vclock().now_us();
  if (now - lastSave > 10000000) { lastSave = now; rtcSave(); }
  return (size_t)_rxlen;
}
int TwoWire::available() { return _rxlen - _rxpos; }
int TwoWire::read() { return _rxpos < _rxlen ? _rxbuf[_rxpos++] : -1; }
int TwoWire::peek() { return _rxpos < _rxlen ? _rxbuf[_rxpos] : -1; }

// ---- WiFi -----------------------------------------------------------------------------------------
static bool g_staBegun = false;
static unsigned long g_staBeginMs = 0;
static String g_ssid;
static bool g_apUp = false;
bool WiFiClass::mode(wifi_mode_t) { return true; }
wl_status_t WiFiClass::begin(const char* ssid, const char*) { g_ssid = ssid ? ssid : ""; g_staBegun = true; g_staBeginMs = millis(); return WL_DISCONNECTED; }
wl_status_t WiFiClass::status() {
  if (!g_emu.wifiUp) return WL_DISCONNECTED;
  // The emulated board is on a wired LAN as far as the network stack is concerned: it associates
  // a moment after begin(), and is simply up when no credentials were ever configured.
  unsigned long since = g_staBegun ? g_staBeginMs : 0;
  return (millis() - since >= 1500) ? WL_CONNECTED : WL_DISCONNECTED;
}
bool WiFiClass::disconnect(bool, bool) { g_staBegun = false; return true; }
bool WiFiClass::softAP(const char*, const char*, int, int, int) { g_apUp = true; return true; }
bool WiFiClass::softAPdisconnect(bool) { g_apUp = false; return true; }
IPAddress WiFiClass::softAPIP() { return IPAddress(192, 168, 4, 1); }
static String detectIp() {
  if (!g_emu.ip.empty()) return String(g_emu.ip.c_str());
  struct ifaddrs* ifa = nullptr;
  String best = "127.0.0.1";
  if (getifaddrs(&ifa) == 0) {
    for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
      if (p->ifa_flags & IFF_LOOPBACK) continue;
      char b[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &((struct sockaddr_in*)p->ifa_addr)->sin_addr, b, sizeof(b));
      best = b; break;
    }
    freeifaddrs(ifa);
  }
  return best;
}
IPAddress WiFiClass::localIP() { if (status() != WL_CONNECTED) return IPAddress(); IPAddress ip; ip.fromString(detectIp().c_str()); return ip; }
int8_t WiFiClass::RSSI() { return (int8_t)(g_emu.rssi.load() + (int)(millis() / 1000 % 5) - 2); }
String WiFiClass::SSID() { return g_ssid; }
String WiFiClass::macAddress() { char b[20]; snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", g_emu.mac[0], g_emu.mac[1], g_emu.mac[2], g_emu.mac[3], g_emu.mac[4], g_emu.mac[5]); return String(b); }

// ---- Update (browser OTA) ------------------------------------------------------------------------------
bool UpdateClass::begin(size_t, int, int, uint8_t, const char*) {
  String p = String(g_emu.dataDir.c_str()) + "/ota_upload.bin";
  _f = fopen(p.c_str(), "wb");
  _written = 0; _running = _f != nullptr; _finished = false; _error = _f ? 0 : 1;
  return _running;
}
size_t UpdateClass::write(uint8_t* data, size_t len) {
  if (!_running || !_f) return 0;
  if (_written == 0 && len > 0 && data[0] != 0xE9) { _error = 10; return 0; }   // not an ESP image: "Wrong Magic Byte"
  size_t n = fwrite(data, 1, len, _f);
  _written += n;
  // Flash absorbs about 90 KB/s on the real board; the upload can't go faster than that.
  sfemu::vclock().sleep_us((int64_t)(len * 1000000.0 / 90000.0));
  return n;
}
bool UpdateClass::end(bool) {
  if (_f) { fclose(_f); _f = nullptr; }
  _running = false;
  if (_error || _written == 0) { _finished = false; return false; }
  _finished = true;
  printf("[emu] OTA image received (%zu bytes) and kept as ota_upload.bin; the emulator reboots on its built-in firmware\n", _written);
  return true;
}
void UpdateClass::abort() { if (_f) { fclose(_f); _f = nullptr; } _running = false; _finished = false; if (!_error) _error = 8; }
const char* UpdateClass::errorString() {
  switch (_error) { case 0: return "No Error"; case 1: return "Flash Write Failed"; case 8: return "Aborted"; case 10: return "Wrong Magic Byte"; default: return "UNKNOWN"; }
}

// ---- OTA / mDNS ---------------------------------------------------------------------------------------------
void ArduinoOTAClass::begin() { printf("[emu] ArduinoOTA: hostname %s (push updates are not accepted by the emulator)\n", _host.c_str()); }
bool MDNSResponder::addService(const char* service, const char* proto, uint16_t port) { printf("[emu] mDNS: _%s._%s on port %u\n", service, proto, port); return true; }
