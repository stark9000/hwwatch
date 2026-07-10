package hwwatcher;

import javax.swing.*;
import java.awt.*;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Small standalone test harness for the library: a device list grouped by class
 * on the left, a live arrival/removal log on the right. The device list
 * refreshes itself automatically (debounced) whenever a hardware change event
 * comes in, and newly-arrived devices are highlighted for a few seconds, fading
 * back to the normal background.
 *
 * Run with: java -cp out hwwatcher.HwWatchDemo (hwwatch32.dll / hwwatch64.dll
 * need to be next to the working directory the JVM was launched from, or
 * somewhere on java.library.path - see the loadNativeLibrary() comment in
 * HardwareWatcher.java for the exact search order)
 */
public class HwWatchDemo {

    private static final long HIGHLIGHT_FADE_MS = 3000L;
    private static final Color HIGHLIGHT_COLOR = new Color(190, 255, 190);

    public static void main(String[] args) {
        SwingUtilities.invokeLater(HwWatchDemo::buildUi);
    }

    /**
     * One row in the device list: either a class-name header, or an actual
     * device.
     */
    private static final class Row {

        final boolean header;
        final String headerText;
        final DeviceInfo device;
        final String displayText;

        static Row header(String text) {
            return new Row(true, text, null, null);
        }

        static Row device(DeviceInfo d, String text) {
            return new Row(false, null, d, text);
        }

        private Row(boolean header, String headerText, DeviceInfo device, String displayText) {
            this.header = header;
            this.headerText = headerText;
            this.device = device;
            this.displayText = displayText;
        }
    }

    /**
     * Highlight lookup keys work two different ways depending on where the
     * arrival event came from: - Real WM_DEVICECHANGE events (USB/HID/PORT/most
     * NET events) carry a VID/PID pulled from the raw interface path, which
     * matches the VID/PID SetupAPI reports for the same device - so those are
     * keyed by "VIDPID:xxxx_yyyy". - The network-adapter poll fallback (see
     * HardwareWatcher.netPollLoop) has no VID/PID to hand over, but it does
     * pass the device's exact SetupAPI instance id as the path - so those are
     * keyed by "PATH:<instance id>" instead. A device row is checked against
     * both possible keys when rendering, since which one applies depends on how
     * it arrived.
     */
    private static String vidPidKey(String vid, String pid) {
        return vid.isEmpty() ? null : "VIDPID:" + vid.toUpperCase(Locale.ROOT) + "_" + pid.toUpperCase(Locale.ROOT);
    }

    private static String pathKey(String path) {
        return "PATH:" + path;
    }

    private static Color blend(Color from, Color to, double ratio) {
        ratio = Math.max(0, Math.min(1, ratio));
        int r = (int) Math.round(from.getRed() * ratio + to.getRed() * (1 - ratio));
        int g = (int) Math.round(from.getGreen() * ratio + to.getGreen() * (1 - ratio));
        int b = (int) Math.round(from.getBlue() * ratio + to.getBlue() * (1 - ratio));
        return new Color(r, g, b);
    }

    /**
     * Finds whichever device row currently has the most recent (still fading)
     * arrival timestamp in highlightMap and scrolls it into view. Called after
     * every list rebuild - the list is rebuilt from scratch on refresh, so
     * nothing else keeps the scroll position anchored to a device that just
     * showed up.
     */
    private static void scrollToMostRecentArrival(JList<Row> deviceList, DefaultListModel<Row> deviceModel,
            Map<String, Long> highlightMap) {
        int bestIndex = -1;
        long bestTimestamp = -1L;

        for (int i = 0; i < deviceModel.size(); i++) {
            Row row = deviceModel.get(i);
            if (row.header || row.device == null) {
                continue;
            }

            String vpKey = vidPidKey(row.device.getVid(), row.device.getPid());
            Long arrivedAt = vpKey != null ? highlightMap.get(vpKey) : null;
            if (arrivedAt == null) {
                arrivedAt = highlightMap.get(pathKey(row.device.getDeviceId()));
            }

            if (arrivedAt != null && arrivedAt > bestTimestamp) {
                bestTimestamp = arrivedAt;
                bestIndex = i;
            }
        }

        if (bestIndex >= 0) {
            deviceList.ensureIndexIsVisible(bestIndex);
        }
    }

