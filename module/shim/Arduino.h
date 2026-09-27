// Arduino.h -- the emulator's stand-in for megaTinyCore's Arduino core, for the
// SplitFlapUniversalFirmware sketch compiled natively. Only what the sketch uses.
//
// The sketch is compiled UNMODIFIED. Everything it touches on the ATtiny1616 -- pins, the
// clock, EEPROM, the signature row (serial number), the reset controller, the ADC (supply
// voltage), the watchdog and the bit-banged SoftwareSerial -- is implemented in emu.cpp on
// top of a mechanical model of the module and the emulated RS-485 bus.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define HIGH 1
#define LOW  0
#define INPUT        0
#define OUTPUT       1
#define INPUT_PULLUP 2

typedef bool    boolean;
typedef uint8_t byte;

// PROGMEM is a no-op natively: flash and RAM are the same address space here.
#define PROGMEM
#define PGM_P const char*
#define pgm_read_byte(p) (*(const uint8_t*)(p))
#define pgm_read_word(p) (*(const uint16_t*)(p))
#define F(x) (x)

void          pinMode(uint8_t pin, uint8_t mode);
void          digitalWrite(uint8_t pin, uint8_t val);
int           digitalRead(uint8_t pin);
unsigned long millis();
unsigned long micros();
void          delay(unsigned long ms);
void          delayMicroseconds(unsigned int us);

inline bool isDigit(int c)        { return c >= '0' && c <= '9'; }
inline bool isAlpha(int c)        { return isalpha(c) != 0; }
inline bool isAlphaNumeric(int c) { return isalnum(c) != 0; }
inline bool isSpace(int c)        { return isspace(c) != 0; }

