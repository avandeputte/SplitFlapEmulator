// emu.cpp -- one emulated split-flap module: the ATtiny1616 board, its 28BYJ-48 stepper,
// the Hall home sensor, the reel of flaps, and its RS-485 transceiver.
//
// The firmware sketch (vendor/firmware) is compiled into this same process, unmodified, and
// drives the "hardware" through the Arduino API declared in Arduino.h. This file is that
// hardware:
//
//   * time         -- millis()/delay() run on the emulator's virtual clock (common/vclock.h)
//   * RS-485       -- SoftwareSerial is a client of the bus hub (common/buslink.h): every byte
//                     the sketch writes occupies its wire time and blocks the CPU (interrupts
//                     off, exactly like the bit-banged original), every byte on the wire lands
//                     in a 128-byte ring that overflows when the sketch is busy
//   * stepper      -- the four coil pins are decoded as the 8-phase half-step sequence; each
//                     phase change turns the reel by one half-step (or slips, if a fault says so)
//   * Hall sensor  -- active for `magnetWidth` steps around the reel's home edge (position 0),
//                     wired active-LOW or active-HIGH, with the failure modes the 'T' self-test
//                     was written to detect
//   * EEPROM       -- 256 bytes, persisted to a file so the module keeps its ID and calibration
//                     across power cycles; a write costs the real part's ~3.5 ms
//   * SIGROW       -- the factory serial number
//   * ADC          -- the supply-voltage measurement the 'Q' diagnostics report
//   * watchdog     -- 2 s; if the sketch stops petting it the process "resets" (exit code carries
//                     the cause; the supervisor reboots it with RSTFR = WDRF)
//
// What the reel is physically showing is a function of its angle and of where flap 0 really
// sits relative to the magnet (`trueOffset`) -- NOT of what the firmware believes. So a wrong
// home offset in EEPROM shows the wrong (or a half-turned) flap, exactly as on the wall, and
// the gateway's calibration wizard has something real to calibrate.
#define SFEMU_NATIVE_INT 1
#include "Arduino.h"
#include "../../common/vclock.h"
#include "../../common/buslink.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <deque>
#include <random>
#include <csignal>
#include <fstream>
#include <sstream>

using namespace sfemu;

// ---- firmware globals we peek at for the control panel (defined in the sketch) -------------
// (the sketch's `int` is a 16-bit short, see Arduino.h)
extern uint8_t moduleId;
extern short   currentFlapIndex;
extern long    currentStepPos;
extern short   stepsFromHallToZero;
extern short   totalStepsPerRev;
extern short   parseState;
extern unsigned long nextAdvertiseTime;
extern unsigned long suppressAdvUntil;
extern unsigned long lastSerialTime;

// ---- the physical module ----------------------------------------------------------------------
struct Mech {
  int    stepsPerRev  = 4096;   // true half-steps per revolution of THIS reel
  int    trueOffset   = 2832;   // half-steps from the Hall edge to flap 0 dead centre
  int    magnetWidth  = 160;    // half-steps the sensor stays active per revolution
  bool   hallActiveLow= true;   // sensor pulls LOW at the magnet (A3144 style)
  int    flapCount    = 64;     // flaps printed on the reel
  std::string chars   = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw";
  double slipProb     = 0.0;    // probability that a half-step is missed (drag, weak supply)
  std::string hall    = "ok";   // ok | stuck_active | stuck_inactive | noisy | inverted
  std::string motor   = "ok";   // ok | dead
  int    vccMv        = 4980;   // supply voltage the ADC reports
  long   pos          = 0;      // reel angle in half-steps, 0 = Hall edge
};

static Mech        g_mech;
static std::mutex  g_mechMutex;
static std::mt19937 g_rng(12345);

