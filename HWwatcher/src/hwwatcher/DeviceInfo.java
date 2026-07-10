package hwwatcher;

/**
 * One device snapshot, as returned by HardwareWatcher.listDevices(...).
 * Immutable - built entirely from native code via the constructor
 * below. The parameter order/types here have to line up exactly with
 * the NewObject(...) call in hwwatcher.c, so don't reorder or retype
 * fields without updating both sides.
 */
public final class DeviceInfo {

    private final String deviceId;       // full PnP instance id, e.g. USB\VID_2341&PID_0043\5573...
    private final String friendlyName;   // human-friendly device name, may be empty
    private final String description;    // fallback description when there's no friendly name
    private final String manufacturer;   // manufacturer string, may be empty
    private final String className;      // Windows setup-class name, e.g. "Ports (COM & LPT)", "HIDClass"
    private final String enumeratorName; // bus type: "USB", "ROOT", "BTHENUM", "ACPI", "PCI"...
    private final String service;        // driver service name, e.g. "usbser", "ch341ser", "com0com"
    private final String vid;            // vendor id hex string, e.g. "2341" - empty if not a USB device
    private final String pid;            // product id hex string, e.g. "0043" - empty if not a USB device
    private final String serialNumber;   // USB serial or instance-id suffix, may be empty
    private final String comPortName;    // "COM3" etc - empty if this device isn't a serial port
    private final boolean present;       // false = ghost entry: was seen before, isn't plugged in right now
    private final boolean virtual;       // best-effort guess at real hardware vs a software-emulated port

    public DeviceInfo(String deviceId, String friendlyName, String description, String manufacturer,
                       String className, String enumeratorName, String service,
                       String vid, String pid, String serialNumber, String comPortName,
                       boolean present, boolean virtual) {
        this.deviceId = nz(deviceId);
        this.friendlyName = nz(friendlyName);
        this.description = nz(description);
        this.manufacturer = nz(manufacturer);
        this.className = nz(className);
        this.enumeratorName = nz(enumeratorName);
        this.service = nz(service);
        this.vid = nz(vid);
        this.pid = nz(pid);
        this.serialNumber = nz(serialNumber);
        this.comPortName = nz(comPortName);
        this.present = present;
        this.virtual = virtual;
    }

    // Native side hands back "" rather than null for missing properties
    // anyway, but this is here as a belt-and-braces guard.
    private static String nz(String s) {
        return s == null ? "" : s;
    }

    public String getDeviceId() { return deviceId; }
    public String getFriendlyName() { return friendlyName; }
    public String getDescription() { return description; }
    public String getManufacturer() { return manufacturer; }
    public String getClassName() { return className; }
    public String getEnumeratorName() { return enumeratorName; }
    public String getService() { return service; }
    public String getVid() { return vid; }
    public String getPid() { return pid; }
    public String getSerialNumber() { return serialNumber; }
    public String getComPortName() { return comPortName; }
    public boolean isPresent() { return present; }
    public boolean isVirtual() { return virtual; }
    public boolean isUsb() { return "USB".equalsIgnoreCase(enumeratorName); }
    public boolean isComPort() { return !comPortName.isEmpty(); }

    /** Picks the best available label for display: friendly name first, then description, then raw device id. */
    public String getDisplayName() {
        if (!friendlyName.isEmpty()) return friendlyName;
        if (!description.isEmpty()) return description;
        return deviceId;
    }

    @Override
    public String toString() {
        return "DeviceInfo{" +
                (comPortName.isEmpty() ? "" : comPortName + ", ") +
                getDisplayName() +
                (vid.isEmpty() ? "" : ", VID_" + vid + "&PID_" + pid) +
                ", class=" + className +
                ", enum=" + enumeratorName +
                ", service=" + service +
                ", present=" + present +
                ", virtual=" + virtual +
                '}';
    }
}
