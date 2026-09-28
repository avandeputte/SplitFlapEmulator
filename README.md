# SplitFlap Emulator

A Docker-packaged emulator of a **[Split-Flap Gateway](https://github.com/avandeputte/SplitFlapGateway)**
driving a wall of modules that run the **[SplitFlapUniversalFirmware](https://github.com/avandeputte/SplitFlapUniversalFirmware)**,
with a virtual split-flap wall to watch.

It is not a re-implementation. The gateway firmware (v3.13.1) and the module firmware (v32) are
compiled **unmodified** and run natively, each module in its own process, talking over an emulated
RS-485 bus that models the wire: 9600 baud byte timing, half-duplex collisions, the modules'
staggered reply slots, their 50 ms and 200 ms idle timeouts, the 128-byte receive ring that
overflows when a module is busy stepping, and the four seconds a reel needs for a revolution.
Every screen of the gateway dashboard, every REST endpoint, MQTT topic and Home Assistant
discovery message is the real thing, so the
[companion app](https://github.com/avandeputte/SplitFlapGatewayCompanion) drives the emulator
exactly as it drives hardware.

It is a single Docker image, nothing else runs inside it.

```
 browser ──► :8080  gateway dashboard / REST / OTA        (the real firmware, port 80 inside)
 browser ──► :8090  emulator control panel  /             (faults, time, wall size, wire monitor)
                    virtual wall            /display
```

## Quick start

```sh
docker run -d --name splitflap-emulator -p 8080:80 -p 8090:8090 -v emulator-data:/data \
  ghcr.io/avandeputte/splitflap-emulator:latest
# or, from this repository:  docker compose up -d   (builds the same image)

open http://localhost:8080      # the gateway, as shipped: no WiFi, no modules known yet
open http://localhost:8090      # the emulator's control panel
```

Then do what you would do with a new wall:

1. The modules boot factory-fresh: blank EEPROM, unprovisioned. Each homes, measures its own
   revolution, then advertises its serial number every 10–15 s. Within half a minute they appear
   on the gateway's **Provision** tab.
2. Provision them (the slot number printed on each module's top face is the natural id).
3. Set the wall geometry in **Settings ▸ Display Layout** (the emulator's default wall is 3 × 15).
4. Type something on the **Display** tab and watch it on the virtual wall at
   `http://localhost:8090/display`.
5. Calibrate. Every module's home offset is a little off, as on real hardware; the **Calibration**
   wizard has something real to fix, and the wall shows the half-turned flaps until it is fixed.

To drive it from the [companion app](https://github.com/avandeputte/SplitFlapGatewayCompanion),
run the companion wherever you normally do and point its `GATEWAY_URL` at the emulator's gateway
port (`http://<host>:8080`); it registers itself and the gateway grows its **Companion** tab. An
MQTT broker is likewise yours to supply: enter it on the gateway's Settings tab.

## What is emulated, and how faithfully

| Layer | What runs | Notes |
|---|---|---|
| Gateway firmware | `vendor/gateway/src` (3.13.1), unmodified, built against `gateway/shim` | The ESP32 core's own `WebServer`, `String`, `Print`, `Stream` are used verbatim; FreeRTOS tasks are threads, NVS is a JSON file, FATFS a directory, the PCF85063 RTC is emulated on the I2C shim and keeps time across power cycles. |
| Module firmware | `vendor/firmware` (v32), unmodified, built against `module/shim` | One process per module. The stepper is decoded from the four coil pins, the Hall sensor from the reel angle, EEPROM is a 256-byte file with the part's ~3.5 ms write time and the chip's exact layout (the sketch is compiled with the AVR's 16-bit `int`), the 2 s watchdog resets the process, `RSTCTRL.RSTFR` carries the true reset cause. |
| RS-485 bus | `emulator/sfemu/bus.py` | Byte-exact timing at each talker's baud rate, collision corruption when two talkers overlap, a wrong baud rate produces framing garbage. Switchable to an ideal bus; optional random loss and noise for robustness testing. |
| Time | `common/vclock.h`, `emulator/sfemu/vclock.py` | One virtual clock shared by every process. 1× is the physical pace; the control panel can run ¼× to 20×. |
| Virtual wall | `emulator/static/display.html` | Drawn from the **mechanics** (reel angle, true offset), not from what any firmware believes. Miscalibration shows. Flip animation per half-step, optional click sound. |
| WiFi, NTP, OTA, mDNS | shims | The station "associates" 1.5 s after boot, NTP is the host clock, the browser OTA upload is received, size- and magic-byte-checked, then the gateway reboots (on its built-in firmware). ArduinoOTA push is advertised but not served. |
| Not emulated | — | The ESP32 heap/stack numbers on the Status page are plausible constants. There is no real radio, no AP to join. |

### The physical model

Each module has a `mech` record the firmware cannot see and must calibrate against:

* `rev` — true half-steps per revolution (default 4076 ± 4: a 28BYJ-48's 63.68:1 gear train, not
  the nominal 4096; a blank module measures this itself on first boot, as v30+ firmware does)
* `offset` — true half-steps from the Hall edge to flap 0 (default 2832 ± 40)
* `magnet` — width of the sensor's active region; `hallLow` — sensor polarity
* `flaps`, `chars` — what is printed on the reel
* faults: `hall` (`stuck_active`, `stuck_inactive`, `noisy`, `inverted`), `motor` (`dead`),
  `slip` (probability of a missed half-step), `vcc`

All of it is editable live in the control panel (like re-mounting a magnet); **Perfect mechanics** sets a reel's true offset and revolution to exactly what its firmware believes, so nothing needs calibrating. Every module can
be power-cycled, reset, brown-outed (optionally corrupting one EEPROM byte, the classic BOD-off
failure), or have its EEPROM blanked. The module's `T`, `Q` and `M` self-tests report what they
would on such hardware.

## Control panel and API

`http://localhost:8090` shows the gateway board, the wall, the bus, every module (what it is
physically showing versus what its firmware thinks), a wire monitor of every frame on the pair,
and the console of any device. The same is available as JSON:

| Endpoint | |
|---|---|
| `GET /api/emu/state` | everything |
| `POST /api/emu/speed {speed}` | pace of time (0.1–50) |
| `POST /api/emu/bus {ideal, drop, noise}` | bus physics |
| `POST /api/emu/wall {rows, cols}` | resize the wall (new slots get fresh modules) |
| `POST /api/emu/modules/{sn}/mech {…}` | change a module's physical truth or faults |
| `POST /api/emu/modules/{sn}/power {on}` · `/reset {kind: reset\|brownout\|eeprom\|powercycle}` | |
| `POST /api/emu/gateway/power {on}` · `/reset {kind: reboot\|powercycle}` · `/wifi {up, rssi}` | |
| `POST /api/emu/factory-reset` | blank every EEPROM, the gateway's flash, a fresh wall |
| `GET /api/emu/wire?since=` · `GET /api/emu/log?src=` · `WS /ws` | wire frames, consoles, live stream |

## Layout of this repository

```
vendor/        the upstream sources, verbatim (see vendor/UPSTREAM.md)
module/        Makefile + shim/: the ATtiny1616 "hardware" one module runs on
gateway/       Makefile + shim/: the ESP32-S3 "hardware" the gateway runs on
common/        the virtual clock and the bus client shared by both
emulator/      the Python supervisor, bus hub, control API, control panel and virtual wall
tests/         end-to-end tests that drive the real gateway REST API
tools/         bustest.py (bus-level smoke test), sync_upstream.sh
```

## Running without Docker

```sh
make -C module && make -C gateway && mkdir -p bin && cp module/build/sfmodule gateway/build/sfgateway bin/
python3 -m venv venv && venv/bin/pip install -r emulator/requirements.txt
cd emulator && ../venv/bin/python -m sfemu --data ../data --bin ../bin --gateway-port 8080 --port 8090
```

Tests: `venv/bin/pip install pytest httpx && venv/bin/python -m pytest tests` (they run at 5× time;
`SFEMU_TEST_SPEED` changes that).

## Updating to a new firmware release

```sh
tools/sync_upstream.sh ../SplitFlapGateway ../SplitFlapUniversalFirmware
docker compose build
```

If a release starts using an ESP32 or AVR API the shims do not provide, the build says so; the
shims live in `gateway/shim` and `module/shim` and are small.

## Licence

CC BY-NC-SA 4.0, like the firmware it embeds. See `LICENSE` and `vendor/UPSTREAM.md`.
Split-flap module hardware and the original protocol by Adam G Makes.