static std::string g_serialHex;      // 20 hex chars
static uint8_t     g_sigrow[10];
static std::string g_dataDir;
static std::string g_eepromPath;
static BusLink     g_bus;
static uint32_t    g_baud = 9600;
static std::atomic<bool> g_stateDirty{true};
static std::atomic<bool> g_moving{false};
static int64_t     g_lastMoveUs = 0;
static int         g_bootCount = 0;
static uint8_t     g_resetCause = RSTCTRL_PORF_bm;

// Pins: 9,8,7,6 = IN1..IN4; 4 = Hall; 2 = DE; 3 = RX; 1 = TX
static uint8_t g_pinLevel[32] = {0};
static int     g_lastPhase = -1;      // last coil pattern index in the half-step table, -1 = released
static std::atomic<bool> g_coilsOn{false};
static std::atomic<bool> g_coilsDirty{false};   // pins changed since the rotor last "saw" the pattern

RSTCTRL_t RSTCTRL;
VREF_t    VREF;
ADC_t     ADC0;
EEPROMClass EEPROM;

static const uint8_t HALF_STEP[8][4] = {
  {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
  {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
};

static int phaseOf(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  for (int i = 0; i < 8; i++)
    if (HALF_STEP[i][0] == a && HALF_STEP[i][1] == b && HALF_STEP[i][2] == c && HALF_STEP[i][3] == d) return i;
  return -1;  // all off (released) or an invalid pattern
}

// The rotor responds to the complete coil pattern, not to each pin write: the sketch writes
// the four pins back to back, so the pattern is evaluated lazily -- at the next delay() (every
// half-step ends in one), time read, pin read or housekeeping tick.
static void coilsChanged() {
  if (!g_coilsDirty.exchange(false)) return;
  int ph = phaseOf(g_pinLevel[9], g_pinLevel[8], g_pinLevel[7], g_pinLevel[6]);
  bool on = ph >= 0;
  g_coilsOn = on;
  if (!on) {
    if (getenv("SFEMU_DEBUG")) { std::lock_guard<std::mutex> g(g_mechMutex); fprintf(stderr, "[emu] motor released at pos %ld (hall %s)\n", g_mech.pos, g_mech.pos < g_mech.magnetWidth ? "active" : "inactive"); }
    // Released: the rotor rests at its last detent; g_lastPhase keeps it so the next pattern
    // moves relative to where the rotor actually is.
    g_stateDirty = true; return;
  }
  if (g_lastPhase < 0) { g_lastPhase = ph; g_stateDirty = true; return; }   // first energisation ever
  int delta = (ph - g_lastPhase + 8) % 8;
  g_lastPhase = ph;
  // The sketch walks the table in reverse (--phase) to move the reel forward.
  long move = 0;
  if (delta == 7) move = 1;       // forward one half-step
  else if (delta == 6) move = 2;  // forward two (a skipped phase still drags the rotor)
  else if (delta == 1) move = -1;
  else if (delta == 2) move = -2;
  else move = 0;                  // opposite phase: the rotor stalls
  std::lock_guard<std::mutex> g(g_mechMutex);
  if (g_mech.motor == "dead") return;
  if (move && g_mech.slipProb > 0) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    if (u(g_rng) < g_mech.slipProb) move = 0;
  }
  if (move) {
    long rev = g_mech.stepsPerRev;
    g_mech.pos = ((g_mech.pos + move) % rev + rev) % rev;
    g_lastMoveUs = vclock().now_us();
    g_moving = true;
    g_stateDirty = true;
  }
}

// Raw Hall pin level for the current reel angle and fault mode.
static int hallPinLevel() {
  std::lock_guard<std::mutex> g(g_mechMutex);
  long pos = g_mech.pos, rev = g_mech.stepsPerRev;
  bool active = pos < g_mech.magnetWidth;
  if (g_mech.hall == "noisy")           active = active || (pos >= rev / 2 && pos < rev / 2 + g_mech.magnetWidth / 4);
  else if (g_mech.hall == "stuck_active")   active = true;
  else if (g_mech.hall == "stuck_inactive") active = false;
  else if (g_mech.hall == "inverted")       active = !active;
  bool low = g_mech.hallActiveLow ? active : !active;
  return low ? LOW : HIGH;
}

// ---- Arduino API -----------------------------------------------------------------------------
void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t pin, uint8_t val) {
  if (pin >= 32) return;
  uint8_t v = val ? 1 : 0;
  if (g_pinLevel[pin] == v) return;
  g_pinLevel[pin] = v;
  if (pin == 9 || pin == 8 || pin == 7 || pin == 6) g_coilsDirty = true;
}
int digitalRead(uint8_t pin) {
  coilsChanged();
  if (pin == 4) return hallPinLevel();
  return pin < 32 ? g_pinLevel[pin] : LOW;
}

