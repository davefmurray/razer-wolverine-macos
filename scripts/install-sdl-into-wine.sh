#!/bin/bash
# Install our custom libSDL2 (libusb + GIP auth) into a Wine bundle used by Heroic.
# Usage: scripts/install-sdl-into-wine.sh [<wine-root>]   (wine-root = dir containing bin/ and lib/)
# Default: the GPTK bundle Heroic ships as "Game-Porting-Toolkit-latest".
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINE_ROOT="${1:-$HOME/Library/Application Support/heroic/tools/game-porting-toolkit/Game-Porting-Toolkit-latest/Contents/Resources/wine}"
NEW="$ROOT/build/x86_64/lib/libSDL2-2.0.0.dylib"
DST="$WINE_ROOT/lib/libSDL2-2.0.0.dylib"
[ -f "$NEW" ] || { echo "custom SDL not built yet: run scripts/build-sdl2-libusb.sh"; exit 1; }
[ -f "$DST" ] || { echo "no libSDL2-2.0.0.dylib in $WINE_ROOT/lib — is this a Wine bundle?"; exit 1; }
if [ ! -f "$DST.orig" ]; then cp -p "$DST" "$DST.orig"; echo "backed up original to $DST.orig"; fi
cp "$NEW" "$DST"
codesign -s - -f "$DST" >/dev/null 2>&1 || true
echo "installed $(file -b "$DST" | cut -d, -f1-2) into $DST"
echo -n "GIP auth engine present: "; [ "$(strings "$DST" | grep -c "GIP security handshake")" -gt 0 ] && echo yes || echo NO
