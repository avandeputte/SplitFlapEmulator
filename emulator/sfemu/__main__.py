"""python -m sfemu -- run the emulator.

  --data DIR         persistent state (default: $SFEMU_DATA or ./data)
  --bin DIR          where sfmodule / sfgateway live (default: $SFEMU_BIN or ../bin)
  --port N           the emulator's own web port (default 8090)
  --gateway-port N   the port the emulated gateway's HTTP server listens on (default 80)
  --gateway-url URL  how a browser reaches the gateway (shown in the control panel)
"""
import argparse
import os

import uvicorn

from .app import Emulator, create_app


def main() -> None:
    ap = argparse.ArgumentParser(prog="sfemu")
    ap.add_argument("--data", default=os.environ.get("SFEMU_DATA", "./data"))
    ap.add_argument("--bin", default=os.environ.get("SFEMU_BIN", os.path.join(os.path.dirname(__file__), "..", "..", "bin")))
    ap.add_argument("--port", type=int, default=int(os.environ.get("SFEMU_PORT", "8090")))
    ap.add_argument("--gateway-port", type=int, default=int(os.environ.get("SFEMU_GATEWAY_PORT", "80")))
    ap.add_argument("--gateway-url", default=os.environ.get("SFEMU_GATEWAY_URL", ""))
    ap.add_argument("--host", default=os.environ.get("SFEMU_HOST", "0.0.0.0"))
    a = ap.parse_args()
    gw_url = a.gateway_url or (f"http://localhost:{a.gateway_port}" if a.gateway_port != 80 else "http://localhost")
    emu = Emulator(os.path.abspath(a.data), os.path.abspath(a.bin), a.gateway_port, gw_url)
    app = create_app(emu)
    uvicorn.run(app, host=a.host, port=a.port, log_level="warning")


if __name__ == "__main__":
    main()
