package hwwatcher;

import java.io.*;
import java.nio.file.*;
import java.security.CodeSource;
import java.util.*;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Java-side entry point for hwwatcher. Backed by a native DLL
 * (hwwatch32.dll on a 32-bit JVM, hwwatch64.dll on a 64-bit JVM) that
 * does the actual WM_DEVICECHANGE listening and SetupAPI/CfgMgr32
 * enumeration - see hwwatcher.c for that side of things.
 *
 * Java 8, no external dependencies. This class is a singleton, since
 * the native side only supports one active watcher per process.
 *
 * Typical usage:
 *
 *   HardwareWatcher hw = HardwareWatcher.getInstance();
 *   hw.addListener((changeType, deviceClass, vid, pid, path) -> {
 *       SwingUtilities.invokeLater(() -> myLabel.setText(changeType + " " + deviceClass));
 *   });
 *   hw.onComPortsChanged(() -> SwingUtilities.invokeLater(this::refreshComPortList));
 *   hw.start();
 *   ...
 *   hw.stop(); // call this on shutdown so the native thread exits cleanly
 */
public final class HardwareWatcher {

    private static final HardwareWatcher INSTANCE = new HardwareWatcher();

    /** How often the network-adapter poll fallback re-checks the device
     *  list. See the big comment above startNetPolling() for why this
     *  exists at all - WM_DEVICECHANGE isn't reliable for NICs. */
    private static final long NET_POLL_INTERVAL_MS = 3000;

    private final List<HardwareChangeListener> listeners = new CopyOnWriteArrayList<>();
    private volatile boolean running = false;
    private volatile Thread netPollThread;

    private HardwareWatcher() {
        loadNativeLibrary();
    }

    public static HardwareWatcher getInstance() {
        return INSTANCE;
    }

    // ------------------------------------------------------------------
    // Public API
    // ------------------------------------------------------------------

    /** Adds a listener. Note that callbacks land on the native watcher
     *  thread, never the EDT - hop over with SwingUtilities.invokeLater
     *  before touching any Swing component. */
    public void addListener(HardwareChangeListener l) {
        listeners.add(l);
    }

    public void removeListener(HardwareChangeListener l) {
        listeners.remove(l);
    }

    /**
     * Convenience wrapper around addListener(): only invokes onChange
     * when the actual set of COM port names differs from the previous
     * snapshot, so plugging in a mouse or keyboard won't trigger a
     * COM-port-list refresh in your UI just because *something*
     * changed. Register this before calling start().
     */
    public void onComPortsChanged(Runnable onChange) {
        final String[] lastSnapshot = { snapshotComPorts() };
        addListener((changeType, deviceClass, vid, pid, path) -> {
            String now = snapshotComPorts();
            if (!now.equals(lastSnapshot[0])) {
                lastSnapshot[0] = now;
                onChange.run();
            }
        });
    }

    private String snapshotComPorts() {
        StringBuilder sb = new StringBuilder();
        for (DeviceInfo d : listDevices(true)) {
            if (d.isComPort()) {
                sb.append(d.getComPortName()).append(';');
            }
        }
        return sb.toString();
    }

    /** Starts the native watcher thread. Does nothing if it's already running. */
    public synchronized void start() {
        if (running) return;
        nativeStart();
        running = true;
        startNetPolling();
    }

    /** Stops the native watcher thread and tears down the hidden window it uses. */
    public synchronized void stop() {
        if (!running) return;
        stopNetPolling();
        nativeStop();
        running = false;
    }

    public boolean isRunning() {
        return running;
    }

    /**
     * Enumerates devices right now via SetupAPI/CfgMgr32. This is a
     * synchronous, somewhat heavier call - fine to invoke from a
     * button handler, but don't call it in a tight loop on the EDT.
     *
     * @param includeGhosts if true, also returns devices that were
     *                      plugged in before but aren't present right
     *                      now (what Device Manager calls "hidden devices").
     */
    public List<DeviceInfo> listDevices(boolean includeGhosts) {
        DeviceInfo[] arr = nativeListDevices(includeGhosts);
        return arr == null ? Collections.emptyList() : Arrays.asList(arr);
    }

