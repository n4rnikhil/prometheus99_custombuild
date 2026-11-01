#!/bin/sh
# Build the Windows release (exe files + installer) from a Linux machine or CI:
#   needs x86_64-w64-mingw32-gcc / -windres (Debian/Ubuntu: gcc-mingw-w64-x86-64) and makensis (nsis)
#   usage: installer/build_windows_release.sh [out-dir]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/dist}
STAGE=$OUT/stage
CC=x86_64-w64-mingw32-gcc
RC=x86_64-w64-mingw32-windres
W="-std=c99 -Wall -Wextra -Wshadow -Werror"
rm -rf "$STAGE" && mkdir -p "$STAGE/examples"

cd "$ROOT/hmi-runtime"
conv() { sed -n "s/^set $1=//p" build.bat | tr -d '\r' | sed 's#\\#/#g'; }
LIB="$(conv CORE) $(conv RENDER) $(conv DEVICES) $(conv FIELDBUS) $(conv HAL)"
$RC res/industrial_hmi.rc -O coff -o "$STAGE/hmi.res"
$CC $W -Os -s -ffunction-sections -fdata-sections -Wl,--gc-sections -fstack-protector-strong -static \
    -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -DHMI_BUILD_ID=\"release-win64\" -I include \
    src/app/app.c src/platform/win32/main_win32.c $LIB "$STAGE/hmi.res" $(conv GCC_LIBS) -mwindows \
    -o "$STAGE/industrial_hmi.exe"
cp res/industrial_hmi.ico "$STAGE/"
cp examples/*.cfg "$STAGE/examples/"
cp examples/bench.cfg "$STAGE/hmi.cfg"

cd "$ROOT/plc-simulator"
$RC res/plc_simulator.rc -O coff -o "$STAGE/sim.res"
$CC $W -Os -s -static -fstack-protector-strong -I include src/main_win32.c src/plc_models.c src/modbus_server.c \
    "$STAGE/sim.res" -luser32 -lgdi32 -lwinmm -lws2_32 -mwindows -o "$STAGE/plc_simulator.exe"
$CC $W -Os -s -static -I include src/main_console.c src/plc_models.c src/modbus_server.c -lws2_32 \
    -o "$STAGE/plc_simulator_cli.exe"
rm -f "$STAGE"/*.res
cp "$ROOT/installer/README-windows.txt" "$STAGE/README.txt"

cd "$OUT"
makensis -V2 -DSTAGE="$STAGE" "$ROOT/installer/prometheus_hmi.nsi"
mv "$ROOT/installer"/Prometheus-HMI-Setup-*.exe "$OUT/" 2>/dev/null || true
(cd "$STAGE" && sha256sum *.exe >> "$OUT/SHA256SUMS.txt")
(cd "$OUT" && sha256sum Prometheus-HMI-Setup-*.exe >> SHA256SUMS.txt)
echo "done: $OUT"
