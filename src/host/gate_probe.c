/*
 * gate_probe.c - one-second census of the frame-setup gate (temporary).
 *
 * sub_00541510 skips the whole per-frame setup - including the technique
 * table init sub_005599D0 - when
 *
 *     [0xE4D060] != 0 && [[0xE4D060] + 0x28] != 0
 *         && [[[[0xE4D060] + 0x28]] + 0x6C] != 0
 *
 * The getter in that condition is sub_0075A140 (vtable 0x56CB8C slot 34), a
 * one-instruction read of the manager's +0x6C flag; sub_00759C90 (slot 3)
 * raises that flag when the status dword held through the manager's +0x24 is
 * D3DERR_DEVICELOST (0x88760868) or D3DERR_DEVICENOTRESET (0x88760869), and
 * sub_00759D10 (slot 4) reports SUCCEEDED from the same dword. If the flag is
 * non-zero on the device every frame takes the skip path, the technique table
 * stays empty, and the pass-queue flush reads a NULL technique - the Task-2
 * crash. This probe prints the whole chain once a second so the Windows
 * reference values and the device values are directly comparable.
 *
 *   KORRA_GATE=1
 *   KORRA_GATE_FORCE=<n>         hold [mgr+0x6C] at n every tick, to reproduce
 *                                the closed-gate state on purpose. This is a
 *                                diagnostic: it makes the reference build behave
 *                                the way the device behaves, it is not a fix.
 *   KORRA_GATE_AT=<ms>           wait for that many GetTickCount() ms from the
 *                                first tick before forcing. The pass queue only
 *                                fills once the game reaches an area, so the
 *                                faithful reproduction is "let the game load
 *                                normally, then close the gate".
 *
 * m    = [0xE4D060]              manager owner (the render "device" singleton)
 * mgr  = [m + 0x28]              device-state manager
 * +24  = [mgr + 0x24]            pointer to the status dword
 * st   = [st_p]                  the status HRESULT the flag reacts to
 * flag = [mgr + 0x6C]            non-zero => frame setup skipped
 * ctx  = [0xE4D064 + 0x30]       technique slot the flush reads
 * tce  = [0xCA3CE0]              bit 24 recording, bit 25 deferred flush
 */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "recomp_types.h"

static int   s_read, s_on;
static int   s_force_on;
static uint32_t s_force;
static uint32_t s_force_at;
static DWORD s_base;
static DWORD s_last;

void korra_gate_tick(void) {
    uint32_t m, mgr, st_p, st, flag, ctx30, tce;
    DWORD    now;

    if (!s_read) {
        s_read = 1;
        s_on = getenv("KORRA_GATE") != NULL;
        {
            const char *v = getenv("KORRA_GATE_FORCE");
            if (v && *v) { s_force_on = 1; s_force = (uint32_t)strtoul(v, NULL, 0); }
        }
        {
            const char *v = getenv("KORRA_GATE_AT");
            if (v && *v) s_force_at = (uint32_t)strtoul(v, NULL, 0);
        }
        s_base = GetTickCount();
    }
    if (!s_on && !s_force_on) return;

    /* forcing closes the gate continuously, not once a second */
    if (s_force_on) {
        if (s_force_at && GetTickCount() - s_base < s_force_at) goto report;
        m = MEM32(0xE4D060u);
        mgr = m ? MEM32(m + 0x28u) : 0;
        if (mgr) MEM32(mgr + 0x6Cu) = s_force;
    }
report:
    if (!s_on) return;

    now = GetTickCount();
    if (s_last && now - s_last < 1000) return;
    s_last = now ? now : 1;

    m     = MEM32(0xE4D060u);
    mgr   = m ? MEM32(m + 0x28u) : 0;
    st_p  = mgr ? MEM32(mgr + 0x24u) : 0;
    st    = st_p ? MEM32(st_p) : 0;
    flag  = mgr ? MEM32(mgr + 0x6Cu) : 0;
    ctx30 = MEM32(0xE4D094u);
    tce   = MEM32(0xCA3CE0u);
    fprintf(stderr, "[gate] m=%08X mgr=%08X flag=%08X status=%08X"
                    " ctx30=%08X tce=%08X\n",
            m, mgr, flag, st, ctx30, tce);
    fflush(stderr);
}
