/* save_probe_android.c - the save-device init chain, call by call, on the phone.
 *
 * The counterpart of src/host/save_probe.c and the same idea: the save defect
 * is pre-filesystem, so the calls that DID run are the evidence. The save path
 * is built by the engine as
 *
 *     SHGetSpecialFolderPathA(NULL, buf, CSIDL_PERSONAL=5, FALSE)
 *     buf + "/Korra/"                       (0x76CB50, "%s/%s/")
 *     + "SystemData.dat" / "GameData.dat" / "SlotData_%d.dat"
 *
 * and the directory is checked with an enumeration of "<dir>/*.*" (0x76CC40).
 * Every one of those has a shim here (misc_android.c / kernel32_android.c), so
 * the answer each shim gives is the diff surface. This file owns the lines;
 * the shims only call the helpers below.
 *
 *   adb shell setprop debug.korra.trace_save 1
 *
 * Format matches the Windows probe exactly, so the two logs diff line by line:
 * the guest path, the host path it resolved to, and the answer.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

#include "host_android.h"

static int s_on = -1;

int ka_save_on(void) {
    if (s_on < 0) {
        char v[PROP_VALUE_MAX] = {0};
        __system_property_get("debug.korra.trace_save", v);
        s_on = (v[0] && v[0] != '0');
        if (s_on) ka_log("[save] probe on (debug.korra.trace_save=1)");
    }
    return s_on;
}

/* One ka_log call = one line = one write, the ledger's interleave rule. */
void ka_save_str(const char *call, const char *guest, const char *arg,
                 uint32_t res, int last) {
    if (!ka_save_on()) return;
    ka_log("[save] %s %s path=\"%s\" -> ret=%u last=%d", call, arg,
           guest ? guest : "?", res, last);
}

void ka_save_file(const char *call, const char *guest, const char *host,
                  uint32_t access, uint32_t share, uint32_t disp,
                  uint32_t res, int last) {
    if (!ka_save_on()) return;
    ka_log("[save] %s path=\"%s\" access=0x%08X share=0x%08X disp=%u -> 0x%08X last=%d"
           " (host=\"%s\")", call, guest ? guest : "?", access, share, disp,
           res, last, host ? host : "");
}

void ka_save_find(const char *call, const char *pat, const char *host,
                  uint32_t res, int last, const char *first) {
    if (!ka_save_on()) return;
    ka_log("[save] %s pat=\"%s\" -> 0x%08X last=%d first=\"%s\" (host=\"%s\")",
           call, pat ? pat : "?", res, last, first ? first : "",
           host ? host : "");
}

void ka_save_io(const char *call, uint32_t h, uint32_t n, uint32_t res,
                uint32_t count, int last) {
    if (!ka_save_on()) return;
    ka_log("[save] %s h=0x%08X n=%u -> ret=%u count=%u last=%d",
           call, h, n, res, count, last);
}

void ka_save_plain(const char *call, const char *arg, uint32_t res, int last) {
    if (!ka_save_on()) return;
    ka_log("[save] %s %s -> ret=%u last=%d", call, arg ? arg : "", res, last);
}
