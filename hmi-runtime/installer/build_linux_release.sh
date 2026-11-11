#!/bin/sh
# Build the Linux release archives: PC (x86-64), Raspberry Pi 64-bit and 32-bit.
#   needs gcc, aarch64-linux-gnu-gcc, arm-linux-gnueabihf-gcc and the X11 headers (libx11-dev)
#   usage: hmi-runtime/installer/build_linux_release.sh [out-dir]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)   # the repository (this script is in hmi-runtime/installer)
OUT=${1:-$ROOT/dist}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)          # absolute: the builds below run in other folders
# Built in a scratch copy of the sources: the developer's tree (and its test builds) is not touched
WORK=$(mktemp -d "${TMPDIR:-/tmp}/prometheus-release.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
# The X11 headers are architecture-independent: give the cross compilers a copy of just those
# (libX11 itself is loaded at run time, so no target library is needed to build)
X11INC="$WORK/x11-headers"; mkdir -p "$X11INC"; cp -r /usr/include/X11 "$X11INC/"
for t in x86_64-linux-gnu:cc aarch64-linux-gnu:aarch64-linux-gnu-gcc arm-linux-gnueabihf:arm-linux-gnueabihf-gcc; do
    name=${t%%:*}; cc=${t#*:}
    case $name in x86_64*) label=linux-x86_64 ;; aarch64*) label=raspberrypi-arm64 ;; *) label=raspberrypi-armhf ;; esac
    D="$OUT/prometheus-hmi-$label"; rm -rf "$D"; mkdir -p "$D"
    B="$WORK/$label"; mkdir -p "$B"
    cp -r "$ROOT/hmi-runtime" "$B/"
    (cd "$B/hmi-runtime" && make -s clean && make -s industrial_hmi CC=$cc OPT="-Os -ffunction-sections -fdata-sections -idirafter $X11INC" >/dev/null && cp industrial_hmi "$D/")
    cp "$ROOT"/hmi-runtime/installer/linux/* "$D/"
    cp "$ROOT/hmi-runtime/examples/starter.cfg" "$D/hmi.cfg"
    mkdir -p "$D/examples" && cp "$ROOT"/hmi-runtime/examples/*.cfg "$D/examples/"
    python3 -c "from PIL import Image; Image.open('$ROOT/hmi-runtime/res/industrial_hmi.ico').convert('RGBA').resize((256,256)).save('$D/prometheus-hmi.png')" 2>/dev/null \
        || echo "(no PIL: add a 256x256 prometheus-hmi.png to $D)"
    printf 'Prometheus HMI for Linux (%s)\n\n  ./install.sh        then open "Prometheus HMI" from the applications menu\n  ./prometheus-hmi    run without installing\n\nFirst set the role PINs: ./industrial_hmi --set-pin operator (and maintenance, engineer),\nthen list your devices in hmi.cfg. Test bench: --config examples/bench.cfg (factory PINs).\nDesktop: opens as a window (X11, or XWayland on Wayland desktops).\nNo desktop (a panel or a Pi console): uses the framebuffer. Server: add --headless.\n' "$label" > "$D/README.txt"
    (cd "$OUT" && tar -czf "prometheus-hmi-$label.tar.gz" "prometheus-hmi-$label")
done
(cd "$OUT" && sha256sum prometheus-hmi-*.tar.gz >> SHA256SUMS.txt)
echo "done: $OUT"
