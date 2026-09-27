#!/usr/bin/env python3
"""Provision every advertising module with its slot number, through the gateway's own API.

    python3 tools/provision_wall.py [--gateway http://localhost:8080] [--emulator http://localhost:8090] [--layout]

Waits for each module of the emulated wall to advertise (blank modules advertise every 10-15 s),
sends the gateway's POST /api/flap/provision (mXI<sn>:<id>) for it, and optionally sets the
gateway's display layout to the wall's rows x cols. It is what you would do by hand on the
Provision tab, just faster; nothing here bypasses the gateway.
"""
import argparse, json, time, urllib.request


def http(method, url, body=None):
    req = urllib.request.Request(url, method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode() or "null")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gateway", default="http://localhost:8080")
    ap.add_argument("--emulator", default="http://localhost:8090")
    ap.add_argument("--layout", action="store_true", help="also set the gateway display layout to the wall size")
    ap.add_argument("--timeout", type=float, default=240)
    a = ap.parse_args()
    emu = http("GET", a.emulator + "/api/emu/state")
    slots = {m["cfg"]["sn"]: m["cfg"]["slot"] for m in emu["modules"]}
    done = set()
    t0 = time.time()
    while len(done) < len(slots) and time.time() - t0 < a.timeout:
        for m in http("GET", a.gateway + "/api/flap/modules"):
            sn = m["sn"]
            if m["provisioned"] or sn in done or sn not in slots:
                continue
            r = http("POST", a.gateway + "/api/flap/provision", {"sn": sn, "id": slots[sn]})
            print(f"slot {slots[sn]:3d} <- {sn}  {r}")
            done.add(sn)
            time.sleep(0.6)     # the module re-provisions and the gateway queries its version
        time.sleep(2)
    print(f"{len(done)} of {len(slots)} modules provisioned in {time.time() - t0:.0f} s")
    if a.layout:
        print(http("POST", a.gateway + "/api/config/settings", {"gridRows": emu["rows"], "gridCols": emu["cols"]}))


if __name__ == "__main__":
    main()
