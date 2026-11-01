#!/bin/sh
# Build the Linux release archives: PC (x86-64), Raspberry Pi 64-bit and 32-bit.
#   needs gcc, aarch64-linux-gnu-gcc, arm-linux-gnueabihf-gcc and the X11 headers (libx11-dev)
#   usage: installer/build_linux_release.sh [out-dir]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/dist}
mkdir -p "$OUT"
# The X11 headers are architecture-independent: give the cross compilers a copy of just those
# (libX11 itself is loaded at run time, so no target library is needed to build)
X11INC="$OUT/x11-headers"; rm -rf "$X11INC"; mkdir -p "$X11INC"; cp -r /usr/include/X11 "$X11INC/"
for t in x86_64-linux-gnu:cc aarch64-linux-gnu:aarch64-linux-gnu-gcc arm-linux-gnueabihf:arm-linux-gnueabihf-gcc; do
    name=${t%%:*}; cc=${t#*:}
    case $name in x86_64*) label=linux-x86_64 ;; aarch64*) label=raspberrypi-arm64 ;; *) label=raspberrypi-armhf ;; esac
    D="$OUT/prometheus-hmi-$label"; rm -rf "$D"; mkdir -p "$D"
    (cd "$ROOT/hmi-runtime" && make -s clean && make -s industrial_hmi CC=$cc OPT="-Os -ffunction-sections -fdata-sections -idirafter $X11INC" >/dev/null && cp industrial_hmi "$D/" && make -s clean)
    (cd "$ROOT/plc-simulator" && make -s clean && make -s CC=$cc >/dev/null && cp plc_simulator_cli "$D/" && make -s clean)
    cp "$ROOT"/installer/linux/* "$D/"
    cp "$ROOT/hmi-runtime/examples/bench.cfg" "$D/hmi.cfg"
    python3 -c "from PIL import Image; Image.open('$ROOT/hmi-runtime/res/industrial_hmi.ico').convert('RGBA').resize((256,256)).save('$D/prometheus-hmi.png')" 2>/dev/null \
        || echo "(no PIL: add a 256x256 prometheus-hmi.png to $D)"
    printf 'Prometheus HMI for Linux (%s)\n\n  ./install.sh        then open "Prometheus HMI (bench)" from the applications menu\n  ./prometheus-hmi-bench   run without installing\n\nDesktop: opens as a window (X11, or XWayland on Wayland desktops).\nNo desktop (a panel or a Pi console): uses the framebuffer. Server: add --headless.\nBench only: hmi.cfg allows the factory PINs (Operator 1111).\n' "$label" > "$D/README.txt"
    (cd "$OUT" && tar -czf "prometheus-hmi-$label.tar.gz" "prometheus-hmi-$label")
done
rm -rf "$X11INC"
(cd "$OUT" && sha256sum prometheus-hmi-*.tar.gz >> SHA256SUMS.txt)
echo "done: $OUT"
