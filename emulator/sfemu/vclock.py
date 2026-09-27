"""The emulator's virtual clock (Python side).

Mirrors common/vclock.h: virtual time = base_virt_us + (monotonic_ns - base_real_ns) * speed / 1000.
Every emulated process derives the same timeline from CLOCK_MONOTONIC, so the hub can hand a
single anchor to all of them ("TIME <base_real_ns> <base_virt_us> <speed>").
"""
import time


def mono_ns() -> int:
    """CLOCK_MONOTONIC in nanoseconds -- the very same clock the C++ shims read, so every process
    shares one timeline (mono_ns() is a different clock on macOS)."""
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


class VirtualClock:
    def __init__(self, speed: float = 1.0):
        self.base_real_ns = mono_ns()
        self.base_virt_us = 0
        self.speed = float(speed) if speed > 0 else 1.0

    def now_us(self) -> int:
        return self.base_virt_us + int((mono_ns() - self.base_real_ns) * self.speed / 1000.0)

    def set_speed(self, speed: float) -> None:
        """Change the pace without a discontinuity."""
        now_r = mono_ns()
        self.base_virt_us = self.now_us()
        self.base_real_ns = now_r
        self.speed = float(speed) if speed > 0 else 1.0

    def time_line(self) -> str:
        return f"TIME {self.base_real_ns} {self.base_virt_us} {self.speed:.6f}"

    def real_seconds_for(self, virt_us: int) -> float:
        return virt_us / 1e6 / self.speed
