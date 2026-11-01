#!/bin/sh
# Install Prometheus HMI for this user (no root): programs in ~/.local/lib/prometheus-hmi,
# launchers in ~/.local/bin, menu entries and icon in ~/.local/share.
#   ./install.sh            install / upgrade
#   ./install.sh --remove   uninstall (keeps ~/.local/share/prometheus-hmi: log, key, vault, settings)
set -e
SRC="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
LIB="$PREFIX/lib/prometheus-hmi"; BIN="$PREFIX/bin"; APPS="$PREFIX/share/applications"
ICON="$PREFIX/share/icons/hicolor/256x256/apps"
if [ "$1" = "--remove" ]; then
    rm -rf "$LIB" "$BIN/prometheus-hmi" "$BIN/prometheus-hmi-bench" \
           "$APPS/prometheus-hmi.desktop" "$APPS/prometheus-hmi-bench.desktop" "$ICON/prometheus-hmi.png"
    echo "Removed. Your settings, log, key and vault are still in ${XDG_DATA_HOME:-$HOME/.local/share}/prometheus-hmi"
    exit 0
fi
mkdir -p "$LIB" "$BIN" "$APPS" "$ICON"
cp "$SRC/industrial_hmi" "$SRC/plc_simulator_cli" "$SRC/prometheus-hmi" "$SRC/prometheus-hmi-bench" "$SRC/hmi.cfg" "$LIB/"
chmod 755 "$LIB/industrial_hmi" "$LIB/plc_simulator_cli" "$LIB/prometheus-hmi" "$LIB/prometheus-hmi-bench"
ln -sf "$LIB/prometheus-hmi" "$BIN/prometheus-hmi"
ln -sf "$LIB/prometheus-hmi-bench" "$BIN/prometheus-hmi-bench"
cp "$SRC/prometheus-hmi.png" "$ICON/prometheus-hmi.png"
# Exec= takes a quoted path (desktop entry spec): put a backslash before " ` $ and backslash,
# then double every backslash for the key file, and write % as %%. Spaces and # are fine.
QLIB=$(printf '%s' "$LIB" | sed -e 's/[\\"`$]/\\&/g' -e 's/\\/\\\\/g' -e 's/%/%%/g')
for f in prometheus-hmi.desktop prometheus-hmi-bench.desktop; do
    while IFS= read -r line || [ -n "$line" ]; do
        case $line in
            Exec=@LIB@/*) printf 'Exec="%s/%s"\n' "$QLIB" "${line#Exec=@LIB@/}" ;;
            *)            printf '%s\n' "$line" ;;
        esac
    done < "$SRC/$f" > "$APPS/$f"
done
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS" 2>/dev/null || true
echo "Installed. Open \"Prometheus HMI (bench)\" from your applications menu, or run: prometheus-hmi-bench"
