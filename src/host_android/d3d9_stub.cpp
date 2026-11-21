/* d3d9_stub.cpp - the no-DXVK build (KORRA_LINK_GUEST without KORRA_WITH_DXVK).
 *
 * This is bring-up rung 2 of docs/ANDROID_BRINGUP.md section 5.6: the guest
 * runs, logs its imports and stops when it asks for a device. The log line is
 * deliberate - a build in this mode must never look like a working port.
 */
#include <stdint.h>

#include "host_android.h"

#if !KORRA_WITH_DXVK

extern "C" {

static void d3d_Direct3DCreate9(void) {
    uint32_t sdk = A32(0);
    ka_log("d3d9: Direct3DCreate9(sdk=%u) from 0x%08X - this build has no DXVK",
           sdk, g_cur_func);
    RET(0, 1);
}

const ka_shim ka_d3d9_shims[] = {
    { "Direct3DCreate9", d3d_Direct3DCreate9 },
    { NULL, NULL }
};

int ka_d3d9_init(void) {
    ka_shim_register(ka_d3d9_shims);
    ka_log("d3d9: stub bridge (build without -PkorraWithDxvk=ON)");
    return 0;
}

void ka_d3d9_window_changed(void) {}
int  ka_d3d9_frame_presented(void) { return 0; }
void ka_d3d9_shutdown(void) {}

/* The D3D9 bridge decides the `this` convention per call; with no bridge there
 * are no COM objects, so the flag stays 0 and nothing needs shifting. */
void ka_vtable_note_import(const char *name) { (void)name; }

int ka_d3dx_create_texture(void) { return (int)0x8876086Cu; }
int ka_d3dx_texture_from_memory(void) { return (int)0x8876086Cu; }
int ka_d3dx_cube_from_memory(void) { return (int)0x8876086Cu; }
int ka_d3dx_constant_table(void) { return (int)0x8876086Cu; }

} /* extern "C" */

#endif /* !KORRA_WITH_DXVK */