    /** Devices enumerated on the USB bus. */
    public List<DeviceInfo> listUsbDevices(boolean includeGhosts) {
        List<DeviceInfo> out = new ArrayList<>();
        for (DeviceInfo d : listDevices(includeGhosts)) {
            if (d.isUsb()) out.add(d);
        }
        return out;
    }

    /** Devices whose Windows setup-class name mentions HID - covers mice, keyboards, gamepads. */
    public List<DeviceInfo> listHidDevices(boolean includeGhosts) {
        return filterByClassContains(includeGhosts, "HID");
    }

    /** Devices that have a COM port name assigned to them. */
    public List<DeviceInfo> listComPorts(boolean includeGhosts) {
        List<DeviceInfo> out = new ArrayList<>();
        for (DeviceInfo d : listDevices(includeGhosts)) {
            if (d.isComPort()) out.add(d);
        }
        return out;
    }

    public List<DeviceInfo> filterByClassContains(boolean includeGhosts, String needle) {
        String n = needle.toLowerCase(Locale.ROOT);
        List<DeviceInfo> out = new ArrayList<>();
        for (DeviceInfo d : listDevices(includeGhosts)) {
            if (d.getClassName().toLowerCase(Locale.ROOT).contains(n)) out.add(d);
        }
        return out;
    }

    /** Buckets the full device list by Windows setup-class name, e.g. "Ports (COM & LPT)", "HIDClass". */
    public Map<String, List<DeviceInfo>> groupByClass(boolean includeGhosts) {
        Map<String, List<DeviceInfo>> map = new LinkedHashMap<>();
        for (DeviceInfo d : listDevices(includeGhosts)) {
            map.computeIfAbsent(d.getClassName(), k -> new ArrayList<>()).add(d);
        }
        return map;
    }

    // ------------------------------------------------------------------
    // Network adapter polling fallback
    // ------------------------------------------------------------------
    //
    // WM_DEVICECHANGE device-interface notifications only fire when a
    // driver explicitly registers a PnP device interface, and in
    // practice storage/HID/serial drivers do this reliably while a lot
    // of network adapter (especially USB WiFi dongle) driver stacks
    // don't - either registering too late to matter or not triggering
    // the broadcast at all. This isn't something fixable from this side
    // of the fence; Windows itself provides a separate API
    // (NotifyIpInterfaceChange, in iphlpapi.dll) specifically because
    // WM_DEVICECHANGE was never fully dependable for NICs.
    //
    // Rather than pull in that second native notification mechanism,
    // this just diffs the "Net" class device list on a timer and
    // synthesizes ARRIVAL/REMOVAL events through the exact same
    // fireHardwareChanged() path real WM_DEVICECHANGE events use - so
    // from a listener's point of view a polled network adapter change
    // looks identical to a natively-detected one (deviceClass "NET").
    //
    // Runs only while start()/stop() says the watcher is active, and
    // only touches network-class devices - every other device class
    // still goes through the fast, event-driven native path untouched.

    private void startNetPolling() {
        Thread t = new Thread(this::netPollLoop, "hwwatch-net-poll");
        t.setDaemon(true);
        netPollThread = t;
        t.start();
    }

    private void stopNetPolling() {
        Thread t = netPollThread;
        netPollThread = null;
        if (t != null) t.interrupt();
    }

    private void netPollLoop() {
        Set<String> known = snapshotNetDeviceIds();
        while (netPollThread == Thread.currentThread()) {
            try {
                Thread.sleep(NET_POLL_INTERVAL_MS);
            } catch (InterruptedException e) {
                return; // stop() was called
            }

            Set<String> now = snapshotNetDeviceIds();

            for (String id : now) {
                if (!known.contains(id)) {
                    fireHardwareChanged("ARRIVAL", "NET", "", "", id);
                }
            }
            for (String id : known) {
                if (!now.contains(id)) {
                    fireHardwareChanged("REMOVAL", "NET", "", "", id);
                }
            }
            known = now;
        }
    }

    private Set<String> snapshotNetDeviceIds() {
        Set<String> ids = new HashSet<>();
        for (DeviceInfo d : filterByClassContains(false, "Net")) {
            if (d.isPresent()) ids.add(d.getDeviceId());
        }
        return ids;
    }

