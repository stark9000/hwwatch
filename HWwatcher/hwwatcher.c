/*
 * hwwatcher native module
 * ------------------------------------------------------------
 * This is the native half of the hwwatcher Java library. It does two
 * unrelated jobs that happen to share the same SetupAPI/CfgMgr32 data:
 *
 *   1. Runs a hidden window on its own thread and listens for
 *      WM_DEVICECHANGE, forwarding every device arrival/removal to
 *      hwwatcher.HardwareWatcher.fireHardwareChanged(...) in Java.
 *
 *   2. On demand, walks the full device tree with SetupDiGetClassDevs
 *      and builds an array of DeviceInfo objects - including devices
 *      that used to be plugged in but aren't right now ("ghost"
 *      devices), determined by cross-checking against a DIGCF_PRESENT
 *      enumeration rather than CfgMgr32 status flags (see the comment
 *      on collectPresentInstanceIds for why).
 *
 * Toolchain: MinGW, either mingw32 or mingw64 (see build_mingw32.bat /
 * build_mingw64.bat next to this file). Only standard Windows SDK
 * headers are used - windows.h, setupapi.h, cfgmgr32.h, dbt.h - so
 * nothing extra needs to be installed beyond MSYS2/MinGW itself.
 */

#include <jni.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <dbt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

/* These three GUIDs are normally pulled from usbiodef.h / hidclass.h,
 * but not every MinGW/MSYS2 setup ships those DDK headers, so they're
 * just written out by hand here instead. Values are the standard,
 * documented Microsoft constants and won't ever change. */
static const GUID GUID_USB_DEVICE  = {0xA5DCBF10,0x6530,0x11D2,{0x90,0x1F,0x00,0xC0,0x4F,0xB9,0x51,0xED}};
static const GUID GUID_HID_DEVICE  = {0x4D1E55B2,0xF16F,0x11CF,{0x88,0xCB,0x00,0x11,0x11,0x00,0x00,0x30}};
static const GUID GUID_COMPORT     = {0x86E0D1E0,0x8089,0x11D0,{0x9C,0xE4,0x08,0x00,0x3E,0x30,0x1F,0x73}};
/* Network adapters (including USB WiFi/Ethernet dongles once a NIC
 * driver is bound to them) register under this GUID instead of
 * GUID_DEVINTERFACE_USB_DEVICE - without this, their arrival/removal
 * events were being correctly delivered but silently classified as
 * "OTHER", making them look like they weren't being detected at all. */
static const GUID GUID_NET_DEVICE  = {0xCAC88484,0x7515,0x4C03,{0x82,0xE6,0x71,0xA8,0x7A,0xBA,0xC3,0x61}};

/* Everything below is process-wide state for a single watcher. There's
 * no support for more than one HardwareWatcher instance running at
 * once - the Java side is written as a singleton, and this mirrors it. */
static JavaVM   *g_jvm = NULL;
static jobject   g_watcherGlobalRef = NULL;   /* the live HardwareWatcher instance, held as a global ref */
static jmethodID g_fireMethod = NULL;         /* fireHardwareChanged(String,String,String,String,String) */
static jclass    g_deviceInfoClass = NULL;    /* DeviceInfo class, held as a global ref */
static jmethodID g_deviceInfoCtor = NULL;

static HANDLE    g_thread = NULL;
static DWORD     g_threadId = 0;
static HWND      g_hwnd = NULL;
static HDEVNOTIFY g_notifyHandle = NULL;
static volatile LONG g_running = 0;

/* Guards initialization of g_deviceInfoClass/g_deviceInfoCtor, since
 * they can now be lazily populated from either nativeStart() (called
 * on whatever Java thread invokes start()) or nativeListDevices()
 * (which - unlike start() - was always meant to be callable on its
 * own, without start() ever having run first). Without this lock two
 * threads racing into the lazy-init path at once could each create
 * and leak a duplicate global ref. */
static CRITICAL_SECTION g_classInitLock;
static volatile LONG g_classInitLockReady = 0;

#define WM_HWWATCH_STOP (WM_APP + 1)
static const wchar_t *WND_CLASS_NAME = L"HwWatchMsgOnlyWindow";

/* Defined near JNI_OnLoad further down - forward-declared here since
 * nativeListDevices (below) needs to call it before its full
 * definition appears in the file. */
