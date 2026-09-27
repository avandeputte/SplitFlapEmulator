// Wire.h -- the I2C bus with one device on it: the PCF85063 real-time clock.
#pragma once
#include "Arduino.h"
class TwoWire : public Stream {
public:
  bool begin(int sda = -1, int scl = -1, uint32_t freq = 0);
  void beginTransmission(uint8_t addr);
  uint8_t endTransmission(bool stop = true);
  size_t requestFrom(uint8_t addr, size_t len, bool stop = true);
  uint8_t requestFrom(uint8_t addr, uint8_t len) { return (uint8_t)requestFrom(addr, (size_t)len, true); }
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* b, size_t n) override { size_t k = 0; while (n--) k += write(*b++); return k; }
  using Print::write;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override {}
private:
  uint8_t _addr = 0; uint8_t _txbuf[16]; int _txlen = 0;
  uint8_t _rxbuf[32]; int _rxlen = 0, _rxpos = 0;
};
extern TwoWire Wire;