    // ------------------------------------------------------------------
    // Invoked from native code (hwwatcher.c) - name/signature is fixed,
    // do not rename or change the parameter list without updating the
    // GetMethodID call in Java_hwwatcher_HardwareWatcher_nativeStart.
    // ------------------------------------------------------------------
    private void fireHardwareChanged(String changeType, String deviceClass, String vid, String pid, String devicePath) {
        for (HardwareChangeListener l : listeners) {
            try {
                l.onHardwareChanged(changeType, deviceClass, vid, pid, devicePath);
            } catch (Throwable t) {
                // A listener blowing up must not take down the native callback thread.
                t.printStackTrace();
            }
        }
    }

    // ------------------------------------------------------------------
    // Native methods, implemented in hwwatcher.c
    // ------------------------------------------------------------------
    private native void nativeStart();
    private native void nativeStop();
    private native DeviceInfo[] nativeListDevices(boolean includeGhosts);

    // ------------------------------------------------------------------
    // Native library loading
    // ------------------------------------------------------------------
    //
    // Three places are checked, in order, and the first one that finds
    // the DLL wins:
    //
    //   1. Next to the current working directory. When you hit Run in
    //      NetBeans, the working directory is the project root (the
    //      same folder as build.xml) - so dropping hwwatch32.dll /
    //      hwwatch64.dll directly in the project folder is enough for
    //      this to find it during development, no java.library.path
    //      setup needed.
    //   2. Next to wherever this class's own .class/.jar file is
    //      running from. This is what makes it work after you've built
    //      a jar and are running it from somewhere else entirely (e.g.
    //      double-clicking dist\HWwatcher.jar from Explorer) - the
    //      working directory won't be the project root anymore, but the
    //      DLL sitting next to the jar still gets found.
    //   3. As a packaged classpath resource, e.g. if the DLL was bundled
    //      straight inside the jar. Extracted to a temp file since
    //      System.load() needs a real path on disk, not a jar entry.
    //
    // If none of those work, it falls through to a plain
    // System.loadLibrary() call, which is the normal java.library.path
    // search - useful if you've set that up some other way (e.g. -D on
    // the java command line).
    //
    // IMPORTANT DISTINCTION: System.load(path) takes a *full path* to
    // a specific file. System.loadLibrary(name) takes a *bare name*
    // with no path and no ".dll" extension, and searches
    // java.library.path itself. Mixing the two up (e.g. calling
    // loadLibrary with a full path) fails on Windows - that was a bug
    // in an earlier version of this file.
    private static void loadNativeLibrary() {
        String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
        boolean is64 = arch.contains("64");
        String libName = is64 ? "hwwatch64.dll" : "hwwatch32.dll";

        File fromWorkingDir = new File(libName);
        if (fromWorkingDir.exists()) {
            System.load(fromWorkingDir.getAbsolutePath());
            return;
        }

        File fromJarDir = jarDirectoryCandidate(libName);
        if (fromJarDir != null && fromJarDir.exists()) {
            System.load(fromJarDir.getAbsolutePath());
            return;
        }

        try (InputStream in = HardwareWatcher.class.getResourceAsStream("/" + libName)) {
            if (in != null) {
                Path tmp = Files.createTempFile("hwwatch-", ".dll");
                tmp.toFile().deleteOnExit();
                Files.copy(in, tmp, StandardCopyOption.REPLACE_EXISTING);
                System.load(tmp.toAbsolutePath().toString());
                return;
            }
        } catch (IOException e) {
            throw new UnsatisfiedLinkError("Failed extracting " + libName + " from classpath: " + e);
        }

        System.loadLibrary(is64 ? "hwwatch64" : "hwwatch32");
    }

    /** Returns "<folder containing the running jar/classes>/<libName>",
     *  or null if that location can't be determined (e.g. running
     *  under a security manager that blocks it). */
    private static File jarDirectoryCandidate(String libName) {
        try {
            CodeSource src = HardwareWatcher.class.getProtectionDomain().getCodeSource();
            if (src == null || src.getLocation() == null) return null;
            File location = new File(src.getLocation().toURI());
            File dir = location.isDirectory() ? location : location.getParentFile();
            return dir == null ? null : new File(dir, libName);
        } catch (Exception e) {
            return null;
        }
    }
}
