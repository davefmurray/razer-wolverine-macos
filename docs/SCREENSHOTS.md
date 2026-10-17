# README screenshots

Both images were captured from the working setup on October 16, 2026, on an Apple Silicon M3 Mac running macOS 26.3.

| Image | Source and visible evidence |
|---|---|
| [wireless-monitor.png](images/wireless-monitor.png) | Live page at `http://127.0.0.1:8769`, using the physical 2.4 GHz receiver. A is pressed, the left stick is displaced, and RT is 1023. The page shows a completed handshake and 2,807 full input reports. |
| [macos-usb-receiver.png](images/macos-usb-receiver.png) | Native System Information window with Hardware → USB selected. It lists `Razer Wolverine V3 Pro for Xbox 2.4`, vendor `0x1532`, and product `0x0a3f`. |

The monitor screenshot uses real controller input. No fixture data, simulated controls, or image generation was used. The USB screenshot establishes hardware detection, not macOS GameController support or in-game compatibility.

## Capture the monitor

1. Close the game and any other controller reader.
2. Start the monitor using [DASHBOARD.md](../DASHBOARD.md).
3. Press **Start reading** and wait for input reports.
4. Hold A and a trigger while slowly moving a stick.
5. Capture the browser content while the status reads **Live**. On macOS, press Shift-Command-4 and select the page area.
6. Save the PNG as `docs/images/wireless-monitor.png` and update its caption if the visible controls differ.

Continued movement matters. The receiver can pause reports when controls stop changing, and the monitor dims highlights after 400 ms. Keep that behavior intact when taking a screenshot.

## Capture macOS detection

1. Open **System Information** from Spotlight.
2. Select **Hardware → USB** and the Razer receiver.
3. Press Shift-Command-4, then Space, and click the System Information window.
4. Save the PNG as `docs/images/macos-usb-receiver.png`.

Inspect the selected device and surrounding content before committing the image. Capture the USB pane rather than Hardware Overview, which contains the Mac's serial number and hardware UUID.
