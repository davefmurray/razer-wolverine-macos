# Live input dashboard

Direct USB diagnostic for the Razer Wolverine V3 Pro (`1532:0a3f`) through its cable or included 2.4 GHz receiver. The page shows moving stick plots, raw and percentage measurements, trigger levels, button values, and a controller drawing from `build/giptool`. It is not a system-wide HID driver. It does not change Wine, signing, or controller firmware.

The monitor and the Wine adapter need exclusive USB access. Press Stop in the monitor before using the controller in a game. Close the game before starting monitor capture.

The server listens only on `127.0.0.1:8769`. Capture stays stopped until you press Start reading.

See the [live wireless screenshot](docs/images/wireless-monitor.png) and [macOS USB screenshot](README.md#find-the-receiver-in-macos). The USB listing proves device detection; the monitor proves that input reaches the reader.

## Run

You need Apple's Command Line Tools, Python 3, and Homebrew packages `libusb` and `pkgconf`. Run the commands from the repository directory.

```bash
brew install libusb pkgconf
make
python3 tools/dashboard.py
```

Open http://127.0.0.1:8769

Or double-click `launch.command`. It runs `make` when the dashboard is not already up, starts one server, and opens the page. If that URL is already answering, it only opens the browser. Closing the Terminal window sends interrupt to the server. The server stops the reader it started.

Ctrl-C in the server terminal does the same. A reader that ignores interrupt is stopped with terminate, then kill, and only that child is signaled.

## What the screen means

Before the first full input report, stick plots have no dot and the numbers are em dashes. A finished security handshake stays on "Complete, waiting for input" until a real `kind=input` report arrives. Guide packets can light Guide. They do not move the sticks, fill the triggers, or increment the full-input count.

When reports pause but the reader is still running, the last numbers stay and are labeled last received. Button highlights turn off until a fresh report. That silence is not shown as a disconnect.

A replug, a stop, a reader error, or a lost browser poll clears live highlights. Replug also clears the current readings until the next full input report.

Stick values range from -32768 to 32767. Positive X points right, and positive Y points up. Stick percentages use 32768 below zero and 32767 at zero and above. Trigger values range from 0 to 1023. The displayed minimum and maximum values describe the current capture.

Digital buttons read 0 when released and 1 when pressed. LT and RT in the button grid show normalized trigger pressure between 0 and 1. M1–M6 do not appear as independent inputs in normal Xbox mode.

Firmware stays blank if the controller does not send a Hello packet during that capture. Blank firmware does not prevent live input.

## Recover capture

If the monitor reports **Controller handshake failed** or remains at **Handshake Starting**:

1. Close the game and any SDL or XInput diagnostic using the controller.
2. Press **Stop**, then **Start reading**.
3. If the handshake still stalls, unplug and reconnect the USB cable or receiver with capture running. Keep the wireless controller powered on.
4. Move a stick and check for increasing input reports. A solid controller light alone does not prove input is reaching the reader.

An idle wireless controller can show **Last received** until a control changes. The monitor removes active button highlights after 400 ms without fresh reports while retaining the last measurements.

If recovery keeps failing, save the **USB activity log** and stop capture before trying another reader. A Wine controller process can survive after a game exits; inspect processes associated with that game's prefix. Do not terminate unrelated Wine applications.

Intermittent native authentication failures remain unresolved. A leftover Wine process was found during one investigation, but failures also occurred after it was removed. Restarting capture has recovered input; that does not establish the failure's cause.

## Checks

```bash
python3 tools/test_dashboard.py
```

Frozen panels for the same renderer are at http://127.0.0.1:8769/fixtures.html while the server is running. Those panels do not poll and they are not the controller.

## Local verification

On October 16, 2026, the wired controller delivered 2,840 full input reports on macOS 26.3. Both sticks reached −32768 and 32767 on both axes. Both triggers reached 1023. The browser rendered actual trigger activity, and DFM confirmed that the UI works.

A wireless receiver test that day recorded 5,087 input reports. Both sticks reached the full signed range on both axes, and both triggers reached 0..1023. The live browser showed moving sticks and trigger pressure.

A later screenshot session again encountered a handshake failure before a fresh capture succeeded. The [README screenshot](docs/images/wireless-monitor.png) records real wireless input with A pressed, the left stick displaced, and the right trigger at 1023. It shows 2,807 input reports from that separate capture.

The automated suite covers capture lifecycle, handshake failure and recovery, input validity, and local HTTP controls. Separate browser fixtures verify all 15 digital button mappings, stick transforms, trigger fill, and no-data or stale states. Fixture data is never sent to the live capture.

A previous capture reported firmware 1.1.1026.0. The screenshot session did not report firmware.

## License

GPL-2.0-or-later, same as the rest of this repository.
