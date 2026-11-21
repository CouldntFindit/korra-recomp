/* gate_probe_android.c - quarterly-second census of the technique slots (debug).
 *
 * Same question as src/host/gate_probe.c, on the device. Two runs have narrowed
 * it (2026-11-21 23:40 and 23:44, debug.korra.gate=1):
 *
 *   1. [mgr+0x6C] is 0 for the whole session - the frame-setup skip gate
 *      (candidate 1) is not the mechanism.
 *   2. All six per-pass technique slots hold their static descriptor addresses
 *      (0xE1DA54/0xE1DA6C, 0x9DF480, 0x9DF450, 0xE1DBD8, 0xE1DAA0's field), so
 *      sub_005599D0 (a constant re-initializer, not a table builder) does run
 *      and no slot is NULL (candidate 3's "table emptied" is wrong too).
 *   3. [0xE4D094] - the technique pointer 00748CB0 copies to ctx+0x30 - is
 *      NEVER written non-zero; only the valid flag at [0xE4D098] flips 1/0.
 *
 * So 00748CB0 is reached but hands the context a descriptor whose [0] is 0, and
 * the crashing pass ({fn=0074F8F0, key=6B100000}) later reads its own
 * NULL technique field. This version logs the two candidates for that field:
 * the singleton at [0xE46940] (built by 0x732920, vtable 0x96C88C) and its
 * +0x30, together with the four probe objects 00748C30 compares against
 * (ctx+8/+0x10/+0x18/+0x20) and the device object's +4/+8 pair.
 *
 *   adb shell setprop debug.korra.gate 1
 *
 * Lines are read once; when the property is absent the tick costs one compare.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <sys/system_properties.h>

#include "host_android.h"

static int      s_read, s_on;
static uint32_t s_last;

/* The guest space is one 4 GiB PROT_NONE reservation: only committed pages are
 * readable, so a stale pointer read through here faults inside the probe and the
 * crash handler blames whatever guest function was running (that cost two runs
 * on 2026-11-21 23:45/23:47). Only dereference values that look like the app's
 * own objects: image (0x400000), heap (0x20000000-0x30000000), thread stacks. */
static uint32_t rd(uint32_t va) {
    if (va < 0x00400000u || va >= 0x40000000u) return 0;
    return MEM32(va);
}

