// HardwareSerial.h -- UART1 in RS-485 half-duplex mode, i.e. the gateway's bus transceiver.
// Bytes written go to the bus hub timestamped at their wire time; flush() blocks until the last
// stop bit has left, as the real driver does. Received bytes surface when their stop bit has
// passed, into a ring of the size the firmware asks for (setRxBufferSize) that drops on overflow.
#pragma once
#include "Arduino.h"

#define SERIAL_5N1 0x8000010
#define SERIAL_6N1 0x8000014
#define SERIAL_7N1 0x8000018
#define SERIAL_8N1 0x800001c
#define SERIAL_5N2 0x8000030
#define SERIAL_6N2 0x8000034
#define SERIAL_7N2 0x8000038
#define SERIAL_8N2 0x800003c
#define SERIAL_5E1 0x8000012
#define SERIAL_6E1 0x8000016
#define SERIAL_7E1 0x800001a
#define SERIAL_8E1 0x800001e
#define SERIAL_5E2 0x8000032
#define SERIAL_6E2 0x8000036
#define SERIAL_7E2 0x800003a
#define SERIAL_8E2 0x800003e
#define SERIAL_5O1 0x8000013
#define SERIAL_6O1 0x8000017
#define SERIAL_7O1 0x800001b
#define SERIAL_8O1 0x800001f
#define SERIAL_5O2 0x8000033
#define SERIAL_6O2 0x8000037
#define SERIAL_7O2 0x800003b
#define SERIAL_8O2 0x800003f

typedef enum { UART_MODE_UART = 0, UART_MODE_RS485_HALF_DUPLEX = 1, UART_MODE_IRDA = 2, UART_MODE_RS485_COLLISION_DETECT = 3, UART_MODE_RS485_APP_CTRL = 4 } SerialMode;

class HardwareSerial : public Stream {
public:
  explicit HardwareSerial(int uart_nr) : _nr(uart_nr) {}
  size_t setRxBufferSize(size_t n);
  void begin(unsigned long baud, uint32_t config = SERIAL_8N1, int8_t rxPin = -1, int8_t txPin = -1, bool invert = false, unsigned long timeout_ms = 20000UL, uint8_t rxfifo_full_thrhd = 112);
  void end();
  bool setPins(int8_t rxPin, int8_t txPin, int8_t ctsPin = -1, int8_t rtsPin = -1);
  bool setMode(SerialMode mode);
  int available() override;
  int availableForWrite() { return 128; }
  int peek() override;
  int read() override;
  size_t read(uint8_t* buf, size_t n);
  size_t readBytes(char* buf, size_t n) override;
  void flush() override;
  void flush(bool txOnly);
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* b, size_t n) override;
  using Print::write;
  unsigned long baudRate() { return _baud; }
  operator bool() const { return true; }
private:
  int _nr;
  unsigned long _baud = 9600;
};