    private static void buildUi() {
        JFrame frame = new JFrame("hwwatch demo");
        frame.setDefaultCloseOperation(JFrame.EXIT_ON_CLOSE);
        frame.setSize(700, 500);

        DefaultListModel<String> logModel = new DefaultListModel<>();
        JList<String> logList = new JList<>(logModel);
        JScrollPane logScroll = new JScrollPane(logList);
        logScroll.setBorder(BorderFactory.createTitledBorder("Live events"));

        DefaultListModel<Row> deviceModel = new DefaultListModel<>();
        JList<Row> deviceList = new JList<>(deviceModel);
        // Highlight timestamps, keyed as described above. Read from the
        // renderer (EDT) and written from listener callbacks (native
        // watcher thread / poll thread) - ConcurrentHashMap so neither
        // side needs to coordinate with the other.
        Map<String, Long> highlightMap = new ConcurrentHashMap<>();

        deviceList.setCellRenderer(new DefaultListCellRenderer() {
            @Override
            public Component getListCellRendererComponent(JList<?> list, Object value, int index,
                    boolean isSelected, boolean cellHasFocus) {
                Row row = (Row) value;
                JLabel label = (JLabel) super.getListCellRendererComponent(
                        list, row.header ? row.headerText : row.displayText, index, isSelected, cellHasFocus);

                if (row.header) {
                    label.setFont(label.getFont().deriveFont(Font.BOLD));
                } else if (!isSelected && row.device != null) {
                    String vpKey = vidPidKey(row.device.getVid(), row.device.getPid());
                    Long arrivedAt = vpKey != null ? highlightMap.get(vpKey) : null;
                    if (arrivedAt == null) {
                        arrivedAt = highlightMap.get(pathKey(row.device.getDeviceId()));
                    }

                    if (arrivedAt != null) {
                        long elapsed = System.currentTimeMillis() - arrivedAt;
                        if (elapsed < HIGHLIGHT_FADE_MS) {
                            double ratio = 1.0 - (elapsed / (double) HIGHLIGHT_FADE_MS);
                            label.setOpaque(true);
                            label.setBackground(blend(HIGHLIGHT_COLOR, list.getBackground(), ratio));
                        }
                    }
                }
                return label;
            }
        });

        JScrollPane deviceScroll = new JScrollPane(deviceList);
        deviceScroll.setBorder(BorderFactory.createTitledBorder("Devices (grouped by class)"));

        JButton refreshBtn = new JButton("Refresh device list");
        JCheckBox showGhostsBox = new JCheckBox("Include ghost (not-present) devices", false);

        JSplitPane split = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, deviceScroll, logScroll);
        split.setResizeWeight(0.5);
        frame.add(split, BorderLayout.CENTER);

        JPanel southPanel = new JPanel(new FlowLayout(FlowLayout.LEFT));
        southPanel.add(refreshBtn);
        southPanel.add(showGhostsBox);
        frame.add(southPanel, BorderLayout.SOUTH);

        HardwareWatcher hw = HardwareWatcher.getInstance();

