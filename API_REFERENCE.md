# hwwatch API Reference

Covers everything you'd actually call from application code
(`HardwareWatcher`, `DeviceInfo`, `HardwareChangeListener`), plus a short
section on the native side for whoever ends up maintaining `hwwatcher.c`
later.

---

## `HardwareWatcher`

Singleton entry point. Get it with `HardwareWatcher.getInstance()` - never
constructed directly.

### Lifecycle

| Function | Description |
|---|---|
| `static HardwareWatcher getInstance()` | Returns the single shared instance. Loads the native DLL the first time it's called. |
| `void start()` | Starts the native `WM_DEVICECHANGE` watcher thread and the network-adapter poll fallback. No-op if already running. |
| `void stop()` | Stops both the native watcher thread and the poll fallback, releases the hidden window. Call this on app shutdown. |
| `boolean isRunning()` | Whether `start()` has been called without a matching `stop()`. |

### Listening for events

| Function | Description |
|---|---|
| `void addListener(HardwareChangeListener l)` | Registers a callback for every arrival/removal event, from any device class. Fires on a native/poll thread, never the EDT. |
| `void removeListener(HardwareChangeListener l)` | Unregisters a previously-added listener. |
| `void onComPortsChanged(Runnable onChange)` | Convenience wrapper: only fires when the actual set of COM port names changes, so unrelated hardware events (a mouse, a WiFi adapter) don't trigger a spurious COM-port refresh. Register before calling `start()`. |

### Listing devices (one-off snapshot, not live)

All of these call into SetupAPI/CfgMgr32 synchronously - fine from a button
handler, but don't call them in a tight loop on the EDT.

| Function | Description |
|---|---|
| `List<DeviceInfo> listDevices(boolean includeGhosts)` | Every device Windows knows about. `includeGhosts=true` also returns devices that used to be plugged in but aren't present right now (what Device Manager calls hidden devices). |
| `List<DeviceInfo> listUsbDevices(boolean includeGhosts)` | Devices enumerated on the USB bus (`enumeratorName == "USB"`). |
| `List<DeviceInfo> listHidDevices(boolean includeGhosts)` | Devices whose Windows setup-class name contains "HID" - mice, keyboards, gamepads. |
| `List<DeviceInfo> listComPorts(boolean includeGhosts)` | Devices that have a COM port name assigned (`isComPort()` is true). |
| `List<DeviceInfo> filterByClassContains(boolean includeGhosts, String needle)` | Generic version of the three above - devices whose class name contains `needle`, case-insensitive. |
| `Map<String, List<DeviceInfo>> groupByClass(boolean includeGhosts)` | Every device, bucketed by Windows setup-class name (e.g. `"Ports (COM & LPT)"`, `"HIDClass"`, `"Net"`). |

### Internal (not part of the public API, listed for completeness)

| Function | Description |
|---|---|
| `private void fireHardwareChanged(...)` | Called from native code on every real event; fans it out to all registered listeners. Name/signature is load-bearing - `hwwatcher.c` looks it up by exact name via `GetMethodID`. |
| `private native void nativeStart()` / `nativeStop()` / `nativeListDevices(boolean)` | The three JNI entry points implemented in `hwwatcher.c`. |
| `private void startNetPolling()` / `stopNetPolling()` / `netPollLoop()` / `snapshotNetDeviceIds()` | The network-adapter polling fallback described below. |
| `private static void loadNativeLibrary()` / `jarDirectoryCandidate(String)` | Finds and loads the correct DLL - see the big comment block above it in `HardwareWatcher.java` for the exact search order. |

**Why the poll fallback exists:** `WM_DEVICECHANGE` only fires when a
driver explicitly registers a PnP device interface. Storage/HID/serial
drivers do this reliably; a lot of network adapter drivers (especially
USB WiFi dongles) don't. Rather than pull in a second native notification
API, `HardwareWatcher` diffs the "Net"-class device list every 3 seconds
while running and synthesizes `ARRIVAL`/`REMOVAL` events through the same
`fireHardwareChanged()` path real events use - a listener can't tell the
difference, it just sees a normal `"NET"` event either way.

---

## `DeviceInfo`

Immutable snapshot of one device, returned by all the `list*`/`groupByClass`
methods above. Built entirely from native code - the constructor's
parameter order is load-bearing (`hwwatcher.c` builds these objects
positionally) and shouldn't be reordered.

