"""Wall-wide shortcuts that go THROUGH the gateway, like an operator would, only faster:
provision every module with its slot number, or calibrate every module to its physical truth.
Nothing here touches an EEPROM file or a module process directly; the frames on the bus are the
gateway's own (mXI, mXW, mXH), and the gateway's registry sees everything.
"""
from __future__ import annotations

import asyncio
import json
import time
import urllib.request
from typing import Optional


class Job:
    def __init__(self, name: str, total: int):
        self.name = name
        self.total = total
        self.done = 0
        self.state = "running"      # running | done | failed | cancelled
        self.message = ""
        self.started = time.time()
        self.finished = 0.0
        self.task: Optional[asyncio.Task] = None

    def snapshot(self) -> dict:
        return {"name": self.name, "state": self.state, "done": self.done, "total": self.total,
                "message": self.message, "elapsed": (self.finished or time.time()) - self.started}


def _http(method: str, url: str, body=None, timeout: float = 20.0):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, method=method, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read().decode()
        return json.loads(raw) if raw else None


class WallJobs:
    def __init__(self, emu):
        self.emu = emu
        self.current: Optional[Job] = None

    @property
    def gw(self) -> str:
        return f"http://127.0.0.1:{self.emu.gateway_http_port}"

    async def _get(self, path: str):
        return await asyncio.to_thread(_http, "GET", self.gw + path)

    async def _post(self, path: str, body: dict):
        return await asyncio.to_thread(_http, "POST", self.gw + path, body)

    async def _vsleep(self, virtual_s: float) -> None:
        await asyncio.sleep(virtual_s / max(0.1, self.emu.clock.speed))

    def start(self, name: str) -> Job:
        if self.current and self.current.state == "running":
            raise RuntimeError(f"{self.current.name} is still running")
        total = len(self.emu.cfg.modules)
        job = Job(name, total)
        fn = self.provision if name == "provision" else self.calibrate
        job.task = asyncio.create_task(self._run(job, fn))
        self.current = job
        return job

    def cancel(self) -> bool:
        if self.current and self.current.state == "running" and self.current.task:
            self.current.task.cancel()
            return True
        return False

    async def _run(self, job: Job, fn) -> None:
        try:
            await fn(job)
            if job.state == "running":
                job.state = "done"
        except asyncio.CancelledError:
            job.state = "cancelled"
        except Exception as ex:
            job.state = "failed"
            job.message = f"{type(ex).__name__}: {ex}"
        finally:
            job.finished = time.time()

    # ---- provision: mXI<sn>:<slot> for every module, until each one answers on its slot id ----
    async def provision(self, job: Job) -> None:
        cfg = self.emu.cfg
        want = {m.sn: m.slot for m in cfg.modules}
        # The gateway must be up; a module must be booted to hear its frame (a blank one homes
        # first, ~15 s), so the rounds below simply retry the stragglers, like the Provision tab.
        deadline = time.time() + 300 / max(0.1, self.emu.clock.speed)
        rounds = 0
        while time.time() < deadline:
            rounds += 1
            registry = {m["sn"]: m for m in (await self._get("/api/flap/modules") or [])}
            pending = []
            for sn, slot in want.items():
                st = self.emu.module_state.get(sn, {})
                reg = registry.get(sn)
                if st.get("id") == slot and reg and reg.get("provisioned") and reg.get("fwVersion"):
                    continue
                pending.append((sn, slot))
            job.done = job.total - len(pending)
            job.message = f"round {rounds}: {len(pending)} module(s) still to provision"
            if not pending:
                break
            for sn, slot in pending:
                proc = self.emu.sup.procs.get(sn)
                if proc is None or proc.proc is None:
                    continue                     # powered off: nothing to talk to
                st = self.emu.module_state.get(sn, {})
                if st.get("id") == slot:
                    continue                     # it has the id; the gateway just has not confirmed it yet
                await self._post("/api/flap/provision", {"sn": sn, "id": slot})
                await self._vsleep(0.6)          # the module acks ~20 ms later; the gateway then queries it
            # the gateway's post-provision version queries take a few seconds per module
            await self._vsleep(8)
        registry = {m["sn"]: m for m in (await self._get("/api/flap/modules") or [])}
        left = [sn for sn, slot in want.items()
                if not (self.emu.module_state.get(sn, {}).get("id") == slot and registry.get(sn, {}).get("provisioned"))]
        job.done = job.total - len(left)
        # The wall geometry, as an operator would set it on the Settings tab.
        await self._post("/api/config/settings", {"gridRows": cfg.rows, "gridCols": cfg.cols})
        if left:
            job.state = "failed"
            job.message = f"{len(left)} module(s) never confirmed: {', '.join(s[-4:] for s in left[:8])}"
        else:
            job.message = f"all {job.total} modules provisioned by slot; gateway layout set to {cfg.rows} x {cfg.cols}"

    # ---- calibrate: mXW<sn>:<trueOffset>:<trueRev>: then mXH<sn>, verified against the firmware ----
    async def calibrate(self, job: Job) -> None:
        cfg = self.emu.cfg
        targets = {m.sn: (m.mech.offset, m.mech.rev) for m in cfg.modules}
        deadline = time.time() + 240 / max(0.1, self.emu.clock.speed)
        rounds = 0
        while time.time() < deadline:
            rounds += 1
            pending = []
            for sn, (off, rev) in targets.items():
                fw = self.emu.module_state.get(sn, {}).get("fw", {})
                if fw.get("off") == off and fw.get("rev") == rev and fw.get("idx", -1) >= 0 \
                        and self.emu.module_state.get(sn, {}).get("moving") is False:
                    continue
                pending.append(sn)
            job.done = job.total - len(pending)
            job.message = f"round {rounds}: {len(pending)} module(s) to write"
            if not pending:
                break
            for sn in pending:
                proc = self.emu.sup.procs.get(sn)
                if proc is None or proc.proc is None:
                    continue
                off, rev = targets[sn]
                fw = self.emu.module_state.get(sn, {}).get("fw", {})
                if not (fw.get("off") == off and fw.get("rev") == rev):
                    # the gateway's restore-by-serial: offset, steps per revolution, an empty map
                    # (evenly spaced flaps are exact for these reels) -- mXW<sn>:<off>:<rev>:
                    await self._post("/api/flap/restorebysn", {"sn": sn, "homeOffset": off, "totalSteps": rev, "map": ""})
                    await self._vsleep(1.2)      # the module erases its map and writes the fields
                # re-home so the reel picks the new calibration up (mXH<sn>)
                await self._post("/api/flap/homebysn", {"sn": sn})
                await self._vsleep(0.25)
            await self._vsleep(8)                # a homing is up to one revolution plus the offset
        left = [sn for sn, (off, rev) in targets.items()
                if not (self.emu.module_state.get(sn, {}).get("fw", {}).get("off") == off
                        and self.emu.module_state.get(sn, {}).get("fw", {}).get("rev") == rev)]
        job.done = job.total - len(left)
        if left:
            job.state = "failed"
            job.message = f"{len(left)} module(s) did not take the calibration: {', '.join(s[-4:] for s in left[:8])}"
        else:
            job.message = f"every module's home offset and revolution now match its reel; wall homed"