static void ensureDeviceInfoClassCached(JNIEnv *env);

/* ------------------------------------------------------------------ */
/* Small string/registry helpers                                      */
/* ------------------------------------------------------------------ */

/* Converts a wide string to a heap-allocated UTF-8 C string. Never
 * returns NULL - an empty string comes back instead so callers don't
 * need a null check on every use. Caller owns the result and must free() it. */
static char *wideToUtf8(const wchar_t *w) {
    if (!w) return _strdup("");
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return _strdup("");
    char *out = (char *)malloc(len);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, len, NULL, NULL);
    return out;
}

/* Pulls one SPDRP_* registry property off a device as a UTF-8 string.
 * Devices frequently don't have a given property at all (e.g. a ROOT
 * device with no manufacturer string) - that's not an error, it just
 * comes back as "". Caller owns the result and must free() it. */
static char *getDevRegPropertyUtf8(HDEVINFO hDevInfo, PSP_DEVINFO_DATA devData, DWORD prop) {
    DWORD dataType = 0, reqSize = 0;
    SetupDiGetDeviceRegistryPropertyW(hDevInfo, devData, prop, &dataType, NULL, 0, &reqSize);
    if (reqSize == 0) return _strdup("");

    BYTE *buf = (BYTE *)malloc(reqSize + 2);
    memset(buf, 0, reqSize + 2);
    if (!SetupDiGetDeviceRegistryPropertyW(hDevInfo, devData, prop, &dataType, buf, reqSize, &reqSize)) {
        free(buf);
        return _strdup("");
    }
    char *out = wideToUtf8((wchar_t *)buf);
    free(buf);
    return out;
}

/* Not every device has a COM port - only the ones sitting under
 * "Ports (COM & LPT)" do. For those, Windows stores the assigned
 * name ("COM3" etc) as a "PortName" value in the device's own
 * hardware registry key, so that's what gets read here. Devices with
 * no such key/value just come back as "". Caller owns the result. */
