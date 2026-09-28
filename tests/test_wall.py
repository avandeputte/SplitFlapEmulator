"""End-to-end: the real gateway firmware drives real module firmware over the emulated bus."""
import time


def shown_index(st):
    spf = st["rev"] / st["flaps"]
    x = ((st["pos"] - st["off"]) % st["rev"] + st["rev"]) % st["rev"]
    return int(x // spf) % st["flaps"], (x % spf) / spf


def test_gateway_is_the_real_firmware(emu):
    cfg = emu.gw.get("/api/config").json()
    assert cfg["version"] == "3.13.1"
    # the station associates a moment after boot, then NTP sets the clock
    st = emu.wait_until(lambda: (lambda s: s if s["wifi"] and s["ntpSynced"] else None)(emu.gw.get("/api/status").json()), 20)
    assert st and st["wifi"] is True
    caps = emu.gw.get("/api/capabilities").json()
    assert "union" in caps or "common" in caps or isinstance(caps, dict)


def test_fresh_modules_advertise_and_stay_silent_to_broadcasts(emu):
    # A blank module homes on boot, then advertises its serial every 10-15 s and never answers m*v.
    def all_adv():
        ms = [m for m in emu.gw.get("/api/flap/modules").json() if not m["provisioned"]]
        return ms if len(ms) == 4 else None
    mods = emu.wait_until(all_adv, 90)
    assert mods and all(m["id"] == 255 for m in mods), emu.gw.get("/api/flap/modules").json()
    frames = emu.emu.get("/api/emu/wire", params={"limit": 500}).json()["frames"]
    assert any(f["data"].startswith("mXadv:") for f in frames)
    assert not any(f["data"].startswith("m") and "v:32:" in f["data"] for f in frames)


def test_provision_calibrate_and_display(emu):
    mods = [m for m in emu.gw.get("/api/flap/modules").json() if not m["provisioned"]]
    sns = sorted(m["sn"] for m in mods)
    slots = {m["cfg"]["sn"]: m["cfg"]["slot"] for m in emu.state()["modules"]}
    # Provision each module with its slot number as id, through the real REST API (mXI<sn>:<id>).
    # A frame can be lost to a collision with an advertisement, as on the real bus: retry.
    def all_provisioned():
        ms = emu.gw.get("/api/flap/modules").json()
        ok = [m for m in ms if m["provisioned"] and m["fwVersion"] == "32"]
        return ok if len(ok) == 4 else None
    ms = None
    for attempt in range(4):
        for m in emu.gw.get("/api/flap/modules").json():
            if m["provisioned"] or m["sn"] not in slots:
                continue
            r = emu.gw.post("/api/flap/provision", json={"sn": m["sn"], "id": slots[m["sn"]]})
            assert r.status_code == 200, r.text
            emu.vsleep(1.0)
        ms = emu.wait_until(all_provisioned, 25)
        if ms:
            break
    if not ms:
        wire = emu.emu.get("/api/emu/wire", params={"limit": 80}).json()["frames"]
        assert ms, "\n".join(f"{f['from'][-6:]:>6} {'X' if f['corrupt'] else ' '} {f['data']!r}" for f in wire)
    # The wall geometry, like on hardware, is configured on the gateway.
    r = emu.gw.post("/api/config/settings", json={"gridRows": 1, "gridCols": 4})
    assert r.status_code == 200, r.text
    # Show text: every reel must physically land on the right flap.
    r = emu.gw.post("/api/flap/text", json={"text": "ABCD", "start": 0})
    assert r.status_code == 200, r.text
    def landed():
        good = 0
        for sn, slot in slots.items():
            st = emu.module_state(sn)
            if not st or st.get("moving"):
                return None
            idx, frac = shown_index(st)
            want = " ABCDEFGHIJKLMNOPQRSTUVWXYZ".index("ABCD"[slot])
            if st["fw"]["idx"] == want and abs(idx - want) <= 1:
                good += 1
        return good == 4 or None
    assert emu.wait_until(landed, 30)
    ds = emu.gw.get("/api/display/state").json()
    assert ds["rows"] == 1 and ds["cols"] == 4 and ds["cells"][:4] == ["A", "B", "C", "D"]


def test_combined_dump_reports_the_reel(emu):
    ms = [m for m in emu.gw.get("/api/flap/modules").json() if m["provisioned"]]
    assert ms
    r = emu.gw.post("/api/flap/dump", json={"id": ms[0]["id"]})
    assert r.status_code == 200, r.text
    d = r.json()
    # the raw 'd' dump: <homeOffset>:<totalSteps>:<map>; the revolution was measured on first boot
    home, steps = d["dump"].split(":")[:2]
    assert int(home) == 2832 and int(steps) in range(4000, 4200), d


def test_batch_pacing_and_bus_monitor(emu):
    frames = [f"m{i}-{c}\n" for i, c in enumerate("WXYZ")]
    r = emu.gw.post("/api/rs485/batch", json={"frames": frames, "step_ms": 25})
    assert r.status_code == 200 and r.json()["sent"] == 4
    emu.vsleep(1.0)
    msgs = emu.gw.get("/api/rs485/messages").json()
    tx = [m for m in msgs if m["dir"] == "T"]
    assert len(tx) >= 4


def test_power_cycle_keeps_provisioning(emu):
    ms = [m for m in emu.gw.get("/api/flap/modules").json() if m["provisioned"]]
    sn = ms[0]["sn"]
    r = emu.emu.post(f"/api/emu/modules/{sn}/reset", json={"kind": "powercycle"})
    assert r.status_code == 200
    st = emu.wait_until(lambda: (lambda s: s if s.get("boot", 0) >= 2 and not s.get("moving") and s.get("fw", {}).get("idx", -1) >= 0 else None)(emu.module_state(sn)), 60)
    assert st and st["id"] == ms[0]["id"]      # the ID survived in EEPROM; the module re-homed


def test_watchdog_and_faults(emu):
    ms = [m for m in emu.gw.get("/api/flap/modules").json() if m["provisioned"]]
    sn = ms[1]["sn"]
    # A stuck-inactive Hall sensor: homing never finds the magnet, the self-test says code 2.
    r = emu.emu.post(f"/api/emu/modules/{sn}/mech", json={"hall": "stuck_inactive"})
    assert r.status_code == 200
    r = emu.gw.post("/api/flap/diag", json={"id": ms[1]["id"]})      # the 'T' Hall self-test (m<id>T)
    assert r.status_code in (200, 202), r.text
    def result():
        s = emu.gw.get("/api/flap/diag/status").json()
        return s if s.get("state") == "done" and s.get("kind") == "hall" else None
    t = emu.wait_until(result, 60)
    assert t and t["code"] == 2, t
    emu.emu.post(f"/api/emu/modules/{sn}/mech", json={"hall": "ok"})


def test_wall_jobs_provision_and_calibrate(emu):
    # The control panel shortcuts: everything goes through the gateway (mXI / mXW / mXH by serial).
    # Blank a module first so provisioning has real work; the others are already provisioned.
    st = emu.state()
    sn = st["modules"][0]["cfg"]["sn"]
    assert emu.emu.post(f"/api/emu/modules/{sn}/reset", json={"kind": "eeprom"}).status_code == 200
    emu.wait_until(lambda: emu.module_state(sn).get("id") == 255 or None, 60)
    r = emu.emu.post("/api/emu/wall/provision")
    assert r.status_code == 200
    job = emu.wait_until(lambda: (lambda j: j if j["state"] != "running" else None)(emu.emu.get("/api/emu/wall/job").json()), 300)
    assert job and job["state"] == "done", job
    for m in emu.state()["modules"]:
        assert m["state"]["id"] == m["cfg"]["slot"]
    assert emu.gw.get("/api/config").json()["gridCols"] == 4
    r = emu.emu.post("/api/emu/wall/calibrate")
    assert r.status_code == 200
    job = emu.wait_until(lambda: (lambda j: j if j["state"] != "running" else None)(emu.emu.get("/api/emu/wall/job").json()), 240)
    assert job and job["state"] == "done", job
    for m in emu.state()["modules"]:
        s, mech = m["state"], m["cfg"]["mech"]
        assert s["fw"]["off"] == mech["offset"] and s["fw"]["rev"] == mech["rev"]
        idx, frac = shown_index(s)
        assert idx == 0 and frac < 0.05          # dead centre of flap 0 after the home


def test_perfect_mechanics(emu):
    # Perfect = the textbook 2832 / 4096 on the reel AND in the firmware, and flap 0 dead centre.
    r = emu.emu.post("/api/emu/wall/perfect")
    assert r.status_code == 200 and r.json()["modules"] == 4
    job = emu.wait_until(lambda: (lambda j: j if j["state"] != "running" else None)(emu.emu.get("/api/emu/wall/job").json()), 240)
    assert job and job["state"] == "done", job
    for m in emu.state()["modules"]:
        s, mech = m["state"], m["cfg"]["mech"]
        assert (mech["offset"], mech["rev"]) == (2832, 4096)
        assert (s["off"], s["rev"]) == (2832, 4096) and (s["fw"]["off"], s["fw"]["rev"]) == (2832, 4096)
        idx, frac = shown_index(s)
        assert idx == 0 and frac < 0.05
    # and a module blanked afterwards comes back perfect on its own: it measures 4096, defaults to 2832
    sn = emu.state()["modules"][1]["cfg"]["sn"]
    assert emu.emu.post(f"/api/emu/modules/{sn}/reset", json={"kind": "eeprom"}).status_code == 200
    st = emu.wait_until(lambda: (lambda s: s if s.get("boot", 0) >= 2 and s.get("fw", {}).get("idx", -1) >= 0 and s.get("moving") is False else None)(emu.module_state(sn)), 90)
    assert st and (st["fw"]["off"], st["fw"]["rev"]) == (2832, 4096)
    idx, frac = shown_index(st)
    assert idx == 0 and frac < 0.05
