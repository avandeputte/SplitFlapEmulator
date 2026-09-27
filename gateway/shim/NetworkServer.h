#pragma once
#include "Arduino.h"
#include "Server.h"
#include "NetworkClient.h"

class NetworkServer : public Server {
public:
  NetworkServer(uint16_t port = 80, uint8_t max_clients = 4) : _port(port) {}
  NetworkServer(const IPAddress&, uint16_t port = 80, uint8_t max_clients = 4) : _port(port) {}
  ~NetworkServer() { end(); }
  NetworkClient accept();
  NetworkClient available() { return accept(); }
  void begin() override { begin(0); }
  void begin(uint16_t port);
  void begin(uint16_t port, int reuse_enable) { begin(port); }
  void setNoDelay(bool nodelay) { _noDelay = nodelay; }
  bool getNoDelay() { return _noDelay; }
  bool hasClient();
  void end();
  void close() { end(); }
  void stop() { end(); }
  operator bool() { return _listening; }
  int setTimeout(uint32_t seconds) { return 0; }
  size_t write(uint8_t) override { return 0; }
private:
  int _fd = -1;
  uint16_t _port;
  bool _listening = false;
  bool _noDelay = false;
};
