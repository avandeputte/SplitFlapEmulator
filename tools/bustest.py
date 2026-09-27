#!/usr/bin/env python3
"""Smoke test: boot two emulated modules on a bus hub, act as the gateway, provision one, move it.

    python3 tools/bustest.py [--speed 5] [--modules 2]
"""
import argparse, asyncio, os, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "emulator"))
from sfemu.bus import BusHub
from sfemu.vclock import VirtualClock

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SFMODULE = os.path.join(ROOT, "module", "build", "sfmodule")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speed", type=float, default=5.0)
    ap.add_argument("--modules", type=int, default=2)
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="sfemu-", dir="/tmp")
    sock = os.path.join(tmp, "bus.sock")
    clock = VirtualClock(a.speed)
    hub = BusHub(sock, clock)
    frames = asyncio.Queue()
    hub.on_frame = lambda f: frames.put_nowait(f)
    states = {}
    hub.on_state = lambda name, st: states.__setitem__(name, st)
    hub.on_client = lambda kind, name, up: print(f"  [hub] {kind} {name} {'connected' if up else 'gone'}")
    hub.on_log = lambda name, t: print(f"  [log {name}] {t}")
    await hub.start()

    procs = []
    serials = [f"{i:02X}F24C0018E7D29B3F{i:02X}" for i in range(1, a.modules + 1)]
    for sn in serials:
        logf = open(os.path.join(tmp, sn + ".log"), "wb")
        p = subprocess.Popen([SFMODULE, "--serial", sn, "--data", tmp, "--bus", sock, "--cause", "1"],
                             stdout=logf, stderr=subprocess.STDOUT)
        print(f"   module {sn} log: {logf.name}")
        procs.append(p)
    # A pretend gateway on the bus
    r, w = await asyncio.open_unix_connection(sock)
    w.write(b"HELLO gateway test\n")

    async def gw_reader():
        while True:
            line = await r.readline()
            if not line:
                break
    asyncio.create_task(gw_reader())

    def send(frame: bytes):
        w.write(f"TX {clock.now_us()} 9600 {frame.hex()}\n".encode())

    async def wait_frames(seconds, label):
        end = time.monotonic() + seconds
        got = []
        while time.monotonic() < end:
            try:
                f = await asyncio.wait_for(frames.get(), timeout=max(0.01, end - time.monotonic()))
            except asyncio.TimeoutError:
                break
            print(f"  [wire] {f['from'][:8]:8s} {'CORRUPT ' if f['corrupt'] else ''}{f['data']!r}")
            got.append(f)
        return got

    try:
        async def watch_procs():
            while True:
                for sn, p in zip(serials, procs):
                    rc = p.poll()
                    if rc is not None:
                        print(f"   !! module {sn} exited rc={rc}; log tail:")
                        print(open(os.path.join(tmp, sn + ".log"), "rb").read().decode("latin-1")[-600:])
                        return
                await asyncio.sleep(0.5)
        asyncio.create_task(watch_procs())
        print(f"== waiting for boot (auto-measure + home, ~10 s virtual) and advertisements at {a.speed}x")
        got = await wait_frames(70 / a.speed, "adverts")
        adv = [f for f in got if f["data"].startswith("mXadv:")]
        assert adv, "no advertisement seen"
        print(f"== {len(adv)} advertisement(s). Broadcasting m*v (unprovisioned modules must stay silent)")
        send(b"m*v\n")
        got = await wait_frames(2 / a.speed, "m*v")
        assert not [f for f in got if "v:" in f["data"]], "unprovisioned module answered m*v"
        sn = serials[0]
        print(f"== provisioning {sn} as id 5")
        send(f"mXI{sn}:5\n".encode())
        got = await wait_frames(2 / a.speed, "ack")
        assert any(f["data"].startswith(f"mXack:{sn}:5") for f in got), "no provisioning ack"
        print("== direct version query m5v (bare, no newline, like the gateway sends it)")
        send(b"m5v")
        got = await wait_frames(1.5 / a.speed, "version")
        assert any(f["data"].startswith("m05v:32:5:") for f in got), "no version reply"
        print("== m5-B: reel must turn to flap 2")
        send(b"m5-B\n")
        await asyncio.sleep(1.0 / a.speed)
        for _ in range(60):
            st = states.get(sn, {})
            if st.get("fw", {}).get("idx") == 2 and not st.get("moving"):
                break
            await asyncio.sleep(0.1)
        st = states.get(sn, {})
        print(f"   state: id={st.get('id')} fwIdx={st.get('fw',{}).get('idx')} pos={st.get('pos')} off={st.get('off')} rev={st.get('rev')}")
        assert st.get("fw", {}).get("idx") == 2, "firmware did not land on flap 2"
        shown = ((st["pos"] - st["off"]) % st["rev"]) // (st["rev"] // st["flaps"])
        print(f"   physically showing flap index {shown} (default offset matches the reel exactly)")
        print("== m5A: combined dump")
        send(b"m5A\n")
        got = await wait_frames(2 / a.speed, "A")
        assert any(f["data"].startswith("m05A:32:5:") for f in got), "no A reply"
        print("== m*A broadcast then a colliding advert window: just observe")
        send(b"m*A\n")
        await wait_frames(3 / a.speed, "A*")
        print("== stats", hub.stats)
        print("SMOKE TEST PASSED")
    except Exception:
        import traceback
        traceback.print_exc()
        raise
    finally:
        for p in procs:
            p.terminate()
        w.close()
        await hub.stop()

asyncio.run(main())
