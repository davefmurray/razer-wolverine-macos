#!/bin/bash
# Configure winebus in a Wine prefix: disable the IOHID bus (hides Razer Synapse's phantom
# "Xbox 360 Controller"), keep the SDL bus. Usage: scripts/wine-registry.sh <WINEPREFIX> [<wine64 binary>]
set -euo pipefail
PREFIX="${1:?usage: wine-registry.sh <WINEPREFIX> [wine64]}"
WINE="${2:-$HOME/Library/Application Support/heroic/tools/game-porting-toolkit/Game-Porting-Toolkit-latest/Contents/Resources/wine/bin/wine64}"
export WINEPREFIX="$PREFIX" WINEDEBUG=-all WINEESYNC=${WINEESYNC:-0} WINEMSYNC=${WINEMSYNC:-0}
"$WINE" reg add 'HKLM\System\CurrentControlSet\Services\WineBus' /v DisableHidraw /t REG_DWORD /d 1 /f
"$WINE" reg add 'HKLM\System\CurrentControlSet\Services\WineBus' /v 'Enable SDL' /t REG_DWORD /d 1 /f
"$WINE" reg add 'HKLM\System\CurrentControlSet\Services\WineBus' /v 'Map Controllers' /t REG_DWORD /d 1 /f
"$WINE" reg query 'HKLM\System\CurrentControlSet\Services\WineBus'