static int64_t g_bootUs = 0;
unsigned long millis() { coilsChanged(); return (unsigned long)((vclock().now_us() - g_bootUs) / 1000); }
unsigned long micros() { coilsChanged(); return (unsigned long)(vclock().now_us() - g_bootUs); }
void delay(unsigned long ms) { coilsChanged(); vclock().sleep_us((int64_t)ms * 1000); }
void delayMicroseconds(unsigned int us) { coilsChanged(); vclock().sleep_us(us); }

uint8_t emu_mmio_byte(uint16_t addr) {
  if (addr >= 0x1103 && addr < 0x1103 + 10) return g_sigrow[addr - 0x1103];
  return 0;
}

AdcResReg::operator uint16_t() const {
  int vcc;
  { std::lock_guard<std::mutex> g(g_mechMutex); vcc = g_mech.vccMv; }
  std::uniform_int_distribution<int> jitter(-8, 8);
  vcc += jitter(g_rng);
  if (vcc < 1200) vcc = 1200;
  return (uint16_t)((1024UL * 1100UL) / (unsigned long)vcc);
}

// ---- EEPROM ------------------------------------------------------------------------------------
static uint8_t g_eeprom[256];
static std::mutex g_eeMutex;
static int64_t g_eeBusyUntil = 0;
static const int64_t EEPROM_WRITE_US = 3500;   // tinyAVR-1 EEPROM erase-write, per byte

static void eepromSave() {
  std::string tmp = g_eepromPath + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return;
  fwrite(g_eeprom, 1, sizeof(g_eeprom), f);
  fclose(f);
  rename(tmp.c_str(), g_eepromPath.c_str());
}
static void eepromLoad() {
  memset(g_eeprom, 0xFF, sizeof(g_eeprom));   // a blank chip reads 0xFF everywhere
  FILE* f = fopen(g_eepromPath.c_str(), "rb");
  if (f) { size_t n = fread(g_eeprom, 1, sizeof(g_eeprom), f); (void)n; fclose(f); }
}
// An NVM access while the previous write is still in flight halts the CPU until it completes.
static void eepromStall() {
  int64_t now = vclock().now_us();
  if (now < g_eeBusyUntil) vclock().sleep_until_us(g_eeBusyUntil);
}
uint8_t EEPROMClass::read(int addr) {
  if (addr < 0 || addr >= 256) return 0xFF;
  eepromStall();
  std::lock_guard<std::mutex> g(g_eeMutex);
  return g_eeprom[addr];
}
void EEPROMClass::write(int addr, uint8_t val) {
  if (addr < 0 || addr >= 256) return;
  eepromStall();
  {
    std::lock_guard<std::mutex> g(g_eeMutex);
    g_eeprom[addr] = val;
    eepromSave();
  }
  g_eeBusyUntil = vclock().now_us() + EEPROM_WRITE_US;
  g_stateDirty = true;
}

