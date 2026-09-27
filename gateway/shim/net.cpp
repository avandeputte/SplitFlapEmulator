// net.cpp -- NetworkClient / NetworkServer on POSIX sockets.
#include "NetworkClient.h"
#include "NetworkServer.h"
#include "emu.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <string>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

struct NetSocket {
  int fd = -1;
  bool peerClosed = false;
  uint32_t connTimeoutMs = 3000;
  int peekByte = -1;
  ~NetSocket() { if (fd >= 0) ::close(fd); }
};

static void applyTimeouts(int fd, uint32_t ms) {
  struct timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

NetworkClient::NetworkClient() {}
NetworkClient::NetworkClient(int fd) : _s(std::make_shared<NetSocket>()) {
  _s->fd = fd;
  int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  applyTimeouts(fd, _s->connTimeoutMs);
}
NetworkClient::~NetworkClient() {}

int NetworkClient::connect(IPAddress ip, uint16_t port) { return connect(ip, port, 3000); }
int NetworkClient::connect(IPAddress ip, uint16_t port, int32_t timeout_ms) { return connect(ip.toString().c_str(), port, timeout_ms); }
int NetworkClient::connect(const char* host, uint16_t port) { return connect(host, port, 3000); }
int NetworkClient::connect(const char* host, uint16_t port, int32_t timeout_ms) {
  if (!g_emu.wifiUp) return 0;                      // no network, no sockets
  stop();
  struct addrinfo hints; memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  char ps[8]; snprintf(ps, sizeof(ps), "%u", port);
  if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return 0;
  int fd = ::socket(res->ai_family, SOCK_STREAM, 0);
  if (fd < 0) { freeaddrinfo(res); return 0; }
  int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  if (rc < 0 && errno != EINPROGRESS) { ::close(fd); return 0; }
  if (rc < 0) {
    struct pollfd p = {fd, POLLOUT, 0};
    if (poll(&p, 1, timeout_ms > 0 ? timeout_ms : 3000) <= 0) { ::close(fd); return 0; }
    int err = 0; socklen_t el = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { ::close(fd); return 0; }
  }
  fcntl(fd, F_SETFL, fl);
  _s = std::make_shared<NetSocket>();
  _s->fd = fd;
  _s->connTimeoutMs = timeout_ms > 0 ? (uint32_t)timeout_ms : 3000;
  applyTimeouts(fd, _s->connTimeoutMs);
  return 1;
}

size_t NetworkClient::write(uint8_t b) { return write(&b, 1); }
size_t NetworkClient::write(const uint8_t* buf, size_t size) {
  if (!_s || _s->fd < 0) return 0;
  size_t sent = 0;
  while (sent < size) {
    ssize_t n = ::send(_s->fd, buf + sent, size - sent, MSG_NOSIGNAL);
    if (n <= 0) { if (n < 0 && errno == EINTR) continue; stop(); return sent; }
    sent += (size_t)n;
  }
  return sent;
}
size_t NetworkClient::write(Stream& stream) { size_t n = 0; while (stream.available()) { int c = stream.read(); if (c < 0) break; n += write((uint8_t)c); } return n; }
size_t NetworkClient::write(Stream& stream, size_t length) { size_t n = 0; while (n < length && stream.available()) { int c = stream.read(); if (c < 0) break; n += write((uint8_t)c); } return n; }
void NetworkClient::flush() {}
int NetworkClient::available() {
  if (!_s || _s->fd < 0) return 0;
  int n = 0;
  if (ioctl(_s->fd, FIONREAD, &n) < 0) return 0;
  return n + (_s->peekByte >= 0 ? 1 : 0);
}
int NetworkClient::read() {
  if (!_s || _s->fd < 0) return -1;
  if (_s->peekByte >= 0) { int b = _s->peekByte; _s->peekByte = -1; return b; }
  uint8_t b;
  ssize_t n = ::recv(_s->fd, &b, 1, MSG_DONTWAIT);
  if (n == 1) return b;
  if (n == 0) _s->peerClosed = true;
  return -1;
}
int NetworkClient::read(uint8_t* buf, size_t size) {
  if (!_s || _s->fd < 0) return -1;
  size_t got = 0;
  if (_s->peekByte >= 0 && size) { buf[got++] = (uint8_t)_s->peekByte; _s->peekByte = -1; }
  if (got < size) {
    ssize_t n = ::recv(_s->fd, buf + got, size - got, MSG_DONTWAIT);
    if (n > 0) got += (size_t)n;
    else if (n == 0) _s->peerClosed = true;
  }
  return (int)got;
}
size_t NetworkClient::readBytes(char* buffer, size_t length) {
  // Blocking read up to the stream timeout, like the core's socket read with SO_RCVTIMEO.
  size_t got = 0;
  unsigned long start = millis();
  while (got < length) {
    int n = read((uint8_t*)buffer + got, length - got);
    if (n > 0) { got += (size_t)n; continue; }
    if (!connected()) break;
    if (millis() - start >= getTimeout()) break;
    delay(1);
  }
  return got;
}
int NetworkClient::peek() {
  if (!_s || _s->fd < 0) return -1;
  if (_s->peekByte >= 0) return _s->peekByte;
  uint8_t b;
  ssize_t n = ::recv(_s->fd, &b, 1, MSG_DONTWAIT | MSG_PEEK);
  return n == 1 ? b : -1;
}
void NetworkClient::clear() { uint8_t b[256]; while (available() > 0) { if (read(b, sizeof(b)) <= 0) break; } }
void NetworkClient::stop() { if (_s) { if (_s->fd >= 0) { ::close(_s->fd); _s->fd = -1; } } _s.reset(); }
uint8_t NetworkClient::connected() {
  if (!_s || _s->fd < 0) return 0;
  if (_s->peekByte >= 0) return 1;
  struct pollfd p = {_s->fd, POLLIN, 0};
  int r = poll(&p, 1, 0);
  if (r > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
    uint8_t b;
    ssize_t n = ::recv(_s->fd, &b, 1, MSG_DONTWAIT | MSG_PEEK);
    if (n == 0) { _s->peerClosed = true; stop(); return 0; }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { stop(); return 0; }
  }
  return 1;
}
void NetworkClient::setSSE(bool sse) { _sse = sse; }
bool NetworkClient::isSSE() { return _sse; }
bool NetworkClient::operator==(const NetworkClient& o) { return _s == o._s; }
int NetworkClient::fd() const { return _s ? _s->fd : -1; }
void NetworkClient::setConnectionTimeout(uint32_t ms) { if (_s) { _s->connTimeoutMs = ms; if (_s->fd >= 0) applyTimeouts(_s->fd, ms); } }
int NetworkClient::setNoDelay(bool nodelay) { if (!_s || _s->fd < 0) return -1; int v = nodelay ? 1 : 0; return setsockopt(_s->fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)); }
bool NetworkClient::getNoDelay() { if (!_s || _s->fd < 0) return false; int v = 0; socklen_t l = sizeof(v); getsockopt(_s->fd, IPPROTO_TCP, TCP_NODELAY, &v, &l); return v != 0; }
static IPAddress addrOf(int fd, bool local, uint16_t* port) {
  struct sockaddr_in a; socklen_t l = sizeof(a);
  int r = local ? getsockname(fd, (struct sockaddr*)&a, &l) : getpeername(fd, (struct sockaddr*)&a, &l);
  if (r < 0) { if (port) *port = 0; return IPAddress(); }
  if (port) *port = ntohs(a.sin_port);
  return IPAddress((uint32_t)a.sin_addr.s_addr);
}
IPAddress NetworkClient::remoteIP() const { return _s ? addrOf(_s->fd, false, nullptr) : IPAddress(); }
uint16_t NetworkClient::remotePort() const { uint16_t p = 0; if (_s) addrOf(_s->fd, false, &p); return p; }
IPAddress NetworkClient::localIP() const { return _s ? addrOf(_s->fd, true, nullptr) : IPAddress(); }
uint16_t NetworkClient::localPort() const { uint16_t p = 0; if (_s) addrOf(_s->fd, true, &p); return p; }

// ---- server -------------------------------------------------------------------------------------
void NetworkServer::begin(uint16_t port) {
  if (_listening) return;
  if (port) _port = port;
  uint16_t real = _port;
  if (_port == 80 && g_emu.httpPort > 0) real = (uint16_t)g_emu.httpPort;   // the emulator's port mapping
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(real);
  if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) { fprintf(stderr, "[emu] cannot listen on port %u: %s\n", real, strerror(errno)); ::close(fd); return; }
  int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  _fd = fd; _listening = true;
  printf("[emu] HTTP server listening on port %u (firmware port %u)\n", real, _port);
}
NetworkClient NetworkServer::accept() {
  if (!_listening) return NetworkClient();
  int c = ::accept(_fd, nullptr, nullptr);
  if (c < 0) return NetworkClient();
  if (_noDelay) { int v = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)); }
  return NetworkClient(c);
}
bool NetworkServer::hasClient() {
  if (!_listening) return false;
  struct pollfd p = {_fd, POLLIN, 0};
  return poll(&p, 1, 0) > 0;
}
void NetworkServer::end() { if (_fd >= 0) ::close(_fd); _fd = -1; _listening = false; }
