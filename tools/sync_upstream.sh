#!/usr/bin/env sh
# Refresh the vendored firmware sources from sibling checkouts.
#   tools/sync_upstream.sh [path/to/SplitFlapGateway] [path/to/SplitFlapUniversalFirmware-src-dir]
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
GW=${1:-"$HERE/../SplitFlapGateway"}
FW=${2:-"$HERE/../SplitFlapUniversalFirmware"}
cp "$GW"/src/*.cpp "$GW"/src/*.h "$HERE/vendor/gateway/src/"
cp "$GW"/openapi.yaml "$HERE/vendor/gateway/"
if [ -f "$FW/src/SplitFlapUniversalFirmware.cpp" ]; then cp "$FW/src/SplitFlapUniversalFirmware.cpp" "$HERE/vendor/firmware/";
elif [ -f "$FW/SplitFlapUniversalFirmware.cpp" ]; then cp "$FW/SplitFlapUniversalFirmware.cpp" "$HERE/vendor/firmware/";
else echo "firmware .cpp not found under $FW" >&2; exit 1; fi
[ -f "$FW/README.md" ] && cp "$FW/README.md" "$HERE/vendor/firmware/README.md"
echo "gateway: $(grep -o 'FW_VERSION *"[^"]*"' "$HERE/vendor/gateway/src/common.h")"
echo "module:  $(grep -o 'FIRMWARE_VERSION\[\] = "[^"]*"' "$HERE/vendor/firmware/SplitFlapUniversalFirmware.cpp")"
echo "Now rebuild: make -C module && make -C gateway   (or docker compose build)"
