#!/bin/bash
# Restore the Wine bundle's original libSDL2.
set -euo pipefail
WINE_ROOT="${1:-$HOME/Library/Application Support/heroic/tools/game-porting-toolkit/Game-Porting-Toolkit-latest/Contents/Resources/wine}"
DST="$WINE_ROOT/lib/libSDL2-2.0.0.dylib"
[ -f "$DST.orig" ] || { echo "no backup at $DST.orig"; exit 1; }
mv -f "$DST.orig" "$DST" && echo "restored original $DST"
