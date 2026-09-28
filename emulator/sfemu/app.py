"""The emulator's own web app: control API, live state over WebSocket, the control panel and
the virtual display. Everything the gateway itself serves stays on the gateway's port; this app
never touches the gateway's HTTP surface, so the dashboard the user sees is the real one.
"""
from __future__ import annotations

import asyncio
import collections
import json
import os
import shutil
import time
from dataclasses import asdict
from typing import Any, Optional

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from . import __version__
from .bus import BusHub
from .config import EmuConfig, ModuleCfg, Mech
from .supervisor import Supervisor
from .jobs import WallJobs
from .vclock import VirtualClock

STATIC = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "static")


class Emulator:
    """Wires the hub, the supervisor and the live state together; owned by the FastAPI app."""

    def __init__(self, data_dir: str, bin_dir: str, gateway_http_port: int, gateway_public_url: str):
        self.data_dir = data_dir
        self.bin_dir = bin_dir
        self.gateway_http_port = gateway_http_port
        self.gateway_public_url = gateway_public_url
        os.makedirs(data_dir, exist_ok=True)
        fresh = not os.path.exists(EmuConfig.path(data_dir))
        if fresh and (os.environ.get("SFEMU_ROWS") or os.environ.get("SFEMU_COLS")):
            cfg = EmuConfig(rows=int(os.environ.get("SFEMU_ROWS", "3")), cols=int(os.environ.get("SFEMU_COLS", "15")))
            cfg.populate()
            cfg.save(data_dir)
        self.cfg = EmuConfig.load(data_dir)
        if os.environ.get("SFEMU_SPEED"):
            self.cfg.speed = max(0.1, min(50.0, float(os.environ["SFEMU_SPEED"])))
        self.clock = VirtualClock(self.cfg.speed)
        self.bus_path = os.path.join(data_dir, "bus.sock")
        self.hub = BusHub(self.bus_path, self.clock)
        self.hub.ideal = self.cfg.bus.ideal
        self.hub.drop_prob = self.cfg.bus.drop
        self.hub.noise_prob = self.cfg.bus.noise
        self.sup = Supervisor(self.cfg, self.hub, data_dir, bin_dir, self.bus_path, gateway_http_port)
        self.module_state: dict[str, dict] = {}
        self.wire: collections.deque = collections.deque(maxlen=600)
        self.wire_seq = 0
        self.log: collections.deque = collections.deque(maxlen=2000)
        self.log_seq = 0
        self.clients: set[WebSocket] = set()
        self._dirty_states: set[str] = set()
        self._pending_wire: list[dict] = []
        self._pending_log: list[dict] = []
        self.hub.on_state = self._on_state
        self.hub.on_frame = self._on_frame
        self.hub.on_log = lambda name, text: self._on_log(name, "[bus] " + text)
        self.hub.on_client = self._on_client
        self.sup.on_log = self._on_log
        self.started = time.time()
        self._push_task: Optional[asyncio.Task] = None
        self.jobs = WallJobs(self)

    async def start(self) -> None:
        await self.hub.start()
        await self.sup.start()
        self._push_task = asyncio.create_task(self._pusher())

    async def stop(self) -> None:
        if self._push_task:
            self._push_task.cancel()
        await self.sup.stop()
        await self.hub.stop()

    # ---- events from the hub / supervisor ------------------------------------------------------------
    def _on_state(self, name: str, st: dict) -> None:
        st["t"] = time.time()
        self.module_state[name] = st
        self._dirty_states.add(name)

    def _on_frame(self, f: dict) -> None:
        self.wire_seq += 1
        f["seq"] = self.wire_seq
        f["wall"] = time.time()
        self.wire.append(f)
        self._pending_wire.append(f)

    def _on_log(self, name: str, text: str) -> None:
        self.log_seq += 1
        item = {"seq": self.log_seq, "t": time.time(), "src": name, "text": text}
        self.log.append(item)
        self._pending_log.append(item)

    def _on_client(self, kind: str, name: str, up: bool) -> None:
        if kind == "module" and not up:
            st = self.module_state.get(name)
            if st:
                st["offline"] = True
                self._dirty_states.add(name)
        self._on_log(name, f"[bus] {'on the bus' if up else 'left the bus'}")

    # ---- snapshots ---------------------------------------------------------------------------------------
    def snapshot(self) -> dict:
        procs = self.sup.status()
        modules = []
        for m in self.cfg.modules:
            st = dict(self.module_state.get(m.sn, {}))
            p = procs.get(m.sn, {})
            st["online"] = bool(p.get("running")) and not st.get("offline", False)
            modules.append({"cfg": asdict(m), "state": st, "proc": p})
        return {
            "version": __version__,
            "speed": self.clock.speed,
            "now_us": self.clock.now_us(),
            "rows": self.cfg.rows, "cols": self.cfg.cols,
            "sound": self.cfg.sound,
            "bus": {"ideal": self.hub.ideal, "drop": self.hub.drop_prob, "noise": self.hub.noise_prob, "stats": dict(self.hub.stats),
                    "clients": len(self.hub.clients)},
            "gateway": {"cfg": asdict(self.cfg.gateway), "proc": procs.get("gateway", {}),
                        "url": self.gateway_public_url, "port": self.gateway_http_port},
            "modules": modules,
            "uptime": time.time() - self.started,
            "job": self.jobs.current.snapshot() if self.jobs.current else None,
        }

    async def broadcast(self, msg: dict) -> None:
        if not self.clients:
            return
        data = json.dumps(msg, separators=(",", ":"))
        dead = []
        for ws in list(self.clients):
            try:
                await ws.send_text(data)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)

    async def _pusher(self) -> None:
        """Push module states at ~30 Hz, wire frames and logs as they come."""
        while True:
            try:
                if self._dirty_states:
                    names = list(self._dirty_states)
                    self._dirty_states.clear()
                    await self.broadcast({"type": "state", "modules": {n: self.module_state[n] for n in names if n in self.module_state}})
                if self._pending_wire:
                    w, self._pending_wire = self._pending_wire, []
                    await self.broadcast({"type": "wire", "frames": w})
                if self._pending_log:
                    l, self._pending_log = self._pending_log, []
                    await self.broadcast({"type": "log", "lines": l})
            except Exception as ex:
                print(f"[emu] push error: {ex!r}")
            await asyncio.sleep(0.033)

    # ---- controls ---------------------------------------------------------------------------------------
    def save(self) -> None:
        self.cfg.save(self.data_dir)

    def set_speed(self, speed: float) -> None:
        speed = max(0.1, min(50.0, float(speed)))
        self.cfg.speed = speed
        self.hub.set_speed(speed)
        self.save()

    def set_bus(self, ideal: Optional[bool], drop: Optional[float], noise: Optional[float]) -> None:
        if ideal is not None:
            self.hub.ideal = self.cfg.bus.ideal = bool(ideal)
        if drop is not None:
            self.hub.drop_prob = self.cfg.bus.drop = max(0.0, min(1.0, float(drop)))
        if noise is not None:
            self.hub.noise_prob = self.cfg.bus.noise = max(0.0, min(1.0, float(noise)))
        self.save()

    async def set_wall(self, rows: int, cols: int) -> None:
        rows = max(1, min(64, int(rows))); cols = max(1, min(64, int(cols)))
        self.cfg.rows, self.cfg.cols = rows, cols
        # modules beyond the wall are removed (unplugged and discarded); empty slots get fresh modules
        for m in [m for m in self.cfg.modules if m.slot >= rows * cols]:
            await self.sup.remove_module(m.sn, wipe=True)
        before = {m.sn for m in self.cfg.modules}
        self.cfg.populate()
        for m in self.cfg.modules:
            if m.sn not in before:
                self.sup._ensure_module(m)
        self.save()

    async def update_mech(self, sn: str, patch: dict) -> ModuleCfg:
        m = self.cfg.module(sn)
        if m is None:
            raise HTTPException(404, "no such module")
        for k, v in patch.items():
            if k in Mech.__dataclass_fields__:
                setattr(m.mech, k, type(getattr(m.mech, k))(v))
        if "label" in patch:
            m.label = str(patch["label"])[:8]
        self.hub.ctl(sn, self.cfg.mech_message(m))
        self.save()
        return m

    async def factory_reset(self) -> None:
        """Every module back to a blank chip, the gateway to a blank flash, a fresh wall."""
        await self.sup.stop()
        for sub in ("modules", "gateway"):
            shutil.rmtree(os.path.join(self.data_dir, sub), ignore_errors=True)
        try:
            os.unlink(EmuConfig.path(self.data_dir))
        except FileNotFoundError:
            pass
        self.module_state.clear()
        rows, cols, speed = self.cfg.rows, self.cfg.cols, self.cfg.speed
        self.cfg = EmuConfig(rows=rows, cols=cols, speed=speed)
        self.cfg.populate()
        self.cfg.save(self.data_dir)
        self.sup = Supervisor(self.cfg, self.hub, self.data_dir, self.bin_dir, self.bus_path, self.gateway_http_port)
        self.sup.on_log = self._on_log
        self.sup._stopping = False
        await self.sup.start()


