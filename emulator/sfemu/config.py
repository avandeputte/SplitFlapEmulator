"""Emulator configuration: the wall (which modules sit in which slot, and what each one's
mechanics are), the gateway board, the bus, and the pace of time. Persisted as
<data>/emulator.json; module EEPROMs and the gateway's NVS/FATFS live next to it.
"""
from __future__ import annotations

import json
import os
import random
from dataclasses import dataclass, field, asdict
from typing import Optional

DEFAULT_FLAP_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$&()-+=;q:%'.,/?*roygbpw"


@dataclass
class Mech:
    """The physical truth about one module -- what the firmware has to discover by calibrating."""
    rev: int = 4076            # half-steps per revolution (a 28BYJ-48's 63.684:1 gear train)
    offset: int = 2832         # half-steps from the Hall edge to flap 0 dead centre
    magnet: int = 160          # half-steps the Hall sensor stays active
    hallLow: bool = True       # sensor pulls LOW at the magnet
    flaps: int = 64
    chars: str = DEFAULT_FLAP_CHARS   # what is printed on the reel (Windows-1252 text)
    slip: float = 0.0          # probability of a missed half-step
    hall: str = "ok"           # ok | stuck_active | stuck_inactive | noisy | inverted
    motor: str = "ok"          # ok | dead
    vcc: int = 4980            # supply millivolts


@dataclass
class ModuleCfg:
    sn: str
    slot: int
    power: bool = True
    mech: Mech = field(default_factory=Mech)
    label: str = ""            # optional sticker text shown on the module


@dataclass
class GatewayCfg:
    power: bool = True
    ip: str = ""               # what WiFi.localIP() reports ("" = auto)
    mac: str = ""              # "" = generated once and kept
    rssi: int = -58
    wifi: bool = True


@dataclass
class BusCfg:
    ideal: bool = False
    drop: float = 0.0
    noise: float = 0.0


@dataclass
class EmuConfig:
    speed: float = 1.0
    rows: int = 3
    cols: int = 15
    modules: list[ModuleCfg] = field(default_factory=list)
    gateway: GatewayCfg = field(default_factory=GatewayCfg)
    bus: BusCfg = field(default_factory=BusCfg)
    sound: bool = True

    # ---- persistence ------------------------------------------------------------------------
    @staticmethod
    def path(data_dir: str) -> str:
        return os.path.join(data_dir, "emulator.json")

    @classmethod
    def load(cls, data_dir: str) -> "EmuConfig":
        p = cls.path(data_dir)
        if not os.path.exists(p):
            cfg = cls()
            cfg.populate()
            cfg.save(data_dir)
            return cfg
        with open(p, "r", encoding="utf-8") as f:
            raw = json.load(f)
        cfg = cls(
            speed=float(raw.get("speed", 1.0)),
            rows=int(raw.get("rows", 3)),
            cols=int(raw.get("cols", 15)),
            gateway=GatewayCfg(**{k: v for k, v in raw.get("gateway", {}).items() if k in GatewayCfg.__dataclass_fields__}),
            bus=BusCfg(**{k: v for k, v in raw.get("bus", {}).items() if k in BusCfg.__dataclass_fields__}),
            sound=bool(raw.get("sound", True)),
        )
        for m in raw.get("modules", []):
            mech = Mech(**{k: v for k, v in m.get("mech", {}).items() if k in Mech.__dataclass_fields__})
            cfg.modules.append(ModuleCfg(sn=m["sn"], slot=int(m.get("slot", 0)), power=bool(m.get("power", True)),
                                         mech=mech, label=m.get("label", "")))
        return cfg

    def save(self, data_dir: str) -> None:
        os.makedirs(data_dir, exist_ok=True)
        tmp = self.path(data_dir) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(asdict(self), f, indent=2)
        os.replace(tmp, self.path(data_dir))

    # ---- wall population ----------------------------------------------------------------------
    @staticmethod
    def new_serial(rng: random.Random) -> str:
        return "".join(rng.choice("0123456789ABCDEF") for _ in range(20))

    @staticmethod
    def new_mech(rng: random.Random) -> Mech:
        # Every real module is a little different: where the magnet sits relative to flap 0, how
        # wide the sensor's active region is, how many steps its gear train really has.
        return Mech(
            rev=4076 + rng.randint(-4, 4),
            offset=2832 + rng.randint(-40, 40),
            magnet=rng.randint(140, 185),
        )

    def populate(self, seed: Optional[int] = None) -> None:
        """Fill every empty slot of the rows x cols wall with a fresh module."""
        rng = random.Random(seed if seed is not None else os.urandom(4).hex())
        used = {m.slot for m in self.modules}
        sns = {m.sn for m in self.modules}
        for slot in range(self.rows * self.cols):
            if slot in used:
                continue
            sn = self.new_serial(rng)
            while sn in sns:
                sn = self.new_serial(rng)
            sns.add(sn)
            self.modules.append(ModuleCfg(sn=sn, slot=slot, mech=self.new_mech(rng)))
        self.modules.sort(key=lambda m: m.slot)

    def module(self, sn: str) -> Optional[ModuleCfg]:
        for m in self.modules:
            if m.sn == sn:
                return m
        return None

    def mech_message(self, m: ModuleCfg) -> dict:
        """The CTL message that hands a module process its mechanics."""
        d = asdict(m.mech)
        d["charsHex"] = m.mech.chars.encode("cp1252", errors="replace").hex()
        d.pop("chars", None)
        d["op"] = "mech"
        return d
