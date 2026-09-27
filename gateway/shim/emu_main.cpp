// main.cpp -- boots the gateway firmware: setup() once, then loop() forever, like the Arduino core.
#include "Arduino.h"
#include "emu.h"
#include <csignal>
#include <sys/stat.h>
#include <thread>

void emuBusRx(int64_t end_us, uint32_t baud, const std::vector<uint8_t>& bytes);

static void onTerm(int) { fflush(stdout); _exit(0); }

static void loadIdentity() {
  // A stable MAC per data volume (the board's eFuse), overridable with SFEMU_MAC.
  const char* env = getenv("SFEMU_MAC");
  unsigned m[6];
  if (env && sscanf(env, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
    for (int i = 0; i < 6; i++) g_emu.mac[i] = (uint8_t)m[i];
    return;
  }
  std::string p = g_emu.dataDir + "/identity";
  FILE* f = fopen(p.c_str(), "rb");
  if (f && fscanf(f, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
    for (int i = 0; i < 6; i++) g_emu.mac[i] = (uint8_t)m[i];
    fclose(f); return;
  }
  if (f) fclose(f);
  srand((unsigned)time(nullptr) ^ (unsigned)getpid());
  g_emu.mac[3] = (uint8_t)rand(); g_emu.mac[4] = (uint8_t)rand(); g_emu.mac[5] = (uint8_t)rand();
  f = fopen(p.c_str(), "wb");
  if (f) { fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x\n", g_emu.mac[0], g_emu.mac[1], g_emu.mac[2], g_emu.mac[3], g_emu.mac[4], g_emu.mac[5]); fclose(f); }
}

static void onCtl(const std::string& j) {
  if (j.find("\"op\":\"wifi\"") != std::string::npos) {
    g_emu.wifiUp = j.find("\"up\":true") != std::string::npos;
    printf("[emu] WiFi %s by the emulator\n", g_emu.wifiUp ? "restored" : "pulled down");
  } else if (j.find("\"op\":\"rssi\"") != std::string::npos) {
    size_t p = j.find("\"rssi\":"); if (p != std::string::npos) g_emu.rssi = atoi(j.c_str() + p + 7);
  } else if (j.find("\"op\":\"reboot\"") != std::string::npos) {
    fflush(stdout); _exit(100 + ESP_RST_EXT);
  }
}

int main(int argc, char** argv) {
  if (const char* e = getenv("SFEMU_DATA")) g_emu.dataDir = e;
  if (const char* e = getenv("SFEMU_BUS")) g_emu.busPath = e;
  if (const char* e = getenv("SFEMU_GATEWAY_IP")) g_emu.ip = e;
  if (const char* e = getenv("SFEMU_HTTP_PORT")) g_emu.httpPort = atoi(e);
  if (const char* e = getenv("SFEMU_RESET_REASON")) g_emu.resetReason = atoi(e);
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
    std::string v;
    if (a == "--data") next(g_emu.dataDir);
    else if (a == "--bus") next(g_emu.busPath);
    else if (a == "--ip") next(g_emu.ip);
    else if (a == "--port") { next(v); g_emu.httpPort = atoi(v.c_str()); }
    else if (a == "--reason") { next(v); g_emu.resetReason = atoi(v.c_str()); }
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  signal(SIGTERM, onTerm); signal(SIGINT, onTerm); signal(SIGPIPE, SIG_IGN);
  mkdir(g_emu.dataDir.c_str(), 0755);
  loadIdentity();

  g_emu.bus.on_rx(emuBusRx);
  g_emu.bus.on_ctl(onCtl);
  if (!g_emu.bus.connect(g_emu.busPath, "gateway", "gateway")) {
    fprintf(stderr, "[emu] cannot reach the bus hub at %s\n", g_emu.busPath.c_str());
    return 3;
  }
  g_emu.bus.wait_time(3000);
  g_emu.bootUs = sfemu::vclock().now_us();
  printf("[emu] gateway booting (reset reason %d, data %s)\n", g_emu.resetReason, g_emu.dataDir.c_str());

  std::thread([] {   // the hub going away is the power going away
    for (;;) { if (!g_emu.bus.connected()) { fprintf(stderr, "[emu] bus hub gone -- powering off\n"); fflush(stdout); _exit(0); } usleep(200000); }
  }).detach();

  setup();
  for (;;) loop();
}
