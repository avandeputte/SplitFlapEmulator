"""Process supervisor: one process per emulated module, one for the gateway board.

A process exiting with 100+n "reset" itself for reason n (module: RSTFR bits, gateway:
esp_reset_reason_t) and is rebooted with that cause; any other exit is a crash (rebooted as a
power-on after a short delay). Powering a device off is simply not running its process; its
EEPROM / NVS files stay on disk, as the chips would.
"""
from __future__ import annotations

import asyncio
import collections
import json
import os
import signal
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

from .bus import BusHub
from .config import EmuConfig, ModuleCfg


@dataclass
class Proc:
    name: str
    kind: str                       # "module" | "gateway"
    proc: Optional[asyncio.subprocess.Process] = None
    log: collections.deque = field(default_factory=lambda: collections.deque(maxlen=400))
    boots: int = 0
    last_exit: Optional[int] = None
    started_at: float = 0.0
    cause: int = 1                  # cause of the next/current boot
    desired: bool = True            # should be running
    task: Optional[asyncio.Task] = None


class Supervisor:
    def __init__(self, cfg: EmuConfig, hub: BusHub, data_dir: str, bin_dir: str, bus_path: str,
                 gateway_http_port: int = 80):
        self.cfg = cfg
        self.hub = hub
        self.data_dir = data_dir
        self.bin_dir = bin_dir
        self.bus_path = bus_path
        self.gateway_http_port = gateway_http_port
        self.procs: dict[str, Proc] = {}
        self.on_log: Optional[Callable[[str, str], None]] = None
        self._stopping = False
        os.makedirs(os.path.join(data_dir, "modules"), exist_ok=True)
        os.makedirs(os.path.join(data_dir, "gateway"), exist_ok=True)

    # ---- lifecycle ---------------------------------------------------------------------------
    async def start(self) -> None:
        for m in self.cfg.modules:
            self._ensure_module(m)
        self._ensure_gateway()

    async def stop(self) -> None:
        self._stopping = True
        for p in list(self.procs.values()):
            p.desired = False
            await self._kill(p)

    def _ensure_module(self, m: ModuleCfg) -> Proc:
        p = self.procs.get(m.sn)
        if p is None:
            p = Proc(m.sn, "module")
            self.procs[m.sn] = p
        p.desired = m.power
        if p.desired and p.task is None:
            p.task = asyncio.create_task(self._run(p))
        return p

    def _ensure_gateway(self) -> Proc:
        p = self.procs.get("gateway")
        if p is None:
            p = Proc("gateway", "gateway")
            self.procs["gateway"] = p
        p.desired = self.cfg.gateway.power
        if p.desired and p.task is None:
            p.task = asyncio.create_task(self._run(p))
        return p

    def _command(self, p: Proc) -> tuple[list[str], dict]:
        env = dict(os.environ)
        if p.kind == "module":
            m = self.cfg.module(p.name)
            cmd = [os.path.join(self.bin_dir, "sfmodule"), "--serial", p.name,
                   "--data", os.path.join(self.data_dir, "modules"), "--bus", self.bus_path,
                   "--cause", str(p.cause), "--boot", str(p.boots)]
            if m:
                cmd += ["--mech", json.dumps(self.cfg.mech_message(m), separators=(",", ":"))]
            if os.environ.get("SFEMU_DEBUG"):
                env["SFEMU_DEBUG"] = "1"
        else:
            g = self.cfg.gateway
            cmd = [os.path.join(self.bin_dir, "sfgateway"), "--data", os.path.join(self.data_dir, "gateway"),
                   "--bus", self.bus_path, "--reason", str(p.cause), "--port", str(self.gateway_http_port)]
            if g.ip:
                cmd += ["--ip", g.ip]
            if g.mac:
                env["SFEMU_MAC"] = g.mac
        return cmd, env

    async def _run(self, p: Proc) -> None:
        backoff = 0.5
        try:
            while p.desired and not self._stopping:
                p.boots += 1
                cmd, env = self._command(p)
                try:
                    p.proc = await asyncio.create_subprocess_exec(*cmd, stdout=asyncio.subprocess.PIPE,
                                                                  stderr=asyncio.subprocess.STDOUT, env=env)
                except FileNotFoundError as ex:
                    self._log(p, f"cannot start: {ex}")
                    await asyncio.sleep(5)
                    continue
                p.started_at = time.time()
                self._log(p, f"--- boot #{p.boots} (cause {p.cause}) pid {p.proc.pid}")
                # Hand a module its mechanics as soon as it is on the bus.
                if p.kind == "module":
                    asyncio.create_task(self._send_mech_when_connected(p.name))
                elif not self.cfg.gateway.wifi:
                    asyncio.create_task(self._ctl_when_connected("gateway", {"op": "wifi", "up": False}))
                assert p.proc.stdout is not None
                while True:
                    line = await p.proc.stdout.readline()
                    if not line:
                        break
                    self._log(p, line.decode("latin-1").rstrip())
                rc = await p.proc.wait()
                p.last_exit = rc
                p.proc = None
                if not p.desired or self._stopping:
                    break
                if rc is not None and 100 <= rc < 200:
                    p.cause = rc - 100          # the device reset itself: reboot with that cause
                    self._log(p, f"--- reset (cause {p.cause})")
                    await asyncio.sleep(0.2)
                    backoff = 0.5
                else:
                    self._log(p, f"--- exited rc={rc}; restarting in {backoff:.1f}s")
                    p.cause = 1
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 2, 10)
        finally:
            p.task = None

    async def _send_mech_when_connected(self, sn: str) -> None:
        for _ in range(200):
            m = self.cfg.module(sn)
            if m is None:
                return
            if self.hub.ctl(sn, self.cfg.mech_message(m)):
                return
            await asyncio.sleep(0.025)

    async def _ctl_when_connected(self, name: str, msg: dict) -> None:
        for _ in range(200):
            if self.hub.ctl(name, msg):
                return
            await asyncio.sleep(0.025)

    async def _kill(self, p: Proc) -> None:
        if p.proc is not None and p.proc.returncode is None:
            try:
                p.proc.send_signal(signal.SIGTERM)
                await asyncio.wait_for(p.proc.wait(), timeout=3)
            except (asyncio.TimeoutError, ProcessLookupError):
                try:
                    p.proc.kill()
                except ProcessLookupError:
                    pass
        if p.task is not None:
            p.task.cancel()
            p.task = None

    def _log(self, p: Proc, text: str) -> None:
        p.log.append((time.time(), text))
        if self.on_log:
            self.on_log(p.name, text)

    # ---- controls ----------------------------------------------------------------------------------
    async def set_module_power(self, sn: str, on: bool) -> None:
        m = self.cfg.module(sn)
        if m is None:
            return
        m.power = on
        p = self._ensure_module(m)
        if not on:
            p.desired = False
            await self._kill(p)
            p.cause = 1                     # next power-up is a power-on reset
        elif p.task is None:
            p.task = asyncio.create_task(self._run(p))

    async def set_gateway_power(self, on: bool) -> None:
        self.cfg.gateway.power = on
        p = self._ensure_gateway()
        if not on:
            p.desired = False
            await self._kill(p)
            p.cause = 1
        elif p.task is None:
            p.task = asyncio.create_task(self._run(p))

    async def power_cycle(self, name: str) -> None:
        p = self.procs.get(name)
        if p is None:
            return
        if p.kind == "module":
            await self.set_module_power(name, False)
            await asyncio.sleep(0.3)
            await self.set_module_power(name, True)
        else:
            await self.set_gateway_power(False)
            await asyncio.sleep(0.3)
            await self.set_gateway_power(True)

    async def add_module(self, m: ModuleCfg) -> None:
        self.cfg.modules.append(m)
        self.cfg.modules.sort(key=lambda x: x.slot)
        self._ensure_module(m)

    async def remove_module(self, sn: str, wipe: bool = True) -> None:
        p = self.procs.pop(sn, None)
        if p is not None:
            p.desired = False
            await self._kill(p)
        self.cfg.modules = [m for m in self.cfg.modules if m.sn != sn]
        if wipe:
            for suffix in (".eeprom", ".eeprom.tmp"):
                try:
                    os.unlink(os.path.join(self.data_dir, "modules", sn + suffix))
                except FileNotFoundError:
                    pass

    def status(self) -> dict:
        out = {}
        for name, p in self.procs.items():
            out[name] = {
                "running": p.proc is not None and p.proc.returncode is None,
                "boots": p.boots, "lastExit": p.last_exit, "cause": p.cause,
                "uptime": (time.time() - p.started_at) if p.proc is not None else 0,
                "desired": p.desired,
            }
        return out