static char *getComPortNameUtf8(HDEVINFO hDevInfo, PSP_DEVINFO_DATA devData) {
    HKEY hKey = SetupDiOpenDevRegKey(hDevInfo, devData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (hKey == INVALID_HANDLE_VALUE) return _strdup("");

    wchar_t value[64];
    DWORD size = sizeof(value);
    DWORD type = 0;
    char *result = _strdup("");
    if (RegQueryValueExW(hKey, L"PortName", NULL, &type, (LPBYTE)value, &size) == ERROR_SUCCESS) {
        value[(size / sizeof(wchar_t)) < 64 ? (size / sizeof(wchar_t)) : 63] = L'\0';
        free(result);
        result = wideToUtf8(value);
    }
    RegCloseKey(hKey);
    return result;
}

/* Case-insensitive strstr - the standard library doesn't reliably
 * provide one across MinGW/MSVC, so this is a small hand-rolled
 * version used by parseUsbIds below. */
static const char *strstrCaseInsensitive(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

/* Pulls VID/PID/serial out of a raw PnP instance id or device path,
 * e.g. "USB\VID_2341&PID_0043\5573323833514..." -> vid=2341, pid=0043,
 * serial=5573323833514... Works whether the string came from
 * SetupDiGetDeviceInstanceId or straight out of a WM_DEVICECHANGE
 * notification, since both use the same VID_/PID_ convention.
 * Matching is case-insensitive - real Windows instance IDs are always
 * uppercase, but this doesn't assume that.
 * out_vid/out_pid need >= 8 bytes, out_serial needs >= 64 bytes. */
static void parseUsbIds(const char *instanceId, char *out_vid, char *out_pid, char *out_serial) {
    out_vid[0] = out_pid[0] = out_serial[0] = '\0';

    const char *vidPos = strstrCaseInsensitive(instanceId, "VID_");
    if (vidPos) {
        strncpy(out_vid, vidPos + 4, 4);
        out_vid[4] = '\0';
    }

    const char *pidPos = strstrCaseInsensitive(instanceId, "PID_");
    if (pidPos) {
        strncpy(out_pid, pidPos + 4, 4);
        out_pid[4] = '\0';
    }

    /* Whatever trails the final backslash is treated as the serial -
     * good enough for typical USB-serial adapters, though some devices
     * put an interface number here instead of a real serial number. */
    const char *lastSlash = strrchr(instanceId, '\\');
    if (lastSlash && *(lastSlash + 1) != '\0') {
        strncpy(out_serial, lastSlash + 1, 63);
        out_serial[63] = '\0';
    }
}

/* Loose "is this a virtual COM port driver" check. This is a denylist
 * of driver service names known to belong to software-only serial
 * port emulators (com0com and friends) - add more here as you run
 * into them. Combined with the ROOT-enumerator check at the call
 * site, this is what backs DeviceInfo.isVirtual(). */
static int isKnownVirtualService(const char *service) {
    static const char *virtualServices[] = {
        "com0com", "vspd", "vspe", "vcp", "vcom", "vcxcom", "cdc_acm_virtual", NULL
    };
    if (!service || !service[0]) return 0;

    char lower[128];
    size_t i;
    for (i = 0; i < sizeof(lower) - 1 && service[i]; i++) lower[i] = (char)tolower((unsigned char)service[i]);
    lower[i] = '\0';

    for (int k = 0; virtualServices[k]; k++) {
        if (strstr(lower, virtualServices[k])) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Device enumeration: SetupAPI -> DeviceInfo[]                       */
/* ------------------------------------------------------------------ */

/* Plain C staging struct - one full device snapshot gets collected
 * into one of these before any JNI object gets built. Building the
 * Java objects only happens after the whole SetupAPI walk is done, so
 * the count is known up front and the jobjectArray can be allocated
 * at exactly the right size in a single pass. */
typedef struct {
    char *deviceId, *friendlyName, *description, *manufacturer;
    char *className, *enumeratorName, *service;
    char vid[8], pid[8], serial[64], comPort[16];
    jboolean present, isVirtual;
} DeviceRec;

static void freeDeviceRec(DeviceRec *r) {
    free(r->deviceId); free(r->friendlyName); free(r->description);
    free(r->manufacturer); free(r->className); free(r->enumeratorName); free(r->service);
}

/* Runs a DIGCF_PRESENT-filtered enumeration and returns the instance
 * IDs it finds. This is used as the ground truth for "is this device
 * present right now" - the same filter Device Manager's own hidden-
 * devices toggle is built on - rather than CM_Get_DevNode_Status's
 * DN_STARTED bit, which turned out to disagree with it for at least
 * one real network adapter driver (reporting a fully working, Device
 * Manager-visible device as not started). Caller owns the returned
 * array and must free each entry, then the array itself. */
static char **collectPresentInstanceIds(size_t *outCount) {
    *outCount = 0;
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (hDevInfo == INVALID_HANDLE_VALUE) return NULL;

    size_t capacity = 64, count = 0;
    char **ids = (char **)malloc(sizeof(char *) * capacity);

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(SP_DEVINFO_DATA);
    for (DWORD idx = 0; SetupDiEnumDeviceInfo(hDevInfo, idx, &devData); idx++) {
        if (count == capacity) {
            capacity *= 2;
            ids = (char **)realloc(ids, sizeof(char *) * capacity);
        }
        wchar_t instBuf[512];
        DWORD instLen = 0;
        if (SetupDiGetDeviceInstanceIdW(hDevInfo, &devData, instBuf, 512, &instLen)) {
            ids[count++] = wideToUtf8(instBuf);
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    *outCount = count;
    return ids;
}

static int isInStringSet(const char *needle, char **set, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (_stricmp(needle, set[i]) == 0) return 1;
    }
    return 0;
}

static void freeStringSet(char **set, size_t count) {
    if (!set) return;
    for (size_t i = 0; i < count; i++) free(set[i]);
    free(set);
}

/*
 * Called from Java as HardwareWatcher.nativeListDevices(includeGhosts).
 * Enumerates every device node Windows knows about, optionally
 * including ones that aren't currently plugged in (DIGCF_PRESENT
 * omitted), and returns one DeviceInfo per device. Grouping/filtering
 * by class is left to the Java side - this just hands back everything.
 */
JNIEXPORT jobjectArray JNICALL Java_hwwatcher_HardwareWatcher_nativeListDevices
  (JNIEnv *env, jobject thisObj, jboolean includeGhosts) {

    (void)thisObj;

    ensureDeviceInfoClassCached(env);
    if (g_deviceInfoClass == NULL) {
        /* FindClass/GetMethodID failed - there's a pending Java exception
         * already describing why. Returning NULL here lets that exception
         * surface normally instead of crashing on a NULL class reference. */
        return NULL;
    }

    /* Only needed when ghosts are in scope - when they're not, the
     * DIGCF_PRESENT flag on the main enumeration below already
     * guarantees every device it returns is present, so there's
     * nothing further to check. */
    char **presentIds = NULL;
    size_t presentCount = 0;
    if (includeGhosts) {
        presentIds = collectPresentInstanceIds(&presentCount);
    }

    DWORD flags = DIGCF_ALLCLASSES;
    if (!includeGhosts) flags |= DIGCF_PRESENT;

    HDEVINFO hDevInfo = SetupDiGetClassDevsW(NULL, NULL, NULL, flags);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        freeStringSet(presentIds, presentCount);
        return (*env)->NewObjectArray(env, 0, g_deviceInfoClass, NULL);
    }

    /* Grow-as-needed staging array so the SetupAPI walk only needs one pass. */
    size_t capacity = 64, count = 0;
    DeviceRec *recs = (DeviceRec *)malloc(sizeof(DeviceRec) * capacity);

    SP_DEVINFO_DATA devData;
    devData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD idx = 0; SetupDiEnumDeviceInfo(hDevInfo, idx, &devData); idx++) {
        if (count == capacity) {
            capacity *= 2;
            recs = (DeviceRec *)realloc(recs, sizeof(DeviceRec) * capacity);
        }
        DeviceRec *r = &recs[count];
        memset(r, 0, sizeof(DeviceRec));

        wchar_t instBuf[512];
        DWORD instLen = 0;
        char *instanceIdUtf8 = _strdup("");
        if (SetupDiGetDeviceInstanceIdW(hDevInfo, &devData, instBuf, 512, &instLen)) {
            free(instanceIdUtf8);
            instanceIdUtf8 = wideToUtf8(instBuf);
        }
        r->deviceId = instanceIdUtf8;

        r->friendlyName    = getDevRegPropertyUtf8(hDevInfo, &devData, SPDRP_FRIENDLYNAME);
        r->description      = getDevRegPropertyUtf8(hDevInfo, &devData, SPDRP_DEVICEDESC);
        r->manufacturer     = getDevRegPropertyUtf8(hDevInfo, &devData, SPDRP_MFG);
        r->className        = getDevRegPropertyUtf8(hDevInfo, &devData, SPDRP_CLASS);
        r->enumeratorName   = getDevRegPropertyUtf8(hDevInfo, &devData, SPDRP_ENUMERATOR_NAME);
        r->service          = getDevRegPropertyUtf8(hDevInfo, &devData, SPDRP_SERVICE);

        char vid[8] = "", pid[8] = "", serial[64] = "";
        parseUsbIds(r->deviceId, vid, pid, serial);
        memcpy(r->vid, vid, sizeof(r->vid));
        memcpy(r->pid, pid, sizeof(r->pid));
        memcpy(r->serial, serial, sizeof(r->serial));

        char *comPort = getComPortNameUtf8(hDevInfo, &devData);
        strncpy(r->comPort, comPort, sizeof(r->comPort) - 1);
        r->comPort[sizeof(r->comPort) - 1] = '\0';
        free(comPort);

        /* When ghosts aren't in scope, DIGCF_PRESENT on this very
         * enumeration already guarantees every device reaching this
         * point is present - no further check needed. When ghosts are
         * in scope, presence comes from set membership in the separate
         * DIGCF_PRESENT-filtered id list collected above, not from
         * CM_Get_DevNode_Status/DN_STARTED (which disagreed with
         * DIGCF_PRESENT for at least one real NIC driver - see the
         * comment on collectPresentInstanceIds). */
        r->present = (!includeGhosts || isInStringSet(r->deviceId, presentIds, presentCount))
                     ? JNI_TRUE : JNI_FALSE;

        r->isVirtual = (_stricmp(r->enumeratorName, "ROOT") == 0 || isKnownVirtualService(r->service))
                       ? JNI_TRUE : JNI_FALSE;

        count++;
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    freeStringSet(presentIds, presentCount);

    /* Second pass: now that the exact device count is known, build the
     * real Java objects and drop them into a correctly-sized array. */
    jobjectArray result = (*env)->NewObjectArray(env, (jsize)count, g_deviceInfoClass, NULL);
    for (size_t i = 0; i < count; i++) {
        DeviceRec *r = &recs[i];
        jstring jDeviceId = (*env)->NewStringUTF(env, r->deviceId);
        jstring jFriendly = (*env)->NewStringUTF(env, r->friendlyName);
        jstring jDesc     = (*env)->NewStringUTF(env, r->description);
        jstring jMfg      = (*env)->NewStringUTF(env, r->manufacturer);
        jstring jClass    = (*env)->NewStringUTF(env, r->className);
        jstring jEnum     = (*env)->NewStringUTF(env, r->enumeratorName);
        jstring jService  = (*env)->NewStringUTF(env, r->service);
        jstring jVid      = (*env)->NewStringUTF(env, r->vid);
        jstring jPid      = (*env)->NewStringUTF(env, r->pid);
        jstring jSerial   = (*env)->NewStringUTF(env, r->serial);
        jstring jComPort  = (*env)->NewStringUTF(env, r->comPort);

        jobject obj = (*env)->NewObject(env, g_deviceInfoClass, g_deviceInfoCtor,
                                         jDeviceId, jFriendly, jDesc, jMfg,
                                         jClass, jEnum, jService,
                                         jVid, jPid, jSerial, jComPort,
                                         r->present, r->isVirtual);
        (*env)->SetObjectArrayElement(env, result, (jsize)i, obj);

        /* Local refs pile up fast in a loop like this one - clean up
         * as we go instead of waiting for the whole call to return. */
        (*env)->DeleteLocalRef(env, jDeviceId);
        (*env)->DeleteLocalRef(env, jFriendly);
        (*env)->DeleteLocalRef(env, jDesc);
        (*env)->DeleteLocalRef(env, jMfg);
        (*env)->DeleteLocalRef(env, jClass);
        (*env)->DeleteLocalRef(env, jEnum);
        (*env)->DeleteLocalRef(env, jService);
        (*env)->DeleteLocalRef(env, jVid);
        (*env)->DeleteLocalRef(env, jPid);
        (*env)->DeleteLocalRef(env, jSerial);
        (*env)->DeleteLocalRef(env, jComPort);
        (*env)->DeleteLocalRef(env, obj);

        freeDeviceRec(r);
    }
    free(recs);
    return result;
}

/* ------------------------------------------------------------------ */
/* Live WM_DEVICECHANGE watcher                                       */
/* ------------------------------------------------------------------ */

/* Known GUIDs get a short friendly name. Anything else comes back as
 * "OTHER:{the-actual-guid}" rather than a bare "OTHER" - that way an
 * unrecognized device class is still fully identifiable from the log
 * instead of disappearing into an undifferentiated bucket, and a new
 * GUID constant can be added above once you know what it is. */
static void classifyGuid(const GUID *g, char *out, size_t outSize) {
    if (IsEqualGUID(g, &GUID_USB_DEVICE)) { snprintf(out, outSize, "USB"); return; }
    if (IsEqualGUID(g, &GUID_HID_DEVICE)) { snprintf(out, outSize, "HID"); return; }
    if (IsEqualGUID(g, &GUID_COMPORT))    { snprintf(out, outSize, "PORT"); return; }
    if (IsEqualGUID(g, &GUID_NET_DEVICE)) { snprintf(out, outSize, "NET"); return; }
    snprintf(out, outSize, "OTHER:{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             g->Data1, g->Data2, g->Data3,
             g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
             g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

/* Bridges a device event from the native watcher thread back onto the
 * JVM. The watcher thread was never created by the JVM, so it isn't
 * attached by default - GetEnv() detects that and AttachCurrentThread
 * fixes it up for the duration of this one call. */
static void callbackToJava(const char *changeType, const char *deviceClass,
                            const char *vid, const char *pid, const char *path) {
    JNIEnv *env = NULL;
    int mustDetach = 0;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, (void **)&env, NULL) != 0) return;
        mustDetach = 1;
    }

    jstring jChange = (*env)->NewStringUTF(env, changeType);
    jstring jClass  = (*env)->NewStringUTF(env, deviceClass);
    jstring jVid    = (*env)->NewStringUTF(env, vid);
    jstring jPid    = (*env)->NewStringUTF(env, pid);
    jstring jPath   = (*env)->NewStringUTF(env, path);

    (*env)->CallVoidMethod(env, g_watcherGlobalRef, g_fireMethod, jChange, jClass, jVid, jPid, jPath);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); /* never let a Java-side exception kill this thread */

    (*env)->DeleteLocalRef(env, jChange);
    (*env)->DeleteLocalRef(env, jClass);
    (*env)->DeleteLocalRef(env, jVid);
    (*env)->DeleteLocalRef(env, jPid);
    (*env)->DeleteLocalRef(env, jPath);

    if (mustDetach) (*g_jvm)->DetachCurrentThread(g_jvm);
}

/* Window procedure for the hidden message-only window. This is the
 * thing WM_DEVICECHANGE actually gets delivered to - everything else
 * in this file exists to set this window up and feed its output to Java. */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DEVICECHANGE) {
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            PDEV_BROADCAST_HDR hdr = (PDEV_BROADCAST_HDR)lParam;
            const char *changeType = (wParam == DBT_DEVICEARRIVAL) ? "ARRIVAL" : "REMOVAL";

            if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                PDEV_BROADCAST_DEVICEINTERFACE_W di = (PDEV_BROADCAST_DEVICEINTERFACE_W)hdr;
                char *pathUtf8 = wideToUtf8(di->dbcc_name);
                char vid[8] = "", pid[8] = "", serial[64] = "";
                parseUsbIds(pathUtf8, vid, pid, serial);
                char cls[80];
                classifyGuid(&di->dbcc_classguid, cls, sizeof(cls));
                callbackToJava(changeType, cls, vid, pid, pathUtf8);
                free(pathUtf8);
            } else if (hdr) {
                /* Something changed, but not through the DEVICEINTERFACE
                 * path this code normally expects (dbch_devicetype is one
                 * of the DBT_DEVTYP_* constants - VOLUME=2, PORT=3,
                 * HANDLE=6, etc). Reported anyway, instead of vanishing
                 * silently, so a device using a different notification
                 * shape than expected still shows up somewhere. */
                char cls[32];
                snprintf(cls, sizeof(cls), "RAWTYPE:%lu", (unsigned long)hdr->dbch_devicetype);
                callbackToJava(changeType, cls, "", "", "");
            }
        }
        return TRUE;
    }
    if (msg == WM_HWWATCH_STOP) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* Runs on its own dedicated thread for the life of the watcher: creates
 * the hidden window, registers for device notifications, then just
 * pumps messages until nativeStop() posts WM_HWWATCH_STOP. */
