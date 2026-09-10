# Razer Wolverine V3 Pro on macOS (Apple Silicon) → Wine / Rocket League

Makes the wired **Razer Wolverine V3 Pro (Xbox mode, USB `1532:0a3f`)** work as a real XInput gamepad
inside Wine on macOS — analog sticks, analog triggers, all buttons — without a kernel extension,
DriverKit driver, SIP change, or Wine rebuild. Tested with Heroic Games Launcher + Apple Game Porting
Toolkit Wine and Rocket League (offline / Free Play).

## Why this exists

macOS has no driver for Microsoft GIP (the Xbox One wired protocol, USB class `FF/47/D0`), and
Razer Synapse for Mac does not support this controller. Worse, **this controller sends no input at all
until the host completes the GIP security handshake** (RSA/TLS-style, command `0x06`). Stock SDL,
Linux `xpad` and every "send power-on and go" implementation get silence from it.

## How it works

```
Wolverine V3 Pro (USB, GIP)
   ↓ libusb, statically linked (x86_64)
custom SDL 2.32.10  ← patches/: libusb HIDAPI backend + GIP security handshake in the Xbox One driver
   = drop-in lib/libSDL2-2.0.0.dylib inside the Wine bundle
   ↓ winebus.sys (SDL bus) → winexinput.sys → xinput1_3.dll
game
```

The handshake is a userspace port of [medusalix/xone](https://github.com/medusalix/xone)'s `auth/auth.c`
(GPL-2.0-or-later) using Apple CommonCrypto + Security.framework, so the SDL patch has no third-party
crypto dependency.

## Contents

| Path | What |
|---|---|
| `src/giptool.c`, `src/gipauth.c` | Native (arm64) diagnostic: talks GIP over libusb, does the handshake, prints live decoded state. `./build/giptool`, `--raw`, `--json`, `--query` |
| `patches/sdl2-000*.patch` | SDL 2.32.10 patches: static libusb on macOS, GIP auth + chunked transport, libusb whitelist for Xbox-class devices |
| `scripts/build-sdl2-libusb.sh` | Builds x86_64 libusb + patched SDL2 → `build/x86_64/lib/libSDL2-2.0.0.dylib` |
| `scripts/install-sdl-into-wine.sh` / `uninstall-…` | Drop the dylib into a Wine bundle (backs up the original) / restore |
| `scripts/wine-registry.sh` | `DisableHidraw=1` etc. in a prefix so phantom HID pads don't take XInput slots |
| `src/sdltest.c`, `src/xinput_test.c`, `scripts/run-xinput-test.sh` | Verification at the SDL layer and the Windows XInput layer |
| `tools/usbinfo.py`, `tools/hidprobe.py` | USB descriptor / HID report probes |
| `NOTES.md` | Full investigation log: protocol details, packet layouts, quirks, sources |

## Quick start (Heroic + GPTK)

```bash
brew install libusb pkgconf cmake mingw-w64
make && ./build/giptool                 # sanity check: move the sticks, Ctrl-C
./scripts/build-sdl2-libusb.sh          # ~2 min
./scripts/install-sdl-into-wine.sh      # into Heroic's Game-Porting-Toolkit-latest bundle
./scripts/wine-registry.sh "$HOME/Games/Heroic/Prefixes/<Game>"
./scripts/run-xinput-test.sh 60         # should show slot 0 CONNECTED and live values
```

In Heroic → game → Settings → Advanced → Environment variables:
`SDL_JOYSTICK_MFI=0`, `SDL_GAMECONTROLLER_IGNORE_DEVICES=0x045e/0x028e,0x045e/0x02e0,0x1532/0x27db`.
Re-run the install script whenever Heroic updates the Wine bundle.

## Quirks

- The controller reboots (~2 s, LEDs off) whenever the last host closes the USB device. Hotplug is
  handled: plugging in after the game started, or unplugging/replugging mid-game, reconnects within a
  few seconds (SDL rescans libusb every 3 s, negotiates immediately, and retries the handshake).
  Holding the Xbox button powers it off.
- Razer Synapse for Mac publishes phantom "Xbox 360 Controller" HID devices even with no controller
  attached; the registry/env settings above hide them.
- Back paddles M1–M6 are not exposed in Xbox mode (see NOTES.md for Razer's vendor protocol).

## License

GPL-2.0-or-later (required by the xone-derived handshake code). SDL2 is zlib-licensed, libusb LGPL-2.1.

## Credits

Microsoft's public [MS-GIPUSB](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/) spec,
[medusalix/xone](https://github.com/medusalix/xone), Linux `xpad`, SDL's `SDL_hidapi_xboxone.c`,
[Netruk44/wolverine-v3-pro-linux](https://github.com/Netruk44/wolverine-v3-pro-linux).
