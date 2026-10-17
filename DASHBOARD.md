# Live input dashboard

Direct USB diagnostic for the wired Razer Wolverine V3 Pro (`1532:0a3f`). The page shows moving stick plots, raw and percentage measurements, trigger levels, button values, and a controller drawing from `build/giptool`. It is not a system-wide HID driver. It does not change Wine, signing, or controller firmware.

The server listens only on `127.0.0.1:8769`. Capture stays stopped until you press Start reading.

## Run

```bash
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

Stick percentages use 32768 below zero and 32767 at zero and above. Triggers use 1023.

## Checks

```bash
python3 tools/test_dashboard.py
```

Frozen panels for the same renderer are at http://127.0.0.1:8769/fixtures.html while the server is running. Those panels do not poll and they are not the controller.

## Local verification

On October 16, 2026, the wired controller delivered 2,840 full input reports on macOS 26.3. Both sticks reached −32768 and 32767 on both axes. Both triggers reached 1023. The browser rendered actual trigger activity, and DFM confirmed that the UI works.

The automated suite covers capture lifecycle, handshake failure and recovery, input validity, and local HTTP controls. Separate browser fixtures verify all 15 digital button mappings, stick transforms, trigger fill, and no-data or stale states. Fixture data is never sent to the live capture.

Firmware stays blank in a session where the controller does not send a Hello packet. A previous capture reported firmware 1.1.1026.0.

## License

GPL-2.0-or-later, same as the rest of this repository.
