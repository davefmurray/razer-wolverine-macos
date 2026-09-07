#!/bin/bash
# Run build/xinput_test.exe under the GPTK Wine in the Rocket League prefix with the same env Heroic uses.
# Usage: scripts/run-xinput-test.sh [seconds] [WINEPREFIX] [wine-root]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-60}"
export WINEPREFIX="${2:-$HOME/Games/Heroic/Prefixes/Rocket League}"
WINE_ROOT="${3:-$HOME/Library/Application Support/heroic/tools/game-porting-toolkit/Game-Porting-Toolkit-latest/Contents/Resources/wine}"
export SDL_JOYSTICK_MFI=0
export SDL_GAMECONTROLLER_IGNORE_DEVICES="0x045e/0x028e,0x045e/0x02e0,0x1532/0x27db"
export SDL_HIDAPI_LIBUSB_WHITELIST=0
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEESYNC=1 WINEMSYNC=1   # must match Heroic's wineserver (enableEsync/enableMsync)
exec "$WINE_ROOT/bin/wine64" "$ROOT/build/xinput_test.exe" "$SECS"
