"""Test fixture: a whole emulator (hub, gateway, a small wall) on free ports, sped up 5x."""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import httpx
import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BIN = os.environ.get("SFEMU_BIN", os.path.join(ROOT, "bin"))
SPEED = float(os.environ.get("SFEMU_TEST_SPEED", "5"))


def free_port() -> int:
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


class Emu:
    def __init__(self, rows=1, cols=4):
        self.data = tempfile.mkdtemp(prefix="sfemu-test-", dir="/tmp")
        self.gw_port = free_port(); self.port = free_port()
        # pre-seed the wall size and pace before the first start
        from sfemu.config import EmuConfig
        cfg = EmuConfig(rows=rows, cols=cols, speed=SPEED); cfg.populate(seed=1); cfg.save(self.data)
        self.proc = subprocess.Popen([sys.executable, "-m", "sfemu", "--data", self.data, "--bin", BIN,
                                      "--gateway-port", str(self.gw_port), "--port", str(self.port), "--host", "127.0.0.1"],
                                     cwd=os.path.join(ROOT, "emulator"), stdout=open(os.path.join(self.data, "emu.log"), "wb"),
                                     stderr=subprocess.STDOUT)
        self.gw = httpx.Client(base_url=f"http://127.0.0.1:{self.gw_port}", timeout=15)
        self.emu = httpx.Client(base_url=f"http://127.0.0.1:{self.port}", timeout=15)
        self.wait_http(self.emu, "/api/emu/state")
        self.wait_http(self.gw, "/api/config")

    @staticmethod
    def wait_http(client, path, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if client.get(path).status_code == 200:
                    return
            except Exception:
                pass
            time.sleep(0.3)
        raise RuntimeError(f"{path} never came up")

    def vsleep(self, virtual_seconds: float) -> None:
        time.sleep(virtual_seconds / SPEED)

    def wait_until(self, pred, virtual_seconds: float, every=0.2):
        end = time.time() + virtual_seconds / SPEED
        while time.time() < end:
            v = pred()
            if v:
                return v
            time.sleep(every)
        return pred()

    def state(self):
        return self.emu.get("/api/emu/state").json()

    def module_state(self, sn):
        for m in self.state()["modules"]:
            if m["cfg"]["sn"] == sn:
                return m["state"]
        return {}

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        if os.environ.get("SFEMU_KEEP"):           # keep the data dir and print the emulator log
            print(f"\n[emu] data kept at {self.data}")
            with open(os.path.join(self.data, "emu.log"), "rb") as f:
                print(f.read().decode("latin-1")[-6000:])
            return
        shutil.rmtree(self.data, ignore_errors=True)


@pytest.fixture(scope="module")
def emu():
    sys.path.insert(0, os.path.join(ROOT, "emulator"))
    e = Emu(rows=1, cols=4)
    yield e
    e.close()
