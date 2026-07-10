package hwwatcher;

/**
 * Callback for hardware arrival/removal events. Implement this and
 * pass it to HardwareWatcher.addListener(...).
 *
 * Fired from a native watcher thread, not the EDT - if you're updating
 * a Swing component, wrap the body in
 * SwingUtilities.invokeLater(...) before touching anything on screen.
 */
public interface HardwareChangeListener {

    /**
     * @param changeType  either "ARRIVAL" or "REMOVAL"
     * @param deviceClass rough classification of what changed - one of
     *                    "USB", "HID", "PORT", "NET", or "OTHER"
     * @param vid         vendor id as a 4-digit hex string, e.g. "2341" -
     *                    empty if the device path didn't contain one
     * @param pid         product id as a 4-digit hex string, e.g. "0043" -
     *                    empty if the device path didn't contain one
     * @param devicePath  the raw device interface path reported by Windows
     */
    void onHardwareChanged(String changeType, String deviceClass, String vid, String pid, String devicePath);
}