        Runnable populateDeviceList = () -> SwingUtilities.invokeLater(() -> {
            // Read Swing state on the EDT (this Runnable can be triggered
            // from a native callback thread), then hand the actual
            // SetupAPI enumeration off to a background thread since it
            // isn't cheap enough for the EDT.
            boolean includeGhosts = showGhostsBox.isSelected();
            new Thread(() -> {
                Map<String, List<DeviceInfo>> grouped = hw.groupByClass(includeGhosts);
                SwingUtilities.invokeLater(() -> {
                    deviceModel.clear();
                    for (Map.Entry<String, List<DeviceInfo>> e : grouped.entrySet()) {
                        deviceModel.addElement(Row.header("== " + e.getKey() + " =="));
                        for (DeviceInfo d : e.getValue()) {
                            String flag = d.isPresent() ? "" : " [GHOST]";
                            String vpid = d.getVid().isEmpty() ? "" : "  VID_" + d.getVid() + "&PID_" + d.getPid();
                            String port = d.getComPortName().isEmpty() ? "" : "  (" + d.getComPortName() + ")";
                            String text = "   " + d.getDisplayName() + port + vpid
                                    + (d.isVirtual() ? "  [virtual]" : "") + flag;
                            deviceModel.addElement(Row.device(d, text));
                        }
                    }
                    scrollToMostRecentArrival(deviceList, deviceModel, highlightMap);
                });
            }, "hwwatch-list").start();
        });

        refreshBtn.addActionListener(e -> populateDeviceList.run());
        showGhostsBox.addActionListener(e -> populateDeviceList.run());

        // Coalesces bursts of events (a single USB device often fires
        // several arrival notifications in quick succession for its
        // different interfaces/functions) into one refresh instead of
        // one per event.
        Timer refreshDebounce = new Timer(400, e -> populateDeviceList.run());
        refreshDebounce.setRepeats(false);

        // Repaints the device list on a short tick so the highlight
        // fade in the renderer above actually animates, and prunes
        // expired entries out of highlightMap so it can't grow forever.
        // Only runs while at least one highlight is still fading - idle
        // otherwise, so this costs nothing between hardware events.
        Timer fadeAnimator = new Timer(60, e -> {
            long now = System.currentTimeMillis();
            highlightMap.values().removeIf(t -> now - t >= HIGHLIGHT_FADE_MS);
            deviceList.repaint();
            if (highlightMap.isEmpty()) {
                ((Timer) e.getSource()).stop();
            }
        });

        // deviceClass here is one of "USB"/"HID"/"PORT"/"NET", or "OTHER:{guid}"
        // for an interface class this library doesn't have a friendly name
        // for yet - the raw GUID is included specifically so an unrecognized
        // device can be identified and a proper label added later.
        hw.addListener((changeType, deviceClass, vid, pid, path) -> {
            String line = changeType + "  class=" + deviceClass
                    + (vid.isEmpty() ? "" : "  VID_" + vid + "&PID_" + pid)
                    + "  " + path;

            if ("ARRIVAL".equals(changeType)) {
                long now = System.currentTimeMillis();
                String vpKey = vidPidKey(vid, pid);
                if (vpKey != null) {
                    highlightMap.put(vpKey, now);
                }
                highlightMap.put(pathKey(path), now); // covers the poll-fallback case too
            }

            SwingUtilities.invokeLater(() -> {
                logModel.addElement(line);
                logList.ensureIndexIsVisible(logModel.size() - 1);
                refreshDebounce.restart();
                if (!fadeAnimator.isRunning()) {
                    fadeAnimator.start();
                }
            });
        });

        // The network-adapter poll fallback (see HardwareWatcher.netPollLoop)
        // already goes through the same addListener path above via
        // fireHardwareChanged(), so it gets the same debounced refresh
        // and arrival highlight for free - nothing extra needed here.
        // onComPortsChanged is left wired up purely to demonstrate the
        // dedicated convenience API - the general listener above already
        // covers refreshing the list for every device class, COM ports
        // included, so this just adds a distinguishing log line.
        hw.onComPortsChanged(()
                -> SwingUtilities.invokeLater(() -> logModel.addElement(">>> COM port list changed <<<")));

        frame.addWindowListener(new java.awt.event.WindowAdapter() {
            @Override
            public void windowClosing(java.awt.event.WindowEvent e) {
                hw.stop();
            }
        });

        hw.start();
        populateDeviceList.run();

        frame.setVisible(true);
    }
}
