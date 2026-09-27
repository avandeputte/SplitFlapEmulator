"""The emulated RS-485 bus: a hub every device connects to over a Unix-domain socket.

Physics modelled (all in virtual time, see vclock.py):
  * bandwidth  -- a byte occupies 10 bit-times at its sender's baud rate; the hub knows when
                  every byte's start bit and stop bit are on the wire
  * half duplex -- there is one wire. Two talkers whose bytes overlap in time corrupt each
                  other: the receivers get garbage for both (a random byte, or nothing when the
                  framing is destroyed), exactly the "glued / garbled frames and poisoned serial
                  numbers" a real collision produces
  * baud mismatch -- a receiver listening at another speed sees framing errors (handled by the
                  receiver, which knows its own speed)
  * faults     -- an optional random byte-drop / noise rate, for testing robustness
  * ideal mode -- switch all of that off: every byte arrives intact, still at wire speed

A byte is delivered once its stop bit has passed (plus a small settling margin so a late-arriving
overlapping transmission is still accounted for). Delivery goes to every OTHER client; a talker
does not hear its own echo (the transceivers in this ecosystem have RE tied to DE).

The hub is also the control channel: the supervisor sends CTL messages to a client through it,
and modules report their mechanical state through it.
"""
from __future__ import annotations

import asyncio
import json
import os
import random
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

from .vclock import VirtualClock


@dataclass
class WireByte:
    start_us: int
    end_us: int
    talker: int          # client id
    value: int
    baud: int
    corrupt: bool = False
    delivered: bool = False


@dataclass
class Client:
    cid: int
    kind: str            # "module" | "gateway" | "monitor"
    name: str
    writer: asyncio.StreamWriter
    baud: int = 9600
    # frame reassembly for the monitor view
    frame: bytearray = field(default_factory=bytearray)
    frame_t: int = 0
    frame_corrupt: bool = False