#ifndef min
template <typename A, typename B> inline A min(A a, B b) { return a < (A)b ? a : (A)b; }
template <typename A, typename B> inline A max(A a, B b) { return a > (A)b ? a : (A)b; }
#endif
template <typename T> inline T constrain(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- ATtiny1616 memory-mapped registers the sketch reads or writes -----------------------
// The signature row (factory serial number) is read byte-wise through _MMIO_BYTE.
uint8_t emu_mmio_byte(uint16_t addr);
#define _MMIO_BYTE(addr) emu_mmio_byte((uint16_t)(addr))

// Reset controller: RSTFR latches why the chip last reset. The emulator's supervisor passes
// the cause of each boot (power-on, brown-out, external, watchdog, software).
struct RSTCTRL_t { uint8_t RSTFR; uint8_t SWRR; };
extern RSTCTRL_t RSTCTRL;
#define RSTCTRL_PORF_bm  0x01
#define RSTCTRL_BORF_bm  0x02
#define RSTCTRL_EXTRF_bm 0x04
#define RSTCTRL_WDRF_bm  0x08
#define RSTCTRL_SWRF_bm  0x10
#define RSTCTRL_UPDIRF_bm 0x20

// Voltage reference + ADC0: used only by readVccMillivolts(). A conversion completes
// instantly (COMMAND reads back 0) and RES yields the value a real part would produce for the
// module's modelled supply voltage.
struct VREF_t { uint8_t CTRLA; uint8_t CTRLB; };
extern VREF_t VREF;
#define VREF_ADC0REFSEL_gm     0x70
#define VREF_ADC0REFSEL_1V1_gc (0x01 << 4)
struct AdcCommandReg {
  uint8_t v = 0;
  operator uint8_t() const { return 0; }           // conversion already done
  AdcCommandReg& operator=(uint8_t x) { v = x; return *this; }
};
struct AdcResReg { operator uint16_t() const; };
struct ADC_t {
  uint8_t CTRLA, CTRLB, CTRLC, CTRLD, CTRLE, SAMPCTRL, MUXPOS;
  AdcCommandReg COMMAND;
  AdcResReg     RES;
};
extern ADC_t ADC0;
#define ADC_PRESC_DIV16_gc   (0x03 << 0)
#define ADC_REFSEL_VDDREF_gc (0x01 << 4)
#define ADC_SAMPCAP_bm       0x40
#define ADC_MUXPOS_INTREF_gc (0x1D << 0)
#define ADC_ENABLE_bm        0x01
#define ADC_STCONV_bm        0x01

// ---- Print: the subset of Arduino's Print that SoftwareSerial inherits ----------------------
// Note the Arduino semantics the replies depend on: print(char) writes the character, while
// print(unsigned char) / print(int) / print(long) write the decimal number.
class Print {
public:
  virtual size_t write(uint8_t b) = 0;
  size_t write(const char* s)                { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
  size_t write(const uint8_t* b, size_t n)   { size_t k = 0; while (n--) k += write(*b++); return k; }
  size_t print(const char* s)                { return write(s); }
  size_t print(char c)                       { return write((uint8_t)c); }
  size_t print(unsigned char v, int base=10) { return printULong(v, base); }
  size_t print(int v, int base=10)           { return printLong(v, base); }
  size_t print(unsigned int v, int base=10)  { return printULong(v, base); }
  size_t print(long v, int base=10)          { return printLong(v, base); }
  size_t print(unsigned long v, int base=10) { return printULong(v, base); }
  size_t println()                           { return write("\r\n"); }
  template <typename T> size_t println(T v)  { size_t n = print(v); return n + println(); }
private:
  size_t printULong(unsigned long v, int base) {
    char buf[8 * sizeof(long) + 1]; char* p = buf + sizeof(buf) - 1; *p = 0;
    if (base < 2) base = 10;
    do { unsigned d = v % base; v /= base; *--p = (char)(d < 10 ? '0' + d : 'A' + d - 10); } while (v);
    return write(p);
  }
  size_t printLong(long v, int base) {
    if (base == 10 && v < 0) { size_t n = write((uint8_t)'-'); return n + printULong((unsigned long)(-v), 10); }
    return printULong((unsigned long)v, base);
  }
};

// ---- SoftwareSerial (megaTinyCore library API) ---------------------------------------------
// Bit-banged, half-duplex: write() blocks for one byte time with interrupts off (so nothing
// can be received meanwhile), the receive side is interrupt-driven into a 128-byte ring that
// overflows silently when the sketch is busy. Implemented in emu.cpp against the bus.
#ifndef _SS_MAX_RX_BUFF
#define _SS_MAX_RX_BUFF 128
#endif
class SoftwareSerial : public Print {
public:
  SoftwareSerial(uint8_t rxPin, uint8_t txPin, bool inverse = false);
  void   begin(long speed);
  void   end() {}
  bool   listen() { return true; }
  bool   isListening() { return true; }
  bool   stopListening() { return true; }
  bool   overflow();
  int    peek();
  size_t write(uint8_t b) override;
  int    read();
  int    available();
  void   flush() {}
  operator bool() { return true; }
  using Print::write;
};

// ---- EEPROM (256 bytes on the ATtiny1616) ---------------------------------------------------
class EEPROMClass {
public:
  uint8_t read(int addr);
  void    write(int addr, uint8_t val);
  void    update(int addr, uint8_t val) { if (read(addr) != val) write(addr, val); }
  template <typename T> T& get(int addr, T& t) {
    uint8_t* p = (uint8_t*)&t;
    for (size_t i = 0; i < sizeof(T); i++) p[i] = read(addr + (int)i);
    return t;
  }
  template <typename T> const T& put(int addr, const T& t) {
    const uint8_t* p = (const uint8_t*)&t;
    for (size_t i = 0; i < sizeof(T); i++) update(addr + (int)i, p[i]);
    return t;
  }
  static constexpr int length() { return 256; }
};
extern EEPROMClass EEPROM;

// ---- Watchdog (avr/wdt.h) -------------------------------------------------------------------
#define WDTO_15MS 0
#define WDTO_30MS 1
#define WDTO_60MS 2
#define WDTO_120MS 3
#define WDTO_250MS 4
#define WDTO_500MS 5
#define WDTO_1S 6
#define WDTO_2S 7
#define WDTO_4S 8
#define WDTO_8S 9
void wdt_enable(uint8_t timeout);
void wdt_reset();
void wdt_disable();

void setup();
void loop();

// The ATtiny1616 is an 8-bit AVR: `int` is 16 bits there, and the sketch's EEPROM layout
// depends on it (an `int` field is two bytes; EEPROM.get/put and the sketch's own
// eepromUpdate<T> copy sizeof(T) bytes). Natively an int is four bytes, which would silently
// overwrite the neighbouring fields -- the module id and the auto-home flag sit right after the
// two int calibration values. So, for the sketch's translation unit only, `int` means `short`:
// exactly the size and overflow behaviour it has on the chip. Every declaration above was made
// with the native int first, so the shim's own ABI is unaffected.
#ifndef SFEMU_NATIVE_INT
#define int short
#endif