// ---- watchdog ----------------------------------------------------------------------------------
static std::atomic<bool>    g_wdtOn{false};
static std::atomic<int64_t> g_wdtPetUs{0};
static int64_t              g_wdtPeriodUs = 2000000;
void wdt_enable(uint8_t t) {
  static const int64_t P[] = {15000,30000,60000,120000,250000,500000,1000000,2000000,4000000,8000000};
  g_wdtPeriodUs = (t < 10) ? P[t] : 2000000;
  g_wdtPetUs = vclock().now_us();
  g_wdtOn = true;
}
void wdt_reset()   { g_wdtPetUs = vclock().now_us(); }
void wdt_disable() { g_wdtOn = false; }

// ---- exit / reset -------------------------------------------------------------------------------
// The process exit code tells the supervisor why the chip "reset" (100 + RSTFR bits) so the
// next boot sees the right cause in RSTCTRL.RSTFR. 0 = powered off (SIGTERM).
[[noreturn]] static void chipReset(uint8_t cause, const char* why) {
  fprintf(stderr, "[emu] reset (%s)\n", why);
  fflush(stdout);
  _exit(100 + cause);
}

// ---- SoftwareSerial: RX ring fed from the bus, TX to the bus -----------------------------------
struct PendingByte { int64_t end_us; uint8_t b; bool corrupt; };
static std::mutex              g_rxMutex;
static std::deque<PendingByte> g_rxPending;          // on the wire, not yet fully received
static uint8_t                 g_rxRing[_SS_MAX_RX_BUFF];
static volatile uint8_t        g_rxHead = 0, g_rxTail = 0;
static bool                    g_rxOverflow = false;
static std::atomic<int64_t>    g_txCliFrom{0}, g_txCliUntil{0};   // interrupts-off window while transmitting
static std::atomic<int64_t>    g_lastTxEndUs{0};
static std::atomic<int64_t>    g_lastRxUs{0};

// Move every byte whose stop bit has passed into the ring (the "RX interrupt").
static void rxPump() {
  int64_t now = vclock().now_us();
  std::lock_guard<std::mutex> g(g_rxMutex);
  while (!g_rxPending.empty() && g_rxPending.front().end_us <= now) {
    PendingByte pb = g_rxPending.front();
    g_rxPending.pop_front();
    g_lastRxUs = pb.end_us;
    uint8_t next = (uint8_t)((g_rxTail + 1) % _SS_MAX_RX_BUFF);
    if (next != g_rxHead) { g_rxRing[g_rxTail] = pb.b; g_rxTail = next; }
    else g_rxOverflow = true;                 // ring full: the byte is lost, flag it
  }
}

static void onBusRx(int64_t end_us, uint32_t baud, const std::vector<uint8_t>& bytes) {
  double bt = BusLink::byte_us(baud);
  std::uniform_int_distribution<int> rnd(0, 255);
  std::lock_guard<std::mutex> g(g_rxMutex);
  for (size_t i = 0; i < bytes.size(); i++) {
    int64_t end = end_us + (int64_t)(i * bt);
    int64_t start = end - (int64_t)bt;
    // Interrupts are off while this module transmits: a byte whose start bit falls in that
    // window is never seen (that includes our own echo).
    if (start >= g_txCliFrom.load() - (int64_t)bt && start < g_txCliUntil.load()) continue;
    uint8_t b = bytes[i];
    if (baud != g_baud) {                    // wrong line speed: framing garbage
      if (rnd(g_rng) < 96) continue;         // often nothing frames at all
      b = (uint8_t)rnd(g_rng);
    }
    g_rxPending.push_back({end, b, false});
  }
}