class BusHub:
    TICK_S = 0.002               # real seconds between delivery passes
    MARGIN_US = 300              # virtual settling margin after a byte's stop bit
    FRAME_IDLE_US = 20000        # a talker quiet this long ends its frame in the monitor view

    def __init__(self, sock_path: str, clock: VirtualClock):
        self.sock_path = sock_path
        self.clock = clock
        self.clients: dict[int, Client] = {}
        self._next_cid = 1
        self.wire: list[WireByte] = []          # bytes in flight / recently completed
        self.ideal = False                      # True: no collisions, no noise
        self.drop_prob = 0.0                    # random byte loss (fault injection)
        self.noise_prob = 0.0                   # random garbage byte injection per delivered byte
        self.stats = {"bytes": 0, "collisions": 0, "frames": 0, "dropped": 0}
        self.on_frame: Optional[Callable[[dict], None]] = None       # complete frame seen on the wire
        self.on_state: Optional[Callable[[str, dict], None]] = None  # module STATE reports
        self.on_client: Optional[Callable[[str, str, bool], None]] = None  # (kind, name, connected)
        self.on_log: Optional[Callable[[str, str], None]] = None
        self._server: Optional[asyncio.AbstractServer] = None
        self._rng = random.Random(7)
        self._task: Optional[asyncio.Task] = None

    # ---- lifecycle ---------------------------------------------------------------------------
    async def start(self) -> None:
        try:
            os.unlink(self.sock_path)
        except FileNotFoundError:
            pass
        os.makedirs(os.path.dirname(self.sock_path) or ".", exist_ok=True)
        self._server = await asyncio.start_unix_server(self._handle, path=self.sock_path)
        self._task = asyncio.create_task(self._ticker())

    async def stop(self) -> None:
        if self._task:
            self._task.cancel()
        for c in list(self.clients.values()):
            try:
                c.writer.close()
            except Exception:
                pass
        if self._server:
            self._server.close()
            try:
                await asyncio.wait_for(self._server.wait_closed(), timeout=1.0)
            except (asyncio.TimeoutError, Exception):
                pass

    # ---- clients -----------------------------------------------------------------------------
    async def _handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        cid = self._next_cid
        self._next_cid += 1
        client: Optional[Client] = None
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                text = line.decode("latin-1").rstrip("\r\n")
                if client is None:
                    if not text.startswith("HELLO "):
                        continue
                    parts = text.split(" ", 2)
                    kind = parts[1] if len(parts) > 1 else "unknown"
                    name = parts[2] if len(parts) > 2 else f"client{cid}"
                    client = Client(cid, kind, name, writer)
                    self.clients[cid] = client
                    self._send(client, self.clock.time_line())
                    if self.on_client:
                        self.on_client(kind, name, True)
                    continue
                try:
                    self._dispatch(client, text)
                except Exception:
                    import traceback
                    traceback.print_exc()
        except (asyncio.CancelledError, ConnectionError):
            pass
        finally:
            if client is not None:
                self.clients.pop(cid, None)
                if self.on_client:
                    self.on_client(client.kind, client.name, False)
            try:
                writer.close()
            except Exception:
                pass

    def _dispatch(self, c: Client, text: str) -> None:
        if text.startswith("TX "):
            parts = text.split(" ")
            if len(parts) < 4:
                return
            try:
                start_us = int(parts[1]); baud = int(parts[2]); data = bytes.fromhex(parts[3])
            except ValueError:
                return
            c.baud = baud
            self._transmit(c, start_us, baud, data)
        elif text.startswith("STATE "):
            if self.on_state:
                try:
                    self.on_state(c.name, json.loads(text[6:]))
                except json.JSONDecodeError:
                    pass
        elif text.startswith("LOG "):
            if self.on_log:
                self.on_log(c.name, text[4:])

    def _send(self, c: Client, line: str) -> None:
        try:
            c.writer.write((line + "\n").encode("latin-1"))
        except Exception:
            pass

    def find(self, name: str) -> Optional[Client]:
        for c in self.clients.values():
            if c.name == name:
                return c
        return None

    def ctl(self, name: str, msg: dict) -> bool:
        c = self.find(name)
        if not c:
            return False
        self._send(c, "CTL " + json.dumps(msg, separators=(",", ":")))
        return True

    def broadcast_time(self) -> None:
        line = self.clock.time_line()
        for c in self.clients.values():
            self._send(c, line)

    def set_speed(self, speed: float) -> None:
        self.clock.set_speed(speed)
        self.broadcast_time()

    # ---- the wire ----------------------------------------------------------------------------
    def _transmit(self, c: Client, start_us: int, baud: int, data: bytes) -> None:
        bt = 10.0 * 1e6 / baud
        now = self.clock.now_us()
        # A talker cannot start in the past by more than a moment; clamp gross skew.
        if start_us < now - 5000:
            start_us = now - 5000
        for i, b in enumerate(data):
            s = start_us + int(i * bt)
            e = start_us + int((i + 1) * bt)
            self.wire.append(WireByte(s, e, c.cid, b, baud))
        self.stats["bytes"] += len(data)

    async def _ticker(self) -> None:
        while True:
            try:
                self._deliver()
            except Exception as ex:  # never let the wire die
                print(f"[bus] deliver error: {ex!r}")
            await asyncio.sleep(self.TICK_S)

    def _deliver(self) -> None:
        now = self.clock.now_us()
        cutoff = now - self.MARGIN_US
        if not self.wire:
            self._flush_idle_frames(now)
            return
        ready = [w for w in self.wire if not w.delivered and w.end_us <= cutoff]
        if not ready:
            self._flush_idle_frames(now)
            return
        # Collision check: each ready byte against every other talker's byte overlapping in time.
        if not self.ideal:
            for w in ready:
                for o in self.wire:
                    if o.talker == w.talker:
                        continue
                    if o.start_us < w.end_us and w.start_us < o.end_us:
                        if not w.corrupt:
                            self.stats["collisions"] += 1
                        w.corrupt = True
                        o.corrupt = True
        ready.sort(key=lambda w: (w.end_us, w.talker))
        for w in ready:
            w.delivered = True
        # Drop delivered bytes once nothing arriving late could still overlap them.
        prune = now - 20000
        self.wire = [w for w in self.wire if not (w.delivered and w.end_us < prune)]
        # Build runs: consecutive intact bytes from one talker at one baud, contiguous on the wire.
        run_first: Optional[WireByte] = None
        run: bytearray = bytearray()
        run_last_end = 0

        def flush() -> None:
            nonlocal run_first, run, run_last_end
            if run_first is not None and run:
                self._emit(run_first, bytes(run), self.clients.get(run_first.talker))
            run_first = None
            run = bytearray()
            run_last_end = 0

        for w in ready:
            v: int = w.value
            if w.corrupt and not self.ideal:
                # a collision destroys framing more often than it yields a clean wrong byte
                v = -1 if self._rng.random() < 0.6 else self._rng.randrange(256)
            elif not self.ideal and self.drop_prob and self._rng.random() < self.drop_prob:
                v = -1
                self.stats["dropped"] += 1
            bt = 10.0 * 1e6 / w.baud
            contiguous = (run_first is not None and run_first.talker == w.talker and run_first.baud == w.baud
                          and abs(w.end_us - run_last_end - bt) < 50 and len(run) < 64)
            if v < 0:
                flush()
                continue
            if not contiguous:
                flush()
                run_first = w
            run.append(v)
            run_last_end = w.end_us
            if not self.ideal and self.noise_prob and self._rng.random() < self.noise_prob:
                run.append(self._rng.randrange(256))
                run_last_end += int(bt)
        flush()

    def _emit(self, first: WireByte, data: bytes, talker: Optional[Client]) -> None:
        line = f"RX {first.end_us} {first.baud} {data.hex()}"
        for c in self.clients.values():
            if c.cid == first.talker or c.kind == "monitor":
                continue
            self._send(c, line)
        # monitor-side frame reassembly (what an oscilloscope on the wire would show)
        if talker is not None:
            if talker.frame and first.end_us - talker.frame_t > self.FRAME_IDLE_US:
                self._finish_frame(talker)
            if not talker.frame:
                talker.frame_t = first.start_us
            talker.frame_corrupt = talker.frame_corrupt or first.corrupt
            for b in data:
                talker.frame.append(b)
                if b == 0x0A:
                    self._finish_frame(talker)
            talker.frame_t = first.end_us if talker.frame else talker.frame_t

    def _flush_idle_frames(self, now: int) -> None:
        for c in self.clients.values():
            if c.frame and now - c.frame_t > self.FRAME_IDLE_US:
                self._finish_frame(c)

    def _finish_frame(self, c: Client) -> None:
        if not c.frame:
            return
        self.stats["frames"] += 1
        if self.on_frame:
            self.on_frame({
                "t": c.frame_t, "from": c.name, "kind": c.kind,
                "data": bytes(c.frame).decode("latin-1"), "corrupt": c.frame_corrupt,
            })
        c.frame = bytearray()
        c.frame_corrupt = False
