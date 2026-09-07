# Investigation notes (see README.md for the how-to)

# Razer Wolverine V3 Pro on macOS (Apple Silicon) → Wine/Rocket League — working notes

Last updated: 2026-09-07

## Controller identification (detected)
- USB: **VID 0x1532 (Razer), PID 0x0A3F** = "Razer Wolverine V3 Pro for Xbox", Xbox mode. Firmware 1.1.770.0 (from GIP Hello).
- Device class/subclass/protocol **0xFF/0x47/0xD0 = Microsoft GIP** (Xbox One "Gaming Input Protocol"). Not HID. macOS attaches no driver.
- Configuration 1, two interfaces (both FF/47/D0):
  - if#0: interrupt **EP 0x81 IN / EP 0x01 OUT**, 64 bytes, interval 4 ms — the GIP data pipe.
  - if#1: alt0 no endpoints / alt1 isochronous 0x03/0x83 — headset audio (unused; we select alt0 like Linux xpad).
- Other modes (not used): PC mode (Fn+Menu+A) changes PID to 0x0A4C (Xbox 360 protocol per PCGamingWiki/Linux xpad). Holding the Xbox button powers the controller off; it re-enumerates on the next plug.
- Reproduce: `python3 tools/usbinfo.py` (descriptors + claim test), `ioreg -p IOUSB -w0 | grep -i razer`.

## Razer Synapse for Mac (installed) — red herring
- Its DriverKit extension `com.razer.appengine.virtual.driver` publishes a virtual "Xbox 360 Controller" (0x045E:0x028E) and "Razer DriverKit VirtualJoystic" (0x1532:0x27DB) **whether or not a controller is attached**; Synapse for Mac supports no controllers. They never emit data (`tools/hidprobe.py`). Treat as phantom devices to filter out in Wine (`DisableHidraw=1` + `SDL_GAMECONTROLLER_IGNORE_DEVICES`).

## Protocol findings (measured with `build/giptool`)
- libusb (Homebrew, arm64) can open the device and claim interface 0 as a normal user. No sudo, no kext.
- Device sends **Hello (0x02)** every 500 ms until the host answers. Metadata request `04 20 seq 00` → 260-byte fragmented response (class `Windows.Xbox.Input.Gamepad`; declares messages 0x20 in (32 bytes), 0x09 out, 0x10 in / 0x0F out (Razer vendor frames)). Fragments are ACKed with `01 20 <seq> 09 00 04 20 <received lo hi> 00 00 <remaining lo hi>`.
- **The controller sends NO input until the GIP security handshake (command 0x06) completes.** Verified: with the handshake → thousands of 0x20 reports + Guide (0x07) packets; without it (Start/LED/rumble/xpad-style "auth done" only) → nothing, even while operating the controller. Fake `06 20 seq 02 01 00` alone is not accepted.
- Handshake (ported from medusalix/xone `auth/auth.c`, TLS-like, v1 RSA path used by this controller): host hello (32-byte random) → ack → request client hello → client hello (random) → request certificate (X.509 from "Xbox Accessories Class Prod CA 001", 825 bytes, chunked) → extract RSA-2048 public key → send PKCS#1-encrypted 48-byte premaster (274 bytes, chunked) → master = PRF("Master Secret") → host finish PRF("Host Finished", transcript) → ack → request client finish → verify PRF("Device Finished") → control "complete" (`06 20 seq 02 01 00`). Transcript = SHA-256 over every handshake message minus the 6-byte handshake header (received messages hashed before our reply, client-finish excluded).
- Working order: Hello → Metadata → Security → Set Device State: Start (`05 20 s 01 00`) → LED (`0A 20 s 03 00 01 14`) → rumble init.
- Input report 0x20: 36 bytes (4 hdr + 32 payload). Payload: buttons u16 LE (bit1 = keep-alive, ignore; 0x04 Menu 0x08 View 0x10 A 0x20 B 0x40 X 0x80 Y; hi byte 0x01 DU 0x02 DD 0x04 DL 0x08 DR 0x10 LB 0x20 RB 0x40 LS 0x80 RS), LT/RT u16 LE 0..1023, LX/LY/RX/RY s16 LE (−32768..32767, Y up = positive). Bytes 14..31 are zero in normal mode (paddles M1–M6 only appear in Razer "diagnostic mode", see Netruk44/wolverine-v3-pro-linux docs/protocol.md). Guide button: `07 20 seq 02 <down> 5b`, no ACK needed.
- Razer vendor channel: request `0F ..` / reply `10 ..` 64-byte frames with XOR checksum at byte 0x3E (query class 0/sub 0x84 = diagnostic-mode state works: `giptool --query`).

## Architecture that works

```
Wolverine V3 Pro (USB, GIP, if#0 EP 0x81/0x01)
   ↓ libusb 1.0.29 (statically linked, x86_64)
custom SDL 2.32.10 (patches/sdl2-000*.patch: libusb HIDAPI backend + GIP security handshake in SDL_hidapi_xboxone.c)
   = drop-in lib/libSDL2-2.0.0.dylib inside the GPTK Wine bundle
   ↓ Wine winebus.sys (SDL bus, is_gamepad=1) → winexinput.sys (XI_00 / IG_00) → xinput1_3.dll
Rocket League
```
No Wine rebuild, no kernel/DriverKit driver, no SIP change. Fully reversible: `scripts/uninstall-sdl-from-wine.sh`.