SoftwareSerial::SoftwareSerial(uint8_t, uint8_t, bool) {}
void SoftwareSerial::begin(long speed) { g_baud = (uint32_t)speed; }
bool SoftwareSerial::overflow() {
  std::lock_guard<std::mutex> g(g_rxMutex);
  bool r = g_rxOverflow; g_rxOverflow = false; return r;
}
int SoftwareSerial::available() {
  rxPump();
  std::lock_guard<std::mutex> g(g_rxMutex);
  return (int)((uint8_t)(g_rxTail + _SS_MAX_RX_BUFF - g_rxHead) % _SS_MAX_RX_BUFF);
}
int SoftwareSerial::peek() {
  rxPump();
  std::lock_guard<std::mutex> g(g_rxMutex);
  if (g_rxHead == g_rxTail) return -1;
  return g_rxRing[g_rxHead];
}
int SoftwareSerial::read() {
  rxPump();
  std::lock_guard<std::mutex> g(g_rxMutex);
  if (g_rxHead == g_rxTail) return -1;
  uint8_t d = g_rxRing[g_rxHead];
  g_rxHead = (uint8_t)((g_rxHead + 1) % _SS_MAX_RX_BUFF);
  return d;
}
// One byte, bit-banged: occupies one byte time on the wire and blocks the CPU meanwhile.
size_t SoftwareSerial::write(uint8_t b) {
  int64_t bt = (int64_t)BusLink::byte_us(g_baud);
  int64_t now = vclock().now_us();
  int64_t last = g_lastTxEndUs.load();
  // Bytes the sketch writes back to back are contiguous on the wire; a late wake-up of this
  // process (sleep overshoot) must not open a gap the real bit-banger never has.
  int64_t start = (now > last + 400) ? now : last;
  g_txCliFrom = start; g_txCliUntil = start + bt;
  if (getenv("SFEMU_DEBUG")) fprintf(stderr, "[emu] tx byte 0x%02x at %lld\n", b, (long long)start);
  g_lastTxEndUs = start + bt;
  g_bus.tx(start, g_baud, &b, 1);
  vclock().sleep_until_us(start + bt);
  return 1;
}

// ---- state reporting ------------------------------------------------------------------------
static std::string jsonEscape(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else if (c >= 0x80) { o += (char)(0xC0 | (c >> 6)); o += (char)(0x80 | (c & 0x3F)); }   // Latin-1 -> UTF-8
    else o += (char)c;
  }
  return o;
}

static void sendState() {
  Mech m;
  { std::lock_guard<std::mutex> g(g_mechMutex); m = g_mech; }
  int id, fi, ps; long sp; int off, tot;
  { std::lock_guard<std::mutex> g(g_eeMutex); id = g_eeprom[5]; }
  fi = currentFlapIndex; ps = parseState; sp = currentStepPos; off = stepsFromHallToZero; tot = totalStepsPerRev;
  (void)id;
  std::ostringstream o;
  o << "{\"sn\":\"" << g_serialHex << "\""
    << ",\"id\":" << (int)moduleId
    << ",\"pos\":" << m.pos
    << ",\"rev\":" << m.stepsPerRev
    << ",\"off\":" << m.trueOffset
    << ",\"flaps\":" << m.flapCount
    << ",\"charsHex\":\"" << to_hex((const uint8_t*)m.chars.data(), m.chars.size()) << "\""
    << ",\"coils\":" << (g_coilsOn ? "true" : "false")
    << ",\"hall\":" << (hallPinLevel() == (m.hallActiveLow ? LOW : HIGH) ? "true" : "false")
    << ",\"moving\":" << (g_moving ? "true" : "false")
    << ",\"up\":" << millis()
    << ",\"boot\":" << g_bootCount
    << ",\"cause\":" << (int)g_resetCause
    << ",\"fw\":{\"idx\":" << fi << ",\"step\":" << sp << ",\"off\":" << off << ",\"rev\":" << tot
    << ",\"parse\":" << ps << ",\"wdt\":" << (g_wdtOn ? "true" : "false")
    << ",\"ms\":" << millis() << ",\"nextAdv\":" << nextAdvertiseTime << ",\"advSuppress\":" << suppressAdvUntil << "}"
    << ",\"fault\":{\"hall\":\"" << m.hall << "\",\"motor\":\"" << m.motor << "\",\"slip\":" << m.slipProb << "}"
    << "}";
  g_bus.state(o.str());
}

