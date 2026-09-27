// vclock.h -- the emulator's virtual clock, shared by the module and gateway shims.
//
// Every emulated device measures time with this clock instead of the host's. It runs at
// `speed` times real time (1.0 = the physical pace), and the hub can re-anchor it at any
// moment ("TIME" message) without a discontinuity: the anchor is moved to the current point
// before the new speed applies, so millis() never jumps.
//
// All timestamps exchanged on the bus are in this virtual timeline (microseconds), which is
// shared by every process on the machine because it is derived from CLOCK_MONOTONIC.
#pragma once
#include <cstdint>
#include <ctime>
#include <mutex>
#include <thread>
#include <chrono>

namespace sfemu {

inline int64_t mono_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

class VClock {
public:
  // Anchor the virtual timeline: virtual `base_virt_us` corresponds to real `base_real_ns`,
  // and time advances `speed` times faster than real time from there.
  void set(int64_t base_real_ns, int64_t base_virt_us, double speed) {
    std::lock_guard<std::mutex> g(m_);
    base_real_ns_ = base_real_ns;
    base_virt_us_ = base_virt_us;
    speed_ = speed > 0 ? speed : 1.0;
  }
  // Change only the speed, keeping the current virtual instant continuous.
  void set_speed(double speed) {
    std::lock_guard<std::mutex> g(m_);
    int64_t now_r = mono_ns();
    base_virt_us_ = virt_at_locked(now_r);
    base_real_ns_ = now_r;
    speed_ = speed > 0 ? speed : 1.0;
  }
  double speed() const { std::lock_guard<std::mutex> g(m_); return speed_; }
  int64_t now_us() const {
    std::lock_guard<std::mutex> g(m_);
    return virt_at_locked(mono_ns());
  }
  // Real nanoseconds until virtual instant t_us (0 if already reached).
  int64_t real_ns_until(int64_t t_us) const {
    std::lock_guard<std::mutex> g(m_);
    int64_t now_v = virt_at_locked(mono_ns());
    if (t_us <= now_v) return 0;
    return (int64_t)((double)(t_us - now_v) * 1000.0 / speed_);
  }
  // Sleep until virtual instant t_us. Sleeps in slices so a speed change mid-sleep is honoured.
  void sleep_until_us(int64_t t_us) const {
    for (;;) {
      int64_t ns = real_ns_until(t_us);
      if (ns <= 0) return;
      if (ns > 2000000) ns = 2000000;        // 2 ms slices: re-check the speed often
      std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
  }
  void sleep_us(int64_t us) const { sleep_until_us(now_us() + us); }

private:
  int64_t virt_at_locked(int64_t real_ns) const {
    return base_virt_us_ + (int64_t)((double)(real_ns - base_real_ns_) * speed_ / 1000.0);
  }
  mutable std::mutex m_;
  int64_t base_real_ns_ = mono_ns();
  int64_t base_virt_us_ = 0;
  double  speed_ = 1.0;
};

// The one clock of this process.
inline VClock& vclock() { static VClock c; return c; }

} // namespace sfemu
