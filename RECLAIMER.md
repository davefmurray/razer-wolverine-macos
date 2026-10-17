# Project Reclaimer on macOS

The Wolverine adapter and the monitor share this repository and the same GIP authentication code. The adapter runs inside Wine through SDL2. It does not install a system-wide macOS HID driver or require a paid Apple Developer membership.

## Installed setup

The tested installation uses Wine 11.18 Staging on Apple Silicon and Project Reclaimer 0.9.18. Its Intel SDL2 2.32.8 library was replaced with the patched SDL2 2.32.10 library. libusb 1.0.29 is linked statically, and the library uses an ad-hoc local signature.

Locations below assume the default Project Reclaimer installation:

```bash
RECLAIMER_ROOT="$HOME/Games/ProjectReclaimer"
WINE_ROOT="$RECLAIMER_ROOT/wine/Wine Staging.app/Contents/Resources/wine"
export WINEPREFIX="$RECLAIMER_ROOT/prefix-wine"
```

## Build and install

Run these commands from this repository. Close Project Reclaimer first and press Stop in the monitor.

```bash
brew install cmake pkgconf mingw-w64
./scripts/build-sdl2-libusb.sh
./scripts/install-sdl-into-wine.sh "$WINE_ROOT"
./scripts/wine-registry.sh "$WINEPREFIX" "$WINE_ROOT/bin/wine"
```

The installer preserves the original SDL library as `lib/libSDL2-2.0.0.dylib.orig`. The registry helper enables SDL and controller mapping and disables the alternative HID bus to avoid duplicate or phantom controllers.

Add these two exports to Project Reclaimer's `game/run.sh` before it runs Wine. They were added to the local installation during setup.

```bash
export SDL_JOYSTICK_MFI=0
export SDL_GAMECONTROLLER_IGNORE_DEVICES="0x045e/0x028e,0x045e/0x02e0,0x1532/0x27db"
```

Launch the game through its normal Project Reclaimer app. The monitor must remain stopped while the game runs. The browser window can stay open.

## Verify Windows input

Reclaimer's bundle has `bin/wine`, not the `bin/wine64` assumed by the upstream Heroic test helper. Use the explicit executable:

```bash
x86_64-w64-mingw32-gcc -O2 -o build/xinput_test.exe src/xinput_test.c -lxinput
export SDL_JOYSTICK_MFI=0
export SDL_GAMECONTROLLER_IGNORE_DEVICES="0x045e/0x028e,0x045e/0x02e0,0x1532/0x27db"
WINEDEBUG=-all MVK_CONFIG_LOG_LEVEL=1 "$WINE_ROOT/bin/wine" build/xinput_test.exe 60
```

Move the sticks, squeeze both triggers, and press buttons. The output should show `slot 0: CONNECTED` followed by changing values. The test executable's exit code alone does not prove input works; inspect its output. Its coverage summary combines all connected slots despite the upstream slot-0 label.

Before installing the adapter, this Wine prefix exposed no connected XInput controller. After installation it reported the Wolverine in slot 0 and completed its GIP handshake. Project Reclaimer was then launched through its normal app, and its own log reported `Controller 1 connected.` Physical stick and button response in the game still needs a user test.

## Recover and restore

If the handshake stalls after switching between the monitor and Wine, unplug and reconnect the controller with the intended reader running. Stop monitor capture before restarting the game. If Project Reclaimer replaces its Wine bundle during an update, rebuild or reinstall the adapter and reapply the launch environment if needed.

To restore the original Wine library after closing the game:

```bash
./scripts/uninstall-sdl-from-wine.sh "$WINE_ROOT"
```

On the original tested prefix, the three custom WineBus values did not exist before setup. To restore that baseline:

```bash
"$WINE_ROOT/bin/wine" reg delete 'HKLM\System\CurrentControlSet\Services\WineBus' /v DisableHidraw /f
"$WINE_ROOT/bin/wine" reg delete 'HKLM\System\CurrentControlSet\Services\WineBus' /v 'Enable SDL' /f
"$WINE_ROOT/bin/wine" reg delete 'HKLM\System\CurrentControlSet\Services\WineBus' /v 'Map Controllers' /f
```

Remove the two SDL exports from `game/run.sh`. Restore different pre-existing registry values if this guide is used on another installation. Local original hashes, registry export, and launch-script backup are kept outside the repository in Project Reclaimer's `controller-research` folder.