// ---- control messages from the emulator -----------------------------------------------------
// Tiny JSON field extraction (the messages are flat objects the emulator itself composes).
static bool jsonNum(const std::string& j, const char* key, double& out) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = j.find(k); if (p == std::string::npos) return false;
  p += k.size();
  const char* s = j.c_str() + p; char* e = nullptr;
  double v = strtod(s, &e);
  if (e == s) return false;
  out = v; return true;
}
static bool jsonStr(const std::string& j, const char* key, std::string& out) {
  std::string k = std::string("\"") + key + "\":\"";
  size_t p = j.find(k); if (p == std::string::npos) return false;
  p += k.size();
  std::string o;
  while (p < j.size() && j[p] != '"') {
    if (j[p] == '\\' && p + 1 < j.size()) { p++; o += j[p]; }
    else o += j[p];
    p++;
  }
  out = o; return true;
}
static bool jsonBool(const std::string& j, const char* key, bool& out) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = j.find(k); if (p == std::string::npos) return false;
  p += k.size();
  if (j.compare(p, 4, "true") == 0) { out = true; return true; }
  if (j.compare(p, 5, "false") == 0) { out = false; return true; }
  return false;
}

static void applyMech(const std::string& j) {
  std::lock_guard<std::mutex> g(g_mechMutex);
  double d; std::string s; bool b;
  if (jsonNum(j, "rev", d) && d >= 500 && d <= 8000) {
    g_mech.stepsPerRev = (int)d;
    if (g_mech.pos >= g_mech.stepsPerRev) g_mech.pos %= g_mech.stepsPerRev;
  }
  if (jsonNum(j, "offset", d))  g_mech.trueOffset  = ((int)d % g_mech.stepsPerRev + g_mech.stepsPerRev) % g_mech.stepsPerRev;
  if (jsonNum(j, "magnet", d))  g_mech.magnetWidth = (int)d;
  if (jsonBool(j, "hallLow", b)) g_mech.hallActiveLow = b;
  if (jsonNum(j, "flaps", d) && d >= 1 && d <= 64) g_mech.flapCount = (int)d;
  if (jsonStr(j, "charsHex", s)) { auto v = from_hex(s); g_mech.chars.assign(v.begin(), v.end()); }
  if (jsonNum(j, "slip", d))    g_mech.slipProb = d < 0 ? 0 : (d > 1 ? 1 : d);
  if (jsonStr(j, "hall", s))    g_mech.hall  = s;
  if (jsonStr(j, "motor", s))   g_mech.motor = s;
  if (jsonNum(j, "vcc", d))     g_mech.vccMv = (int)d;
  if (jsonNum(j, "pos", d))     g_mech.pos = ((long)d % g_mech.stepsPerRev + g_mech.stepsPerRev) % g_mech.stepsPerRev;
  g_stateDirty = true;
}

static void onCtl(const std::string& j) {
  std::string op;
  if (!jsonStr(j, "op", op)) return;
  if (op == "mech") { applyMech(j); return; }
  if (op == "reset") chipReset(RSTCTRL_EXTRF_bm, "external reset");
  if (op == "brownout") {
    bool corrupt = false; jsonBool(j, "corrupt", corrupt);
    if (corrupt) {
      // The classic BOD-off failure: a random EEPROM byte comes back wrong after a power dip.
      std::lock_guard<std::mutex> g(g_eeMutex);
      std::uniform_int_distribution<int> a(0, 255), v(0, 255);
      int addr = a(g_rng); g_eeprom[addr] = (uint8_t)v(g_rng);
      eepromSave();
      fprintf(stderr, "[emu] brown-out corrupted EEPROM byte 0x%02X\n", addr);
    }
    chipReset(RSTCTRL_BORF_bm, "brown-out");
  }
  if (op == "eeprom") {
    // Reflash without "EEPROM retained": the chip comes back blank.
    { std::lock_guard<std::mutex> g(g_eeMutex); memset(g_eeprom, 0xFF, sizeof(g_eeprom)); eepromSave(); }
    chipReset(RSTCTRL_PORF_bm, "EEPROM erased");
  }
  if (op == "state") { g_stateDirty = true; return; }
}