## What works (verified 2026-09-07)
- `build/giptool` (arm64, libusb + CommonCrypto/Security.framework): handshake + live decoded state. All 14 buttons, Guide, triggers 0..1023, sticks ±32767.
- `build/sdltest` (x86_64, custom SDL): SDL sees "Razer Wolverine V3 Pro for Xbox" as an Xbox One GameController, 6 axes / 15 buttons, full ranges, triggers analog.
- `build/xinput_test.exe` under GPTK Wine in the Rocket League prefix: XInput slot 0 connected (type 1 subtype 1); LX/LY/RX/RY reach ±32767, up/right positive; LT/RT analog 0..255 with intermediate values; all 14 XInput buttons (0xF3FF).

## Status: COMPLETE — confirmed working in Rocket League (2026-09-07)
- User confirmation: after signing into Epic once (online launch) the splash advanced and the Wolverine "works perfectly" in-game.
- End-to-end verified: giptool (native) → patched SDL (x86_64) → winebus SDL bus (is_gamepad=1) → winexinput (XI_00) → xinput1_3 in the Rocket League prefix. XInput slot 0 delivered full analog sticks (±32767), analog triggers (0..255), and all 14 buttons while the game was running.
- In-game, Rocket League sits on its "Press any button to start" splash — NOT a controller problem: a keyboard cannot pass it either. The game log shows `FEOSManager: OnlineSubsystem is null` / `not logged into EOS`: the splash waits on Epic Online Services sign-in (and the first-run EULA). Fix: launch Heroic online / signed into Epic once so EOS initialises; after that the splash advances and the controller (XInput slot 0) drives the game.
- Phantom-device cleanup verified: with `DisableHidraw=1` on `HKLM\System\CurrentControlSet\Services\WineBus` plus `SDL_JOYSTICK_MFI=0` and `SDL_GAMECONTROLLER_IGNORE_DEVICES`, XInput shows exactly one slot (the Wolverine). Note `scripts/wine-registry.sh` must run with `WINEESYNC=0 WINEMSYNC=0` when Heroic's wineserver is not running, or with matching values when it is.

## Known quirks
- When the host closes the USB device (game/test exits) macOS unconfigures it and the controller reboots (~2 s, LEDs off, re-enumerates). macOS may pop its "Keyboard Setup Assistant" during that transient. Harmless; plug stays in.
- Holding the Xbox button powers the controller off. Tap it only.
- SDL only rescans libusb devices every 3 s and only picks the controller up if it is present and stable at scan time; if it was mid-reboot when the game started, wait a few seconds and restart the game.
- Stick rest position is not exactly 0 (e.g. LX ≈ +900 of 32767) — normal for this pad; XInput/Rocket League deadzones absorb it.
- Paddles M1–M6 are not exposed in Xbox mode (they mirror face buttons unless Razer's diagnostic mode is enabled; see `giptool --query`).

## Reproduce the setup
```bash
brew install libusb pkgconf cmake mingw-w64        # dev deps
make                                                # build/giptool (native diagnostic)
./build/giptool                                     # live state; Ctrl-C to quit
./scripts/build-sdl2-libusb.sh                      # x86_64 libusb + patched SDL2 → build/x86_64/lib/libSDL2-2.0.0.dylib
make build/sdltest && SDL_JOYSTICK_MFI=0 ./build/sdltest 60   # SDL-level check (Rosetta)
./scripts/install-sdl-into-wine.sh                  # drop into Heroic's GPTK bundle (backs up .orig)
./scripts/wine-registry.sh "$HOME/Games/Heroic/Prefixes/Rocket League"   # DisableHidraw=1 in the prefix
./scripts/run-xinput-test.sh 60                     # XInput check under Wine (build/xinput_test.exe)
```
Re-run `install-sdl-into-wine.sh` after Heroic updates/reinstalls the GPTK bundle (it overwrites lib/).

## Heroic configuration (Rocket League, app "Sugar") — already applied to GamesConfig/Sugar.json
- Wine: Game-Porting-Toolkit-latest (GPTK 1.1 / wine 7.7). Prefix: ~/Games/Heroic/Prefixes/Rocket League.
- Settings → Advanced → Environment variables:
  - `SDL_JOYSTICK_MFI=0` (hide Razer Synapse's phantom "Xbox 360 Controller" that GCController re-exports)
  - `SDL_GAMECONTROLLER_IGNORE_DEVICES=0x045e/0x028e,0x045e/0x02e0,0x1532/0x27db` (phantoms)
  - `SDL_HIDAPI_LIBUSB_WHITELIST=0` (belt and braces; the patch already whitelists Xbox-class devices)
- Launch arguments: `-noeac` (offline/training only; EAC/online out of scope).
- Registry in the prefix: `HKLM\System\CurrentControlSet\Services\WineBus` DisableHidraw=1, Enable SDL=1, Map Controllers=1.
- To use a different Wine (e.g. Wine-Staging 11.16 for DXMT): install it via Heroic's Wine Manager, then `scripts/install-sdl-into-wine.sh <that bundle>/Contents/Resources/wine` and `scripts/wine-registry.sh <prefix> <that wine64>` — the drop-in only requires the bundle to load `lib/libSDL2-2.0.0.dylib` (check with `otool`/`strings` on its winebus.so).

## Sources borrowed from
- Microsoft MS-GIPUSB open spec (2024-09-16) — packet formats, startup order, ACK rules.
- medusalix/xone (bus/protocol.c, auth/auth.c, auth/crypto.c) — chunking + security handshake.
- Linux xpad.c, SDL `SDL_hidapi_xboxone.c` — init packets, report layout.
- Netruk44/wolverine-v3-pro-linux — Razer vendor frame format, diagnostic mode, proof that 0a3f works with a full GIP host (SteamOS xbox_gip).
- Razer Insider "Synapse 4 for Mac creates phantom Xbox 360 controller" — phantom device explanation.
