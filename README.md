# Bluetooth Battery Overlay

A small Win32 overlay that stays on top of the desktop and shows battery percentage, estimated remaining time, and a historical health estimate for Bluetooth LE devices that expose the standard Battery Service.

## What it does

- Polls Bluetooth LE devices on a background thread.
- Reads battery percentage through the GATT Battery Service (`0x180F`) and Battery Level characteristic (`0x2A19`).
- Tracks discharge history per device and estimates remaining time.
- Estimates health from observed drain behavior when the device does not expose a direct health metric.
- Renders through a layered, click-through Win32 window so the UI thread stays cheap.

## Requirements

- **CMake** 3.15 or later
- **Visual Studio 18** (MSVC) or equivalent C++ compiler
- Windows 10 or later

## Build

Use CMake with a Windows toolchain:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Notes

- The overlay is intentionally minimal and optimized for idle screen use.
- Easy close hotkeys: Ctrl+Shift+Q and Ctrl+Alt+Q.
- Runtime toggle: Ctrl+Alt+T enables/disables click-through without restarting.
- Tray icon menu (system tray): right-click for Reload now and Exit.
- The current implementation targets Bluetooth LE devices that expose the standard battery GATT service. That is the most reliable native path on Windows without adding heavier runtime dependencies.

## Debug mode

- Run with --debug to disable click-through and enable a normal window frame with a close button.
- Run with --console to open a debug console and print refresh logs.
- Per-device diagnostics are printed in debug console logs (discovery stage and battery read reason).
- In the overlay, diagnostics are shown in each row when battery is unavailable or when running with --debug.
- Battery lookup order is: LE GATT battery service -> Windows Bluetooth device property fallback.
- If standard keys are empty, the app performs a numeric property-key scan fallback and reports the winning key in diagnostics.
- Address-based sibling fallback now uses confidence scoring; weak key-scan values are logged as candidates but are not auto-applied.

Examples:

```powershell
build/windows-x64/Release/BluetoothBatteryOverlay.exe --debug
build/windows-x64/Release/BluetoothBatteryOverlay.exe --debug --console
```
