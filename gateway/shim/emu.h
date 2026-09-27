// emu.h -- process-wide emulator state for the gateway shim.
#pragma once
#include <string>
#include <atomic>
#include "../../common/vclock.h"
#include "../../common/buslink.h"

struct GatewayEmu {
  std::string dataDir = "./data";
  std::string busPath = "/tmp/sfemu-bus.sock";
  std::string ip      = "";          // reported as WiFi.localIP(); auto-detected when empty
  uint8_t     mac[6]  = {0x48, 0x27, 0xE2, 0x5F, 0x1A, 0xC4};
  int         resetReason = 1;       // esp_reset_reason_t value for this boot
  int         httpPort = 80;         // where NetworkServer(80) really listens
  std::atomic<bool> wifiUp{true};    // emulator switch: pull the station down
  std::atomic<int>  rssi{-58};
  int64_t     bootUs = 0;
  sfemu::BusLink bus;
};
extern GatewayEmu g_emu;
