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

/* Guest reads must not fault the probe: the guest image occupies only the
 * committed part of the address range, exactly as on the device. */
static uint32_t rd(uint32_t va) {
    if (va < 0x00400000u || va >= 0x40000000u) return 0;
    return MEM32(va);
}

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
    /* The device-side census compares these against the Android run: the
     * gate binds each technique through sub_00756600, whose cached path
     * hands SetRenderTarget(0, [t+0x30]+4). Mirror every field the device
     * probe prints so the two logs can be diffed line for line. */
    {
        uint32_t t1 = MEM32(0xCA3A00u), t2 = MEM32(0xCA39E8u);
        uint32_t dev = MEM32(0xE4D064u);
        uint32_t c08 = MEM32(0xE4D06Cu), c10 = MEM32(0xE4D074u);
        uint32_t c18 = MEM32(0xE4D07Cu), c20 = MEM32(0xE4D084u);
        uint32_t t30 = rd(t2 + 0x30u);
        fprintf(stderr, "[gate] tech 3a00=%08X t4=%08X t8=%08X t14=%08X"
                        " t30=%08X t30p4=%08X\n",
                t1, rd(t1 + 4u), rd(t1 + 8u), rd(t1 + 0x14u),
                rd(t1 + 0x30u), rd(rd(t1 + 0x30u) + 4u));
        fprintf(stderr, "[gate] tech 39e8=%08X t4=%08X t8=%08X t14=%08X"
                        " t30=%08X t30p4=%08X\n",
                t2, rd(t2 + 4u), rd(t2 + 8u), rd(t2 + 0x14u), t30,
                rd(t30 + 4u));
        fprintf(stderr, "[gate] ctx dev=%08X d4=%08X d8=%08X"
                        " c08=%08X c10=%08X c18=%08X c20=%08X\n",
                dev, rd(dev + 4u), rd(dev + 8u), c08, c10, c18, c20);
        /* Same `rec` line as the device probe (gate_probe_android.c): the
         * recorder's technique cache and the flags that gate it. The binder
         * sub_00550280 at 0x5502FF passes
         * `[0xE4CCF8] > 0xB ? [0xE4CCF4] + 0xDC : 0` as the technique to
         * sub_007492B0 - a NULL there is the Task-2 fault, so this is the line
         * that says whether the cache is filled on this machine. */
        fprintf(stderr, "[gate] rec e46e90=%08X q74=%08X q68=%08X"
                        " ccf0=%08X ccf4=%08X ccf8=%08X ccfc=%08X\n",
                MEM32(0xE46E90u), MEM32(0xDC0F74u), MEM32(0xDC0F68u),
                MEM32(0xE4CCF0u), MEM32(0xE4CCF4u), MEM32(0xE4CCF8u),
                MEM32(0xE4CCFCu));
        /* The package the technique cache is resolved from - same line as the
         * device probe. sub_0053F82C looks up "pre_texture.wta"/".wtp" in the
         * object at 0xDC0988 through 0x73C4A0 and only fills [0xE4CCF4] when
         * both names are found. */
        {
            uint32_t head = MEM32(0xDC0988u), page = head ? rd(head) : 0;
            fprintf(stderr, "[gate] pkg d984=%08X d988=%08X d98c=%08X"
                            " head0=%08X head4=%08X head8=%08X head10=%08X\n",
                    MEM32(0xDC0984u), head, MEM32(0xDC098Cu),
                    page ? rd(page) : 0, page ? rd(page + 4u) : 0,
                    page ? rd(page + 8u) : 0, page ? rd(page + 0x10u) : 0);
        }
        /* 00748B30 hands 00756600 [entry+0x14], not the table entry, so the
         * cached pass state lives at [[t+0x14]+0x30]+4. Same lines as the
         * device probe, so the bind inputs can be diffed directly. */
        {
            uint32_t o = rd(t1 + 0x14u), n = rd(t2 + 0x14u);
            uint32_t o30 = rd(o + 0x30u), n30 = rd(n + 0x30u);
            uint32_t o14 = rd(o + 0x14u), o18 = rd(o + 0x18u);
            uint32_t n14 = rd(n + 0x14u), n18 = rd(n + 0x18u);
            fprintf(stderr, "[gate] obj 3a00 t14=%08X o14=%08X o18=%08X"
                            " o30=%08X o34=%08X p14=%08X p18=%08X\n",
                    o, o14, o18, o30, rd(o30 + 4u),
                    rd(o14 + 0x14u), rd(o18 + 0x18u));
            fprintf(stderr, "[gate] obj 39e8 t14=%08X o14=%08X o18=%08X"
                            " o30=%08X o34=%08X p14=%08X p18=%08X\n",
                    n, n14, n18, n30, rd(n30 + 4u),
                    rd(n14 + 0x14u), rd(n18 + 0x18u));
        }
    }
    fflush(stderr);
}