def create_app(emu: Emulator) -> FastAPI:
    app = FastAPI(title="SplitFlap Emulator", version=__version__)

    @app.on_event("startup")
    async def _startup() -> None:
        await emu.start()

    @app.on_event("shutdown")
    async def _shutdown() -> None:
        await emu.stop()

    # ---- pages -------------------------------------------------------------------------------------------
    @app.get("/")
    async def index() -> FileResponse:
        return FileResponse(os.path.join(STATIC, "index.html"))

    @app.get("/display")
    async def display() -> FileResponse:
        return FileResponse(os.path.join(STATIC, "display.html"))

    app.mount("/static", StaticFiles(directory=STATIC), name="static")

    # ---- state -------------------------------------------------------------------------------------------
    @app.get("/api/emu/state")
    async def state() -> dict:
        return emu.snapshot()

    @app.get("/api/emu/wire")
    async def wire(since: int = 0, limit: int = 200) -> dict:
        items = [f for f in emu.wire if f["seq"] > since][-limit:]
        return {"frames": items, "seq": emu.wire_seq}

    @app.get("/api/emu/log")
    async def log(src: str = "", since: int = 0, limit: int = 300) -> dict:
        items = [l for l in emu.log if l["seq"] > since and (not src or l["src"] == src)][-limit:]
        return {"lines": items, "seq": emu.log_seq}

    # ---- global controls --------------------------------------------------------------------------------
    @app.post("/api/emu/speed")
    async def speed(body: dict) -> dict:
        emu.set_speed(body.get("speed", 1.0))
        return {"speed": emu.clock.speed}

    @app.post("/api/emu/bus")
    async def bus(body: dict) -> dict:
        emu.set_bus(body.get("ideal"), body.get("drop"), body.get("noise"))
        return emu.snapshot()["bus"]

    @app.post("/api/emu/wall")
    async def wall(body: dict) -> dict:
        await emu.set_wall(body.get("rows", emu.cfg.rows), body.get("cols", emu.cfg.cols))
        return {"rows": emu.cfg.rows, "cols": emu.cfg.cols, "modules": len(emu.cfg.modules)}

    @app.post("/api/emu/sound")
    async def sound(body: dict) -> dict:
        emu.cfg.sound = bool(body.get("sound", True)); emu.save()
        return {"sound": emu.cfg.sound}

    @app.post("/api/emu/wall/provision")
    async def wall_provision() -> dict:
        try:
            return emu.jobs.start("provision").snapshot()
        except RuntimeError as ex:
            raise HTTPException(409, str(ex))

    @app.post("/api/emu/wall/calibrate")
    async def wall_calibrate() -> dict:
        try:
            return emu.jobs.start("calibrate").snapshot()
        except RuntimeError as ex:
            raise HTTPException(409, str(ex))

    @app.get("/api/emu/wall/job")
    async def wall_job() -> dict:
        return emu.jobs.current.snapshot() if emu.jobs.current else {"state": "idle"}

    @app.post("/api/emu/wall/job/cancel")
    async def wall_job_cancel() -> dict:
        return {"cancelled": emu.jobs.cancel()}

    @app.post("/api/emu/factory-reset")
    async def factory_reset() -> dict:
        await emu.factory_reset()
        return {"ok": True}

    # ---- modules ------------------------------------------------------------------------------------------
    @app.post("/api/emu/modules/{sn}/mech")
    async def mech(sn: str, body: dict) -> dict:
        m = await emu.update_mech(sn, body)
        return asdict(m)

    @app.post("/api/emu/modules/{sn}/power")
    async def module_power(sn: str, body: dict) -> dict:
        await emu.sup.set_module_power(sn, bool(body.get("on", True)))
        emu.save()
        return {"ok": True}

    @app.post("/api/emu/modules/{sn}/reset")
    async def module_reset(sn: str, body: dict) -> dict:
        kind = body.get("kind", "reset")
        if kind == "powercycle":
            await emu.sup.power_cycle(sn)
        elif kind in ("reset", "brownout", "eeprom"):
            msg = {"op": kind}
            if kind == "brownout":
                msg["corrupt"] = bool(body.get("corrupt", False))
            if not emu.hub.ctl(sn, msg):
                raise HTTPException(409, "module is not on the bus")
        else:
            raise HTTPException(400, "kind must be reset | brownout | eeprom | powercycle")
        return {"ok": True}

    @app.post("/api/emu/modules")
    async def module_add(body: dict) -> dict:
        import random
        rng = random.Random()
        slot = int(body.get("slot", len(emu.cfg.modules)))
        if any(m.slot == slot for m in emu.cfg.modules):
            raise HTTPException(409, "slot occupied")
        m = ModuleCfg(sn=body.get("sn") or EmuConfig.new_serial(rng), slot=slot, mech=EmuConfig.new_mech(rng))
        await emu.sup.add_module(m)
        emu.save()
        return asdict(m)

    @app.delete("/api/emu/modules/{sn}")
    async def module_remove(sn: str, wipe: bool = True) -> dict:
        await emu.sup.remove_module(sn, wipe=wipe)
        emu.module_state.pop(sn, None)
        emu.save()
        return {"ok": True}

    @app.get("/api/emu/modules/{sn}/eeprom")
    async def module_eeprom(sn: str) -> dict:
        p = os.path.join(emu.data_dir, "modules", sn + ".eeprom")
        if not os.path.exists(p):
            raise HTTPException(404, "no EEPROM image yet")
        with open(p, "rb") as f:
            data = f.read()
        return {"sn": sn, "hex": data.hex(), "bytes": len(data)}

    # ---- gateway -------------------------------------------------------------------------------------------
    @app.post("/api/emu/gateway/power")
    async def gw_power(body: dict) -> dict:
        await emu.sup.set_gateway_power(bool(body.get("on", True)))
        emu.save()
        return {"ok": True}

    @app.post("/api/emu/gateway/reset")
    async def gw_reset(body: dict) -> dict:
        kind = body.get("kind", "reboot")
        if kind == "powercycle":
            await emu.sup.power_cycle("gateway")
        elif not emu.hub.ctl("gateway", {"op": "reboot"}):
            raise HTTPException(409, "gateway is not running")
        return {"ok": True}

    @app.post("/api/emu/gateway/wifi")
    async def gw_wifi(body: dict) -> dict:
        if "up" in body:
            emu.cfg.gateway.wifi = bool(body["up"])
            emu.hub.ctl("gateway", {"op": "wifi", "up": emu.cfg.gateway.wifi})
        if "rssi" in body:
            emu.cfg.gateway.rssi = int(body["rssi"])
            emu.hub.ctl("gateway", {"op": "rssi", "rssi": emu.cfg.gateway.rssi})
        emu.save()
        return asdict(emu.cfg.gateway)

    @app.get("/api/emu/gateway/nvs")
    async def gw_nvs() -> Any:
        p = os.path.join(emu.data_dir, "gateway", "nvs", "splitflap.json")
        if not os.path.exists(p):
            return {}
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)

    # ---- live stream ---------------------------------------------------------------------------------------
    @app.websocket("/ws")
    async def ws(sock: WebSocket) -> None:
        await sock.accept()
        emu.clients.add(sock)
        try:
            await sock.send_text(json.dumps({"type": "snapshot", "data": emu.snapshot(),
                                             "wire": list(emu.wire)[-100:]}, separators=(",", ":")))
            while True:
                msg = await sock.receive_text()
                if msg == "snapshot":
                    await sock.send_text(json.dumps({"type": "snapshot", "data": emu.snapshot()}, separators=(",", ":")))
        except WebSocketDisconnect:
            pass
        except Exception:
            pass
        finally:
            emu.clients.discard(sock)

    return app
