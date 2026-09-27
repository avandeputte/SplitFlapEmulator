# Vendored upstream sources

Everything under `vendor/` is copied **verbatim** from its upstream project. The emulator
never patches these files: fidelity comes from running the real code, and refreshing to a
newer release is a copy (`tools/sync_upstream.sh`).

| Directory | Source | Version | Licence |
|---|---|---|---|
| `firmware/` | [SplitFlapUniversalFirmware](https://github.com/avandeputte/SplitFlapUniversalFirmware) `src/SplitFlapUniversalFirmware.cpp` | v32 | CC BY-NC-SA 4.0 |
| `gateway/src/` | [SplitFlapGateway](https://github.com/avandeputte/SplitFlapGateway) `src/` (incl. the generated `web_ui.h`) | 3.13.1 | CC BY-NC-SA 4.0 |
| `gateway/openapi.yaml` | SplitFlapGateway | 3.13.1 | CC BY-NC-SA 4.0 |
| `ArduinoJson/` | [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson) `src/` | 7.4.3 | MIT |
| `PubSubClient/` | [knolleary/PubSubClient](https://github.com/knolleary/pubsubclient) `src/` | 2.8 | MIT |
| `esp32core/` | [arduino-esp32](https://github.com/espressif/arduino-esp32) `cores/esp32/` — `WString`, `Print`, `Stream`, `Printable`, `Client`, `Server`, `stdlib_noniso`, `WCharacter` | 3.3.9 | LGPL 2.1 |
| `WebServer/` | arduino-esp32 `libraries/WebServer/src/` | 3.3.9 | LGPL 2.1 |
| `gateway/shim/http_parser.h` (not vendored; generated) | the `http_method` enum of ESP-IDF's `http_parser` | — | MIT |

The module firmware's `platformio.ini` sets `_SS_MAX_RX_BUFF=128`; the emulator builds with the
same value (see `module/Makefile`).
