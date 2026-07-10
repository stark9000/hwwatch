# hwwatch

A lightweight, dependency-free Java 8 library for detecting Windows hardware
changes in real time — USB, HID (mice/keyboards/gamepads), serial (COM)
ports, and network adapters — backed by a small native DLL.

No external Java libraries. No polling required for most device classes.
Just JNI, `WM_DEVICECHANGE`, and SetupAPI/CfgMgr32 doing the real work.

## Features

- **Real-time detection** for USB, HID, and COM port arrivals/removals via
  `WM_DEVICECHANGE`, with a single broad registration
  (`DEVICE_NOTIFY_ALL_INTERFACE_CLASSES`) instead of one listener per class.
- **Network adapter support** through an automatic polling fallback — a lot
  of NIC/WiFi driver stacks don't reliably fire PnP interface notifications,
  so `hwwatch` diffs the device list every few seconds for that one class
  and delivers the event through the exact same listener API.
- **Full device enumeration** (`listDevices`, `groupByClass`,
  `listUsbDevices`, `listHidDevices`, `listComPorts`) via SetupAPI, including
  "ghost" devices that were plugged in before but aren't present right now —
  the same thing Device Manager's "show hidden devices" reveals.
- **VID/PID, serial number, driver service name, bus type, and a real-vs-
  virtual classification** (e.g. distinguishing a real USB-serial adapter
  from a `com0com` virtual port) for every device.
- **Convenience helpers** like `onComPortsChanged(Runnable)`, which only
  fires when the actual set of COM ports changes — so a mouse or keyboard
  event doesn't trigger a spurious refresh.
- Ships as a small `hwwatch32.dll` / `hwwatch64.dll` pair plus three Java
  classes — drop it into any Swing/AWT project, or call it from a background
  thread in anything else.

## Requirements

- Java 8 or later (JNI, no other runtime dependency)
- Windows (uses SetupAPI, CfgMgr32, and User32 directly)
- MinGW-w64 if you need to rebuild the native side (`build_mingw32.bat` /
  `build_mingw64.bat` are included)

## Quick start

```java
HardwareWatcher hw = HardwareWatcher.getInstance();

hw.addListener((changeType, deviceClass, vid, pid, path) -> {
    // fires on a native/poll thread - hop to the EDT before touching Swing
    SwingUtilities.invokeLater(() ->
        System.out.println(changeType + " " + deviceClass + " " + vid + ":" + pid));
});

hw.onComPortsChanged(() -> SwingUtilities.invokeLater(this::refreshComPortDropdown));

hw.start();
// ...
hw.stop(); // on shutdown
```

Listing what's connected right now, grouped by Windows device class:

```java
Map<String, List<DeviceInfo>> byClass = hw.groupByClass(/* includeGhosts */ false);
```

Drop `hwwatch32.dll` / `hwwatch64.dll` next to your working directory (or
add them to `java.library.path`) — see the loader's search order documented
in `HardwareWatcher.java` for the full fallback chain, including support for
DLLs bundled straight inside a jar.

## How it works

A hidden message-only window runs on its own native thread and registers
for `WM_DEVICECHANGE` across every device interface class in one call.
Arrivals/removals are parsed for VID/PID and classified (`USB`, `HID`,
`PORT`, `NET`, or `OTHER:{guid}` for anything not yet given a friendly
label) and handed back to Java through a single callback method. Device
listing is a separate, on-demand SetupAPI/CfgMgr32 walk, with presence
determined by cross-checking against a `DIGCF_PRESENT`-filtered
enumeration rather than trusting `CM_Get_DevNode_Status` alone, since that
flag has been observed to disagree with `DIGCF_PRESENT` for at least one
real network adapter driver.

Full function-by-function reference: see `API_REFERENCE.md`.

## Known limitations

- Live, event-driven detection for network adapters isn't guaranteed —
  it depends entirely on whether the installed driver registers a PnP
  device interface, which many NIC drivers skip. The polling fallback
  covers this, at a coarser (multi-second) latency than the native path.
- The real-vs-virtual classification is a best-effort heuristic (bus
  enumerator + a small denylist of known virtual-serial driver service
  names), not a guarantee.

## License

MIT (or replace with whatever you prefer before publishing).