// ---- housekeeping thread: RX interrupt, watchdog, state reports -------------------------------
static void housekeeping() {
  int64_t lastState = 0;
  for (;;) {
    rxPump();
    coilsChanged();
    int64_t now = vclock().now_us();
    if (g_wdtOn && now - g_wdtPetUs.load() > g_wdtPeriodUs) chipReset(RSTCTRL_WDRF_bm, "watchdog");
    if (g_moving && now - g_lastMoveUs > 60000) { g_moving = false; g_stateDirty = true; }
    // While moving, report at ~25 Hz (virtual); otherwise only on a change.
    bool due = (g_stateDirty && (now - lastState > (g_moving ? 40000 : 5000))) || (now - lastState > 2000000);
    if (due) { g_stateDirty = false; lastState = now; sendState(); }
    if (!g_bus.connected()) { fprintf(stderr, "[emu] bus hub gone -- powering off\n"); fflush(stdout); _exit(0); }
    std::this_thread::sleep_for(std::chrono::microseconds(1000));
  }
}

static void onTerm(int sig) {
  fprintf(stderr, "[emu] terminated by signal %d\n", sig);
  fflush(stdout);
  _exit(0);
}

int main(int argc, char** argv) {
  std::string busPath = "/tmp/sfemu-bus.sock";
  g_dataDir = ".";
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
    if (a == "--serial") next(g_serialHex);
    else if (a == "--data") next(g_dataDir);
    else if (a == "--bus") next(busPath);
    else if (a == "--cause") { std::string s; next(s); g_resetCause = (uint8_t)atoi(s.c_str()); }
    else if (a == "--boot") { std::string s; next(s); g_bootCount = atoi(s.c_str()); }
    else if (a == "--mech") { std::string s; next(s); applyMech(s); }   // the physical truth, before boot
  }
  if (g_serialHex.size() != 20) { fprintf(stderr, "usage: sfmodule --serial <20 hex> --data <dir> --bus <socket> [--cause n]\n"); return 2; }
  for (int i = 0; i < 10; i++) g_sigrow[i] = (uint8_t)strtoul(g_serialHex.substr(2 * i, 2).c_str(), nullptr, 16);
  { std::seed_seq seq(g_sigrow, g_sigrow + 10); g_rng.seed(seq); }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  signal(SIGTERM, onTerm);
  signal(SIGINT, onTerm);
  signal(SIGPIPE, SIG_IGN);

  g_eepromPath = g_dataDir + "/" + g_serialHex + ".eeprom";
  eepromLoad();

  g_bus.on_rx(onBusRx);
  g_bus.on_ctl(onCtl);
  if (!g_bus.connect(busPath, "module", g_serialHex)) { fprintf(stderr, "[emu] cannot reach bus hub at %s\n", busPath.c_str()); return 3; }
  g_bus.wait_time(3000);
  // Give the hub a moment to deliver the mechanical configuration before the sketch boots.
  usleep(50000);

  g_bootUs = vclock().now_us();
  RSTCTRL.RSTFR = g_resetCause;
  std::thread(housekeeping).detach();
  printf("[emu] module %s booting (cause 0x%02X)\n", g_serialHex.c_str(), g_resetCause);

  setup();
  for (;;) {
    loop();
    // The real chip spins; here we yield briefly when nothing is pending so 250 modules do
    // not burn 250 cores. Virtual time keeps flowing regardless.
    bool pending;
    { std::lock_guard<std::mutex> g(g_rxMutex); pending = g_rxHead != g_rxTail || !g_rxPending.empty(); }
    if (!pending) std::this_thread::sleep_for(std::chrono::microseconds(1000));
  }
}