| Function | Description |
|---|---|
| `String getDeviceId()` | Full PnP instance id, e.g. `USB\VID_2341&PID_0043\5573...`. |
| `String getFriendlyName()` | Human-readable name, may be empty. |
| `String getDescription()` | Fallback description when there's no friendly name. |
| `String getManufacturer()` | Manufacturer string, may be empty. |
| `String getClassName()` | Windows setup-class name, e.g. `"Ports (COM & LPT)"`, `"HIDClass"`, `"Net"`. |
| `String getEnumeratorName()` | Bus type: `"USB"`, `"ROOT"`, `"BTHENUM"`, `"ACPI"`, `"PCI"`, etc. |
| `String getService()` | Driver service name, e.g. `"usbser"`, `"ch341ser"`, `"com0com"`. |
| `String getVid()` | Vendor id, 4-digit hex string (e.g. `"2341"`) - empty if not a USB device. |
| `String getPid()` | Product id, 4-digit hex string - empty if not a USB device. |
| `String getSerialNumber()` | USB serial or instance-id suffix, may be empty. |
| `String getComPortName()` | `"COM3"` etc, empty if this device isn't a serial port. |
| `boolean isPresent()` | `false` = ghost entry - was seen before, isn't plugged in right now. |
| `boolean isVirtual()` | Best-effort guess at real hardware vs. a software-emulated port (e.g. com0com). |
| `boolean isUsb()` | Shorthand for `"USB".equalsIgnoreCase(getEnumeratorName())`. |
| `boolean isComPort()` | Shorthand for `!getComPortName().isEmpty()`. |
| `String getDisplayName()` | Best available label: friendly name, else description, else raw device id. |
| `String toString()` | One-line human-readable dump of every field, useful for logging. |

---

## `HardwareChangeListener`

Functional interface - implement as a lambda and pass to `addListener()`.

| Function | Description |
|---|---|
| `void onHardwareChanged(String changeType, String deviceClass, String vid, String pid, String devicePath)` | Called on arrival/removal. `changeType` is `"ARRIVAL"` or `"REMOVAL"`. `deviceClass` is `"USB"`, `"HID"`, `"PORT"`, `"NET"`, or `"OTHER:{guid}"` for an interface class not yet given a friendly name. `vid`/`pid` are 4-digit hex strings, empty if not parseable (always empty for polled NET events). `devicePath` is the raw device interface path Windows reported (or, for polled NET events, the device's exact SetupAPI instance id). |

---

## Native side (`hwwatcher.c`) - for whoever maintains the DLL

Only three functions are exported and callable from Java; everything else
is a static helper.

| Function | Description |
|---|---|
| `Java_hwwatcher_HardwareWatcher_nativeStart` | Spins up the hidden message-only window and its own thread, registers for `WM_DEVICECHANGE` across every interface class (`DEVICE_NOTIFY_ALL_INTERFACE_CLASSES`), and waits briefly for the message loop to be alive before returning. |
| `Java_hwwatcher_HardwareWatcher_nativeStop` | Posts a stop message to the watcher window, waits for its thread to exit, releases the global ref on the Java-side watcher. |
| `Java_hwwatcher_HardwareWatcher_nativeListDevices` | Runs a full SetupAPI/CfgMgr32 enumeration and builds a `DeviceInfo[]`. Presence (`isPresent()`) is determined by cross-checking against a separate `DIGCF_PRESENT`-filtered enumeration, not `CM_Get_DevNode_Status`/`DN_STARTED` - that flag turned out to disagree with `DIGCF_PRESENT` for at least one real NIC driver. |

Internal helpers worth knowing about if you're debugging a device that
isn't classifying correctly:

| Function | Description |
|---|---|
| `classifyGuid(...)` | Maps a device interface GUID to `"USB"`/`"HID"`/`"PORT"`/`"NET"`, or formats an unknown one as `"OTHER:{guid}"` so it's still identifiable instead of disappearing into a generic bucket. |
| `parseUsbIds(...)` | Pulls VID/PID/serial out of a raw instance id or device path. Case-insensitive on `VID_`/`PID_` since real IDs are usually but not always uppercase. |
| `isKnownVirtualService(...)` | Denylist of driver service names known to be software-only virtual serial port emulators (`com0com`, `vspd`, etc) - backs `DeviceInfo.isVirtual()`. |
| `collectPresentInstanceIds(...)` / `isInStringSet(...)` | The ground-truth presence check described above. |
| `ensureDeviceInfoClassCached(...)` | Lazily looks up and caches the `DeviceInfo` class/constructor, shared by both `nativeStart` and `nativeListDevices` so `listDevices()` works even if called before `start()`. Guarded by a `CRITICAL_SECTION` against concurrent first-time init. |

**If you ever rename a Java method these depend on, or change `DeviceInfo`'s
constructor signature**, the DLL needs recompiling to match - see
`build_mingw32.bat` / `build_mingw64.bat`.
