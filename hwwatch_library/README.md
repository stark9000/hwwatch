# Using hwwatch in a new NetBeans project

## What you need
- `hwwatch.jar` - the compiled library (HardwareWatcher, DeviceInfo, HardwareChangeListener)
- `hwwatch32.dll` / `hwwatch64.dll` - the native side, unchanged from your working HWwatcher project

## The one rule
The DLL's exported JNI functions are named `Java_hwwatcher_HardwareWatcher_...` - baked in at
compile time. As long as `hwwatch.jar` is on the classpath (unmodified, still package
`hwwatcher`), this just works. **Do not** decompile/recompile these three classes into a
different package - that would break the link between Java and the DLL, and the DLL would need
recompiling with matching symbol names to match.

Your own project's code can be in any package you like - only the library's own three classes
need to stay as they are.

## Steps in NetBeans
1. Right-click your project → **Properties → Libraries → Add JAR/Folder** → select `hwwatch.jar`.
2. Copy `hwwatch32.dll` and `hwwatch64.dll` into your new project's root folder (same folder as
   its `build.xml`) - that's the working directory NetBeans runs from by default, and
   `HardwareWatcher`'s loader checks there first.
3. Use it like any other library:

```java
import hwwatcher.HardwareWatcher;
import hwwatcher.DeviceInfo;

HardwareWatcher hw = HardwareWatcher.getInstance();
hw.addListener((changeType, deviceClass, vid, pid, path) -> {
    // handle it, hop to the EDT with SwingUtilities.invokeLater if touching Swing
});
hw.start();
```

## If you ever need to change HardwareWatcher.java/DeviceInfo.java itself
Edit the source under `src-for-reference/hwwatcher/` in this folder (or your original
HWwatcher project), recompile into a fresh `hwwatch.jar`, and only rebuild the DLL if you
changed a native method's name or signature, or DeviceInfo's constructor signature - those are
the only things the C side depends on directly.
