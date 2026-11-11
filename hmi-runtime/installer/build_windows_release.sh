#!/bin/sh
# Build the Windows release (industrial_hmi.exe + installer) from a Linux machine:
#   needs x86_64-w64-mingw32-gcc / -windres (Debian/Ubuntu: gcc-mingw-w64-x86-64) and makensis (nsis)
#   usage: hmi-runtime/installer/build_windows_release.sh [out-dir]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)   # the repository (this script is in hmi-runtime/installer)
OUT=${1:-$ROOT/dist}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)          # absolute: the builds below run in other folders
STAGE=$OUT/stage
CC=x86_64-w64-mingw32-gcc
RC=x86_64-w64-mingw32-windres
W="-std=c99 -Wall -Wextra -Wshadow -Werror"
rm -rf "$STAGE" && mkdir -p "$STAGE/examples"

cd "$ROOT/hmi-runtime"
conv() { sed -n "s/^set $1=//p" build.bat | tr -d '\r' | sed 's#\\#/#g'; }
LIB="$(conv CORE) $(conv RENDER) $(conv DEVICES) $(conv FIELDBUS) $(conv HAL)"
$RC res/industrial_hmi.rc -O coff -o "$STAGE/hmi.res"
$CC $W -Os -s -ffunction-sections -Wl,--gc-sections -fstack-protector-strong -static \
    -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -DHMI_BUILD_ID=\"release-win64\" -I include \
    src/app/app.c src/platform/win32/main_win32.c $LIB "$STAGE/hmi.res" $(conv GCC_LIBS) -mwindows \
    -o "$STAGE/industrial_hmi.exe"
cp res/industrial_hmi.ico "$STAGE/"
cp alarm.mp3 "$STAGE/"
cp examples/*.cfg "$STAGE/examples/"
cp examples/starter.cfg "$STAGE/hmi.cfg"

rm -f "$STAGE"/*.res
cp "$ROOT/hmi-runtime/installer/README-windows.txt" "$STAGE/README.txt"

cd "$OUT"
makensis -V2 -DSTAGE="$STAGE" "$ROOT/hmi-runtime/installer/prometheus_hmi.nsi"
mv "$ROOT/hmi-runtime/installer"/Prometheus-HMI-Setup-*.exe "$OUT/" 2>/dev/null || true
(cd "$STAGE" && sha256sum *.exe >> "$OUT/SHA256SUMS.txt")
(cd "$OUT" && sha256sum Prometheus-HMI-Setup-*.exe >> SHA256SUMS.txt)
echo "done: $OUT"