void ka_gate_tick(void) {
    uint32_t m, mgr, st_p, st, flag, ctx30, tce;
    uint32_t s39dc, s39e8, s39f4, s39fc, s3a00, sdaa0, t39d8;
    uint32_t ctx, dev, dev4, dev8, c04, l04, c08, c10, c18, c20, gp, gp30;
    uint32_t ctx34, a08, b08, a10, b10, a18, b18, a20, b20;
    uint32_t t1, t2, t1_14, t1_30, t1_34, t2_14, t2_30, t2_34;
    uint32_t o14, o18, o30, o34, n14, n18, n30, n34;
    uint32_t o0, o8, oc, n0, n8, nc;
    uint32_t now;

    if (!s_read) {
        char buf[PROP_VALUE_MAX] = {0};
        s_read = 1;
        __system_property_get("debug.korra.gate", buf);
        s_on = buf[0] && buf[0] != '0';
        if (s_on) ka_log("[gate] census on (debug.korra.gate=1)");
    }
    if (!s_on) return;

    now = ka_mono_ms();
    if (s_last && now - s_last < 250u) return;
    s_last = now ? now : 1u;

    m     = MEM32(0xE4D060u);
    mgr   = m ? MEM32(m + 0x28u) : 0;
    st_p  = mgr ? MEM32(mgr + 0x24u) : 0;
    st    = st_p ? MEM32(st_p) : 0;
    flag  = mgr ? MEM32(mgr + 0x6Cu) : 0;
    ctx30 = MEM32(0xE4D094u);
    tce   = MEM32(0xCA3CE0u);
    t39d8 = MEM32(0xCA39D8u);
    s39dc = MEM32(0xCA39DCu);
    s39e8 = MEM32(0xCA39E8u);
    s39f4 = MEM32(0xCA39F4u);
    s39fc = MEM32(0xCA39FCu);
    s3a00 = MEM32(0xCA3A00u);
    sdaa0 = MEM32(0xE1DAA0u);

    /* The context object the install writes into is the static 0xE4D064. */
    ctx   = 0xE4D064u;
    dev   = MEM32(ctx);
    dev4  = dev ? rd(dev + 4u) : 0;
    dev8  = dev ? rd(dev + 8u) : 0;
    /* 0x748580 sets ctx+8 = [[ctx+4]] + 0xC each frame, and 0x748CB0 bails when
     * ctx+8 is 0 - so the list at [ctx+4] is the gate on every technique
     * install. Log the chain, not just its result. */
    c04   = MEM32(ctx + 0x04u);
    l04   = c04 ? rd(c04) : 0;
    c08   = MEM32(ctx + 0x08u);
    c10   = MEM32(ctx + 0x10u);
    c18   = MEM32(ctx + 0x18u);
    c20   = MEM32(ctx + 0x20u);
    gp    = MEM32(0xE46940u);
    gp30  = gp ? rd(gp + 0x30u) : 0;
    ctx34 = MEM32(0xE4D098u);

    /* 00748C30 matches the pair ([obj+4],[obj+8]) of [ctx] against the same
     * pair of ctx+8/+0x10/+0x18/+0x20. Log both halves, guarded. */
    a08 = c08 ? rd(c08 + 4u) : 0;  b08 = c08 ? rd(c08 + 8u) : 0;
    a10 = c10 ? rd(c10 + 4u) : 0;  b10 = c10 ? rd(c10 + 8u) : 0;
    a18 = c18 ? rd(c18 + 4u) : 0;  b18 = c18 ? rd(c18 + 8u) : 0;
    a20 = c20 ? rd(c20 + 4u) : 0;  b20 = c20 ? rd(c20 + 8u) : 0;

    /* The technique objects (070756600's arg1) carry the cached pass state at
     * +0x30: [t+0x30]+4 is what SetRenderTarget(0, ...) receives on the cached
     * path. On the device that dword is 0 while [t+0x30] itself is non-zero,
     * so the engine unbinds RT0 and DXVK answers D3DERR_INVALIDCALL, which
     * makes 756600 return 0 and 748CB0 skip the technique copy. Log both
     * technique slots the frames use, plus the shader/group pair 748B30 reads. */
    t1     = MEM32(0xCA3A00u);
    t2     = MEM32(0xCA39E8u);
    t1_14  = t1 ? rd(t1 + 0x14u) : 0;
    t1_30  = t1 ? rd(t1 + 0x30u) : 0;
    t1_34  = t1_30 ? rd(t1_30 + 4u) : 0;
    t2_14  = t2 ? rd(t2 + 0x14u) : 0;
    t2_30  = t2 ? rd(t2 + 0x30u) : 0;
    t2_34  = t2_30 ? rd(t2_30 + 4u) : 0;

    ka_log("[gate] flag=%08X st=%08X ctx30=%08X tce=%08X"
           " slots dc=%08X e8=%08X f4=%08X fc=%08X 3a00=%08X",
           flag, st, ctx30, tce,
           s39dc, s39e8, s39f4, s39fc, s3a00);
    ka_log("[gate] tech 3a00=%08X t4=%08X t8=%08X t14=%08X t30=%08X t30p4=%08X",
           t1, t1 ? rd(t1 + 4u) : 0, t1 ? rd(t1 + 8u) : 0, t1_14, t1_30, t1_34);
    ka_log("[gate] tech 39e8=%08X t4=%08X t8=%08X t14=%08X t30=%08X t30p4=%08X",
           t2, t2 ? rd(t2 + 4u) : 0, t2 ? rd(t2 + 8u) : 0, t2_14, t2_30, t2_34);

    /* 00748B30 does not hand 00756600 the table entry: it hands [entry+0x14],
     * a heap object (0x200C42A0 for 0xE1DA6C). That object's +0x30 is the
     * pass state whose +4 the cached path binds, so read the object, not the
     * table entry. */
    o14 = t1_14 ? rd(t1_14 + 0x14u) : 0;
    o18 = t1_14 ? rd(t1_14 + 0x18u) : 0;
    o30 = t1_14 ? rd(t1_14 + 0x30u) : 0;
    o34 = o30 ? rd(o30 + 4u) : 0;
    o0 = o30 ? rd(o30) : 0;
    o8 = o30 ? rd(o30 + 8u) : 0;
    oc = o30 ? rd(o30 + 0xCu) : 0;
    ka_log("[gate] obj 3a00 t14=%08X o14=%08X o18=%08X o30=%08X o34=%08X p14=%08X p18=%08X"
           " o0=%08X o8=%08X oc=%08X",
           t1_14, o14, o18, o30, o34,
           o14 ? rd(o14 + 0x14u) : 0, o18 ? rd(o18 + 0x18u) : 0, o0, o8, oc);

    n14 = t2_14 ? rd(t2_14 + 0x14u) : 0;
    n18 = t2_14 ? rd(t2_14 + 0x18u) : 0;
    n30 = t2_14 ? rd(t2_14 + 0x30u) : 0;
    n34 = n30 ? rd(n30 + 4u) : 0;
    n0 = n30 ? rd(n30) : 0;
    n8 = n30 ? rd(n30 + 8u) : 0;
    nc = n30 ? rd(n30 + 0xCu) : 0;
    ka_log("[gate] obj 39e8 t14=%08X o14=%08X o18=%08X o30=%08X o34=%08X p14=%08X p18=%08X"
           " o0=%08X o8=%08X oc=%08X",
           t2_14, n14, n18, n30, n34,
           n14 ? rd(n14 + 0x14u) : 0, n18 ? rd(n18 + 0x18u) : 0, n0, n8, nc);
    ka_log("[gate] ctx dev=%08X d4=%08X d8=%08X v34=%08X c04=%08X l04=%08X"
           " c08=%08X c10=%08X c18=%08X c20=%08X gp=%08X gp30=%08X daa0=%08X",
           dev, dev4, dev8, ctx34, c04, l04, c08, c10, c18, c20, gp, gp30, sdaa0);

    /* The recorder's technique cache, and the two flags that gate it.
     * sub_00550280 binds pass 3 with `[0xE4CCF8] > 0xB ? [0xE4CCF4] + 0xDC : 0`
     * (0x5502FF), so a cache that is still empty there hands sub_007492B0 a
     * NULL technique and the flush faults reading [NULL+4] - the Task-2 crash
     * on the device. 0xE4CCF0 is the fill's own success flag (set at 0x73C564
     * from sub_007516C0's return), so these four dwords separate "the fill
     * never ran", "it ran and found nothing" and "it ran, then a reset zeroed
     * it". [0xDC0F68] is the pass-queue non-empty gate, [0xDC0F74] the
     * deferred-flush flag the binder tests, [0xE46E90] the in-pass flag. */
    ka_log("[gate] rec e46e90=%08X q74=%08X q68=%08X ccf0=%08X ccf4=%08X"
           " ccf8=%08X ccfc=%08X",
           MEM32(0xE46E90u), MEM32(0xDC0F74u), MEM32(0xDC0F68u),
           MEM32(0xE4CCF0u), MEM32(0xE4CCF4u), MEM32(0xE4CCF8u),
           MEM32(0xE4CCFCu));
    /* The package the technique cache comes from. sub_0053F82C resolves
     * "pre_texture.wta"/".wtp" through 0x73C4A0 against the object at
     * 0xDC0988, whose head is [0xDC0984]; 0x73C4A0 walks that chain with the
     * ASCII stricmp 0x77BE27 and only fills [0xE4CCF4] when BOTH names are
     * found. On the device the cache is still {0,0} at the crash, on Windows
     * it is {0B3F5EE4, 0xF} by the second census - so this line says whether
     * the package was never built (head 0), built without those two entries
     * (pages 0), or built and simply not consulted. */
    {
        uint32_t head = MEM32(0xDC0988u), page = head ? rd(head) : 0;
        ka_log("[gate] pkg d984=%08X d988=%08X d98c=%08X"
               " head0=%08X head4=%08X head8=%08X head10=%08X",
               MEM32(0xDC0984u), head, MEM32(0xDC098Cu),
               page ? rd(page) : 0, page ? rd(page + 4u) : 0,
               page ? rd(page + 8u) : 0, page ? rd(page + 0x10u) : 0);
    }
    ka_log("[gate] pair %08X:%08X | %08X:%08X | %08X:%08X | %08X:%08X",
           a08, b08, a10, b10, a18, b18, a20, b20);
}
