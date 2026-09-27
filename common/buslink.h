// buslink.h -- client side of the emulated RS-485 bus (a Unix-domain stream socket to the hub).
//
// Wire protocol (newline-delimited text, both directions):
//   client -> hub   HELLO <kind> <name>            kind = module | gateway
//                   TX <start_us> <baud> <hex>     bytes put on the wire, first start bit at start_us
//                   STATE <json>                   (module) mechanical / firmware state snapshot
//                   LOG <text>                     free-text diagnostics
//   hub -> client   TIME <base_real_ns> <base_virt_us> <speed>
//                   RX <end_us> <baud> <hex>       bytes as received: byte i completes at end_us + i*bytetime
//                   CTL <json>                     control command (power, faults, reel, ...)
//
// One reader thread per process dispatches incoming lines to callbacks. TX is written straight
// to the socket from the caller's thread (serialised by a mutex).
#pragma once
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include "vclock.h"

namespace sfemu {

inline std::string to_hex(const uint8_t* d, size_t n) {
  static const char* H = "0123456789abcdef";
  std::string s; s.resize(n * 2);
  for (size_t i = 0; i < n; i++) { s[2*i] = H[d[i] >> 4]; s[2*i+1] = H[d[i] & 15]; }
  return s;
}
inline std::vector<uint8_t> from_hex(const std::string& s) {
  std::vector<uint8_t> v; v.reserve(s.size() / 2);
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back((uint8_t)((nib(s[i]) << 4) | nib(s[i+1])));
  return v;
}

class BusLink {
public:
  using RxFn  = std::function<void(int64_t end_us, uint32_t baud, const std::vector<uint8_t>&)>;
  using CtlFn = std::function<void(const std::string& json)>;

  // Wire time of one byte (start + 8 data + stop = 10 bits) in microseconds.
  static double byte_us(uint32_t baud) { return baud ? 10.0 * 1e6 / (double)baud : 1041.67; }

  bool connect(const std::string& path, const std::string& kind, const std::string& name) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
    if (::connect(fd_, (struct sockaddr*)&a, sizeof(a)) < 0) { ::close(fd_); fd_ = -1; return false; }
    send_line("HELLO " + kind + " " + name);
    reader_ = std::thread([this] { reader_loop(); });
    return true;
  }
  bool connected() const { return fd_ >= 0 && !closed_; }
  void on_rx(RxFn f)   { rx_ = std::move(f); }
  void on_ctl(CtlFn f) { ctl_ = std::move(f); }
  // Blocks until the hub has sent its first TIME message (or the timeout expires).
  bool wait_time(int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
      if (have_time_) return true;
      usleep(1000);
    }
    return have_time_;
  }

  void tx(int64_t start_us, uint32_t baud, const uint8_t* d, size_t n) {
    if (!n) return;
    char head[64];
    snprintf(head, sizeof(head), "TX %lld %u ", (long long)start_us, (unsigned)baud);
    send_line(std::string(head) + to_hex(d, n));
  }
  void state(const std::string& json) { send_line("STATE " + json); }
  void log(const std::string& text)   { send_line("LOG " + text); }

  void send_line(const std::string& line) {
    if (fd_ < 0 || closed_) return;
    std::lock_guard<std::mutex> g(wm_);
    std::string s = line + "\n";
    const char* p = s.data(); size_t left = s.size();
    while (left) {
      ssize_t w = ::write(fd_, p, left);
      if (w <= 0) { closed_ = true; return; }
      p += w; left -= (size_t)w;
    }
  }
  ~BusLink() { closed_ = true; if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR); if (reader_.joinable()) reader_.join(); if (fd_ >= 0) ::close(fd_); }

private:
  void reader_loop() {
    std::string buf; char tmp[4096];
    while (!closed_) {
      ssize_t n = ::read(fd_, tmp, sizeof(tmp));
      if (n <= 0) { closed_ = true; break; }
      buf.append(tmp, (size_t)n);
      size_t pos;
      while ((pos = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        handle(line);
      }
    }
  }
  void handle(const std::string& line) {
    if (line.compare(0, 5, "TIME ") == 0) {
      long long br = 0, bv = 0; double sp = 1.0;
      if (sscanf(line.c_str() + 5, "%lld %lld %lf", &br, &bv, &sp) == 3) {
        vclock().set(br, bv, sp);
        have_time_ = true;
      }
    } else if (line.compare(0, 3, "RX ") == 0) {
      long long end = 0; unsigned baud = 9600; char hex[4096] = {0};
      if (sscanf(line.c_str() + 3, "%lld %u %4095s", &end, &baud, hex) >= 2 && rx_) {
        rx_((int64_t)end, baud, from_hex(hex));
      }
    } else if (line.compare(0, 4, "CTL ") == 0) {
      if (ctl_) ctl_(line.substr(4));
    }
  }
  int fd_ = -1;
  std::atomic<bool> closed_{false};
  std::atomic<bool> have_time_{false};
  std::thread reader_;
  std::mutex wm_;
  RxFn rx_; CtlFn ctl_;
};

} // namespace sfemu
