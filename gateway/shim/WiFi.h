// WiFi.h -- the station is emulated as always able to associate: the gateway "connects" a
// moment after WiFi.begin() (or, with no credentials configured, is simply on the network the
// container sits on). RSSI and IP are reported from the emulator's configuration.
#pragma once
#include "Arduino.h"
#include "Network.h"
typedef NetworkClient WiFiClient;
typedef NetworkServer WiFiServer;
typedef enum { WL_NO_SHIELD = 255, WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL, WL_SCAN_COMPLETED, WL_CONNECTED, WL_CONNECT_FAILED, WL_CONNECTION_LOST, WL_DISCONNECTED } wl_status_t;
typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
#define WIFI_OFF    WIFI_MODE_NULL
#define WIFI_STA    WIFI_MODE_STA
#define WIFI_AP     WIFI_MODE_AP
#define WIFI_AP_STA WIFI_MODE_APSTA
class WiFiClass {
public:
  bool mode(wifi_mode_t m);
  wl_status_t status();
  wl_status_t begin(const char* ssid, const char* pass = NULL);
  bool disconnect(bool wifioff = false, bool eraseap = false);
  bool setSleep(bool on) { return true; }
  bool softAP(const char* ssid, const char* pass = NULL, int ch = 1, int hidden = 0, int maxc = 4);
  bool softAPdisconnect(bool wifioff = false);
  IPAddress softAPIP();
  IPAddress localIP();
  int8_t RSSI();
  String SSID();
  String macAddress();
  bool setHostname(const char*) { return true; }
};
extern WiFiClass WiFi;
