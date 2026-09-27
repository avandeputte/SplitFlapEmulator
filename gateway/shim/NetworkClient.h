// NetworkClient.h -- the ESP32 core's TCP client (formerly WiFiClient) on POSIX sockets.
// Copies share one socket (the core's semantics): server.client().stop() closes the real one.
#pragma once
#include "Arduino.h"
#include "Client.h"
#include <memory>

struct NetSocket;

class NetworkClient : public Client {
public:
  NetworkClient();
  explicit NetworkClient(int fd);
  ~NetworkClient() override;
  int connect(IPAddress ip, uint16_t port) override;
  int connect(IPAddress ip, uint16_t port, int32_t timeout_ms);
  int connect(const char* host, uint16_t port) override;
  int connect(const char* host, uint16_t port, int32_t timeout_ms);
  size_t write(uint8_t data) override;
  size_t write(const uint8_t* buf, size_t size) override;
  size_t write_P(PGM_P buf, size_t size) { return write((const uint8_t*)buf, size); }
  using Print::write;
  size_t write(Stream& stream);
  size_t write(Stream& stream, size_t length);
  void flush() override;
  int available() override;
  int read() override;
  int read(uint8_t* buf, size_t size) override;
  size_t readBytes(char* buffer, size_t length) override;
  size_t readBytes(uint8_t* buffer, size_t length) override { return readBytes((char*)buffer, length); }
  int peek() override;
  void clear();
  void stop() override;
  uint8_t connected() override;
  void setSSE(bool sse);
  bool isSSE();
  operator bool() override { return connected(); }
  bool operator==(const bool value) { return bool() == value; }
  bool operator!=(const bool value) { return bool() != value; }
  bool operator==(const NetworkClient& o);
  bool operator!=(const NetworkClient& o) { return !this->operator==(o); }
  int fd() const;
  void setConnectionTimeout(uint32_t milliseconds);
  int setNoDelay(bool nodelay);
  bool getNoDelay();
  IPAddress remoteIP() const;
  uint16_t remotePort() const;
  IPAddress localIP() const;
  uint16_t localPort() const;
private:
  std::shared_ptr<NetSocket> _s;
  bool _sse = false;
};