static DWORD WINAPI watcherThreadProc(LPVOID param) {
    (void)param;
    HINSTANCE hInst = GetModuleHandleW(NULL);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = WND_CLASS_NAME;
    RegisterClassW(&wc);

    /* HWND_MESSAGE = message-only window: no visible surface, never
     * shows up in the taskbar or Alt-Tab, exists purely to receive
     * WM_DEVICECHANGE broadcasts. */
    g_hwnd = CreateWindowExW(0, WND_CLASS_NAME, L"hwwatch", 0,
                              0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE_W filter;
    memset(&filter, 0, sizeof(filter));
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    /* The GUID field is left zeroed on purpose - Windows ignores it
     * once DEVICE_NOTIFY_ALL_INTERFACE_CLASSES is set below, which is
     * what makes this watcher pick up USB/HID/serial/everything else
     * through one single registration instead of one per class. */
    g_notifyHandle = RegisterDeviceNotificationW(g_hwnd, &filter,
        DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

    InterlockedExchange(&g_running, 1);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_notifyHandle) {
        UnregisterDeviceNotification(g_notifyHandle);
        g_notifyHandle = NULL;
    }
    UnregisterClassW(WND_CLASS_NAME, hInst);
    g_hwnd = NULL;
    InterlockedExchange(&g_running, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JNI entry points                                                    */
/* ------------------------------------------------------------------ */

/* Called automatically the moment System.load()/loadLibrary() pulls
 * this DLL in - stashes the JavaVM pointer for later thread attachment
 * (see callbackToJava) and sets up the lock used by ensureDeviceInfoClassCached. */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_jvm = vm;
    InitializeCriticalSection(&g_classInitLock);
    InterlockedExchange(&g_classInitLockReady, 1);
    return JNI_VERSION_1_6;
}

/* Looks up and caches the DeviceInfo class + its constructor the first
 * time it's needed, whether that's triggered by nativeStart() or by a
 * standalone nativeListDevices() call that happens before start() was
 * ever invoked. Safe to call repeatedly and from more than one thread -
 * after the first successful call every later call is just a null check.
 *
 * If class/constructor lookup fails, a pending Java exception is left
 * in place (ClassNotFoundError / NoSuchMethodError) and g_deviceInfoClass
 * stays NULL - callers must check for that afterwards and bail out
 * rather than handing a NULL class into any further JNI array/object call. */
static void ensureDeviceInfoClassCached(JNIEnv *env) {
    if (g_deviceInfoClass != NULL) return;   /* fast path - already cached */

    if (!InterlockedCompareExchange(&g_classInitLockReady, 1, 1)) return; /* JNI_OnLoad hasn't run yet - shouldn't happen */
    EnterCriticalSection(&g_classInitLock);

    if (g_deviceInfoClass == NULL) {   /* re-check now that we hold the lock */
        jclass local = (*env)->FindClass(env, "hwwatcher/DeviceInfo");
        if (local != NULL) {
            jmethodID ctor = (*env)->GetMethodID(env, local, "<init>",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;ZZ)V");
            if (ctor != NULL) {
                g_deviceInfoCtor = ctor;
                g_deviceInfoClass = (jclass)(*env)->NewGlobalRef(env, local);
            }
            (*env)->DeleteLocalRef(env, local);
        }
    }

    LeaveCriticalSection(&g_classInitLock);
}

/* Called from Java as HardwareWatcher.nativeStart(). Looks up the
 * method/class handles it'll need for callbacks, then spins up the
 * watcher thread and waits (briefly) for its message loop to be alive
 * before returning, so a caller doing start() then addListener() isn't
 * racing the thread startup. */
JNIEXPORT void JNICALL Java_hwwatcher_HardwareWatcher_nativeStart
  (JNIEnv *env, jobject thisObj) {

    if (InterlockedCompareExchange(&g_running, 0, 0) != 0) return; /* already running - nothing to do */

    g_watcherGlobalRef = (*env)->NewGlobalRef(env, thisObj);
    jclass watcherClass = (*env)->GetObjectClass(env, thisObj);
    g_fireMethod = (*env)->GetMethodID(env, watcherClass,
        "fireHardwareChanged",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");

    ensureDeviceInfoClassCached(env);
    if (g_deviceInfoClass == NULL || g_fireMethod == NULL) {
        /* Something about the Java side doesn't match what this native
         * build expects (wrong class version, mismatched signature after
         * an edit to HardwareWatcher.java/DeviceInfo.java, etc). Bail out
         * instead of starting a watcher thread that would eventually
         * crash trying to call back into Java. The pending exception from
         * FindClass/GetMethodID (if any) explains why. */
        return;
    }

    g_thread = CreateThread(NULL, 0, watcherThreadProc, NULL, 0, &g_threadId);
    for (int i = 0; i < 200 && InterlockedCompareExchange(&g_running, 0, 0) == 0; i++) {
        Sleep(5);
    }
}

/* Called from Java as HardwareWatcher.nativeStop(). Politely asks the
 * watcher thread's window to close (which drops it out of the message
 * loop), waits for it to actually exit, and releases the global ref
 * on the Java-side watcher instance. */
JNIEXPORT void JNICALL Java_hwwatcher_HardwareWatcher_nativeStop
  (JNIEnv *env, jobject thisObj) {
    (void)thisObj;
    if (g_hwnd) {
        PostMessageW(g_hwnd, WM_HWWATCH_STOP, 0, 0);
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_watcherGlobalRef) {
        (*env)->DeleteGlobalRef(env, g_watcherGlobalRef);
        g_watcherGlobalRef = NULL;
    }
}
