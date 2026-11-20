/*
 * draw_probe.c - D3D9 draw-path census for the missing-text defect.
 *
 * The port renders every fixed-shape UI element (panel, ornaments, logo, brush
 * stroke, button art) but no glyphs: message-box text and menu text are absent.
 * "No glyphs" can break at three places, and they look identical on screen:
 *
 *   1. the glyph quad is never submitted     (engine-side maths / culling)
 *   2. it is submitted degenerate or NaN     (lifted per-glyph maths)
 *   3. it is submitted fine but drawn wrong  (texture, alpha test, shader,
 *      render state)
 *
 * This probe answers which, without touching guest code and without the Ghidra
 * lock: it rides native32_post_hook, which the runtime calls after every
 * bridged native call - imports and COM methods alike - so every
 * IDirect3DDevice9 call the lifted engine makes passes through here with its
 * argument slots and its return value.
 *
 * Output (stderr, also in the run's korra_host-*.log):
 *
 *   [draw] tex 0x0A1B2C3D 256x256 fmt=0x1C pool=1 from D3DXCreateTexture
 *   [draw] sig#3 first: DrawPrimitiveUP prims=2 stride=24 fvf=0x144
 *          tex=0x... 256x256 fmt=0x15 caller=sub_0058A1B0 v0={...}
 *   [draw] frame 120 draws=41 up=37 vb=4 prims=284 texs=3 fvf=0x144
 *          vs=0x00000000 ps=0x0048C2A0 states{abl=1 src=5 dst=6 zen=0 ...}
 *
 * Method identity does not rely on symbol names: the probe captures the device
 * object (IDirect3D9::CreateDevice's ppDevice, or the first COM call whose
 * vtable lives in d3d9.dll) and resolves each target against that vtable, so
 * slot 83 is DrawPrimitiveUP even if the runtime has no name for it.
 *
 * Knobs:
 *   KORRA_DRAWPATH=1        census on
 *   KORRA_DRAWPATH=2        plus every SetTexture change
 *   KORRA_DRAWPATH=3        plus per-draw lines for the first 8 hits/signature
 *
 * Off by default: with KORRA_DRAWPATH unset nothing is installed and the file
 * is inert. See docs/FINDINGS.md, 2026-11-20 "text".
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <d3d9.h>
#include "native32.h"

/* ---------------------------------------------------------------- D3D9 vtable
 * IDirect3DDevice9 slot order (d3d9.h). Only the slots the census needs are
 * named; the rest are resolved but ignored. */
#define D3D9_CREATEDEVICE           13
#define D3DDEV_PRESENT              17
#define D3DDEV_CREATETEXTURE        23
#define D3DDEV_CREATEVERTEXBUFFER   26
#define D3DDEV_CREATERENDERTARGET   28
#define D3DDEV_CREATEDEPTHSTENCIL   29
#define D3DDEV_SETRENDERTARGET      37
#define D3DDEV_SETDEPTHSTENCIL      39
#define D3DDEV_BEGINSCENE           41
#define D3DDEV_ENDSCENE             42
#define D3DDEV_SETTRANSFORM         44
#define D3DDEV_SETVIEWPORT          47
#define D3DDEV_SETRENDERSTATE       57
#define D3DDEV_SETTEXTURE           65
#define D3DDEV_SETTEXTURESTAGESTATE 67
#define D3DDEV_SETSAMPLERSTATE      69
#define D3DDEV_DRAWPRIMITIVE        81
#define D3DDEV_DRAWINDEXEDPRIMITIVE 82
#define D3DDEV_DRAWPRIMITIVEUP      83
#define D3DDEV_DRAWINDEXEDPRIMITIVEUP 84
#define D3DDEV_SETVERTEXDECLARATION 87
#define D3DDEV_SETFVF               89
#define D3DDEV_SETVERTEXSHADER      92
#define D3DDEV_SETPIXELSHADER       94
#define D3DDEV_SETSTREAMSOURCE      102
#define D3DDEV_SETINDICES           106

/* ---------------------------------------------------------------- state */

typedef struct {
    int      on;          /* 0 = inert, 1..3 = verbosity */
    uint32_t d3d9_lo, d3d9_hi;   /* d3d9.dll image range */
    uint32_t d3dx_lo, d3dx_hi;   /* d3dx9_43.dll image range */
    uint32_t d3d;                /* IDirect3D9* */
    uint32_t dev;                /* IDirect3DDevice9* */
    uint32_t m[128];             /* device method addresses by slot */
    int      have_dev;
    /* per-frame counters */
    unsigned frame, draws, draws_up, draws_ib, prims;
    unsigned vbs, begins, ends, rt_changes;
    uint32_t rt, ds;
    unsigned tex_used[16];
    int      ntex_used;
    /* current state */
    uint32_t tex0, fvf, vs, ps, vdecl, stream0, stream0_stride, indices;
    uint32_t rs[256];            /* last SetRenderState value per state */
    unsigned char rs_seen[256];
    /* signatures seen (kind, prims, stride, fvf, tex class, caller) */
} probe_t;

static probe_t P;

/* textures created so far, keyed by pointer */
typedef struct { uint32_t tex; uint32_t w, h, fmt, pool; unsigned level; int sampled; } texrec_t;
#define MAX_TEX 2048
static texrec_t g_tex[MAX_TEX];
static int g_ntex;

/* ------------------------------------------------------- texture contents
 * A texture that was created blank and filled by guest code (a glyph atlas is
 * the interesting case) is created with IDirect3DDevice9::CreateTexture rather
 * than D3DX from a file. Reading a few bytes back is the difference between
 * "the draw happens with the right quad but the atlas is empty" and "the atlas
 * has glyphs and something else eats them". Locking a MANAGED texture is safe
 * from here: D3D9 holds no callback into the guest. */
typedef struct { uint32_t fmt, type, usage, pool, msaa, msq, w, h; } surf_desc_t;

/* defined below, used here first */
static int readable(uint32_t va, size_t n);
static int in_range(uint32_t va, uint32_t lo, uint32_t hi);
static uint32_t rd32(uint32_t va);

static int sample_texture(uint32_t tex, char *out, size_t cap) {
    uint32_t vt = rd32(tex), getdesc, lock, unlock;
    surf_desc_t d;
    struct { int Pitch; void *pBits; } lr;
    unsigned char *p;
    size_t i, n, nz = 0;
    unsigned sum = 0;

    if (!readable(tex, 4) || !vt) return 0;
    getdesc = rd32(vt + 4u * 17);
    lock    = rd32(vt + 4u * 19);
    unlock  = rd32(vt + 4u * 20);
    if (!in_range(getdesc, P.d3d9_lo, P.d3d9_hi) ||
        !in_range(lock, P.d3d9_lo, P.d3d9_hi) ||
        !in_range(unlock, P.d3d9_lo, P.d3d9_hi))
        return 0;
    memset(&d, 0, sizeof d);
    if (((long(__stdcall *)(void *, void *))getdesc)((void *)(uintptr_t)tex, &d) != 0) return 0;
    if (!d.w || !d.h || d.w > 4096 || d.h > 4096) return 0;
    lr.Pitch = 0; lr.pBits = NULL;
    /* D3DLOCK_READONLY = 0x10 */
    if (((long(__stdcall *)(void *, unsigned, void *, const void *, unsigned))lock)
            ((void *)(uintptr_t)tex, 0, &lr, NULL, 0x10) != 0) {
        _snprintf(out, cap, "%ux%u fmt=0x%X pool=%u LOCK FAILED", d.w, d.h, d.fmt, d.pool);
        return 1;
    }
    p = (unsigned char *)lr.pBits;
    n = (size_t)lr.Pitch * d.h;
    if (n > (2u << 20)) n = 2u << 20;     /* bound the scan: 2 MB */
    if (p && readable((uint32_t)(uintptr_t)p, n < 4096 ? n : 4096)) {
        for (i = 0; i < n; i++) { nz += p[i] != 0; sum += p[i]; }
        _snprintf(out, cap,
                  "%ux%u fmt=0x%X pool=%u pitch=%d nonzero=%u/%u sum=%08X"
                  " head=%02X %02X %02X %02X %02X %02X %02X %02X",
                  d.w, d.h, d.fmt, d.pool, lr.Pitch, (unsigned)nz, (unsigned)n,
                  sum & 0xFFFFFFFFu,
                  p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    } else {
        _snprintf(out, cap, "%ux%u fmt=0x%X pool=%u pitch=%d bits unreadable",
                  d.w, d.h, d.fmt, d.pool, lr.Pitch);
    }
    ((long(__stdcall *)(void *, unsigned))unlock)((void *)(uintptr_t)tex, 0);
    return 1;
}

/* one line per distinct draw shape */
typedef struct {
    int      used;
    int      kind;          /* 1 DrawPrimitive, 2 DrawIndexed, 3 UP, 4 IndexedUP */
    uint32_t prims, stride, fvf, tex;
    uint32_t caller;
    unsigned hits;
} sig_t;
#define MAX_SIG 96
static sig_t g_sig[MAX_SIG];
static int g_nsig;

static native32_post_hook_t g_other_post;
static native32_pre_hook_t  g_other_pre;

/* call census: every bridged call, and the ones that land in d3d9 but are not
 * a method of the captured device (a wrong vtable slot or an object we never
 * captured would show up here). */
static volatile long g_calls, g_d3d_calls;
static uint32_t g_unmatched[48];
static int      g_nunmatched;
static int verbosity(void);

static void note_unmatched(uint32_t target, const uint32_t *a) {
    int i;
    for (i = 0; i < g_nunmatched; i++) if (g_unmatched[i] == target) return;
    if (g_nunmatched >= (int)(sizeof g_unmatched / sizeof g_unmatched[0])) return;
    g_unmatched[g_nunmatched++] = target;
    if (verbosity() >= 2)
        fprintf(stderr, "[draw] unclaimed d3d9 call 0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X dev=%d\n",
                target, a[0], a[1], a[2], a[3], P.have_dev && a[0] == P.dev);
}

static int verbosity(void) {
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("KORRA_DRAWPATH");
        v = s ? atoi(s) : 0;
        if (v < 0) v = 0;
        if (v > 3) v = 3;
    }
    return v;
}

/* ------------------------------------------------------------ module range */

static void module_range(const char *dll, uint32_t *lo, uint32_t *hi) {
    HMODULE h = GetModuleHandleA(dll);
    const unsigned char *b;
    const uint32_t *lfanew;
    uint32_t size;
    *lo = *hi = 0;
    if (!h) return;
    b = (const unsigned char *)h;
    if (b[0] != 'M' || b[1] != 'Z') return;
    lfanew = (const uint32_t *)(uintptr_t)(b + *(const uint32_t *)(uintptr_t)(b + 0x3C));
    /* OptionalHeader.SizeOfImage: PE +4 signature, COFF header 20 bytes, then
     * optional header offset 0x38; as dwords from the signature: 6 + 14. */
    size = lfanew[0x14];
    *lo = (uint32_t)(uintptr_t)b;
    *hi = *lo + size;
}

static int in_range(uint32_t va, uint32_t lo, uint32_t hi) {
    return lo && va >= lo && va < hi;
}

/* A pointer we may read n bytes from without faulting. */
static int readable(uint32_t va, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!va || (va & 3) || n > 0x10000) return 0;
    if (!VirtualQuery((const void *)(uintptr_t)va, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (size_t)((const char *)mbi.BaseAddress + mbi.RegionSize -
                    (const char *)(uintptr_t)va) >= n;
}

static uint32_t rd32(uint32_t va) {
    return readable(va, 4) ? *(const uint32_t *)(uintptr_t)va : 0;
}

/* --------------------------------------------------------- texture records */

static void note_texture(uint32_t tex, uint32_t w, uint32_t h, uint32_t fmt,
                         uint32_t pool, unsigned level, const char *origin) {
    if (!tex || g_ntex >= MAX_TEX) return;
    for (int i = 0; i < g_ntex; i++) if (g_tex[i].tex == tex) return;
    g_tex[g_ntex].tex = tex;
    g_tex[g_ntex].w = w; g_tex[g_ntex].h = h;
    g_tex[g_ntex].fmt = fmt; g_tex[g_ntex].pool = pool;
    g_tex[g_ntex].level = level;
    g_ntex++;
    if (verbosity() >= 1)
        fprintf(stderr, "[draw] tex 0x%08X %ux%u fmt=0x%X pool=%u (%s)\n",
                tex, w, h, fmt, pool, origin);
}

static const texrec_t *find_texture(uint32_t tex) {
    for (int i = 0; i < g_ntex; i++) if (g_tex[i].tex == tex) return &g_tex[i];
    return NULL;
}

/* ------------------------------------------------------------- COM objects
 * Every bridged COM call carries its `this` in args[0]. An object whose vtable
 * lives in d3d9.dll is a D3D9 interface; one whose vtable fills 100+ slots is
 * an IDirect3DDevice9 (that interface has 119 methods). Identity is kept per
 * object because the engine also calls surfaces, swapchains, vertex buffers and
 * shaders - all in d3d9.dll - and one global "the device" guess binds to
 * whichever interface happens to be called first (measured: it bound to the
 * swapchain-like object and every device call went unmatched). */
#define MAX_OBJ 64
typedef struct {
    uint32_t obj, vtbl;
    int      nslots;
    int      is_device;
    uint32_t m[128];
} objrec_t;
static objrec_t g_obj[MAX_OBJ];
static int g_nobj;
static int g_devices;

/* IDirect3DDevice9. Asking the object beats guessing from the vtable: every
 * d3d9 COM vtable is a contiguous run of d3d9 code pointers, so "how many slots
 * are d3d9 code" scores 128 for textures and swapchains too (measured: the
 * first version classified five different interfaces as devices).
 *
 * The GUID comes from the SDK's d3d9.h, not from memory: the hand-typed one was
 * wrong in the last four bytes (A43B0D92B9E4 vs A43B0D82B9EB) and every QI
 * returned E_NOINTERFACE, which read as "the engine never calls a device". */
/* Copied from the SDK's d3d9.h (10.0.26100): DEFINE_GUID only declares the
 * symbol, and linking guid.lib for two constants is not worth it. */
static const GUID IID_D3D9_LOCAL =
    { 0x81bdcbca, 0x64d4, 0x426d, { 0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c } };
static const GUID IID_D3DDEV9_LOCAL =
    { 0xd0223b96, 0xbf7a, 0x43fd, { 0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb } };

static int obj_is_device(uint32_t candidate, uint32_t vtbl) {
    uint32_t qi = rd32(vtbl);
    void *out = NULL;
    long hr;
    static int logged;
    int verbose = verbosity() >= 2 && logged < 10;
    if (!in_range(qi, P.d3d9_lo, P.d3d9_hi)) {
        if (verbose) { logged++; fprintf(stderr, "[draw] qi 0x%08X: vtable[0]=0x%08X outside d3d9\n", candidate, qi); }
        return 0;
    }
    hr = ((long(__stdcall *)(void *, const GUID *, void **))qi)
            ((void *)(uintptr_t)candidate, &IID_D3DDEV9_LOCAL, &out);
    if (verbose) {
        logged++;
        fprintf(stderr, "[draw] qi 0x%08X (qi=0x%08X) -> hr=0x%08lX out=0x%08X\n",
                candidate, qi, hr, (uint32_t)(uintptr_t)out);
        fflush(stderr);
    }
    if (hr != 0 || !out) return 0;
    if (out != (void *)(uintptr_t)candidate) {
        uint32_t ovt = rd32((uint32_t)(uintptr_t)out);
        uint32_t rel = rd32(ovt + 8);
        if (in_range(rel, P.d3d9_lo, P.d3d9_hi))
            ((unsigned long(__stdcall *)(void *))rel)(out);
    } else {
        uint32_t rel = rd32(vtbl + 8);
        if (in_range(rel, P.d3d9_lo, P.d3d9_hi))
            ((unsigned long(__stdcall *)(void *))rel)(out);
    }
    return 1;
}

static objrec_t *obj_for(uint32_t candidate) {
    int i, nslots = 0;
    uint32_t vtbl;
    objrec_t *o;
    if (!candidate || (candidate & 3) || !readable(candidate, 4)) return NULL;
    for (i = 0; i < g_nobj; i++) if (g_obj[i].obj == candidate) return &g_obj[i];
    if (g_nobj >= MAX_OBJ) return NULL;
    vtbl = rd32(candidate);
    /* The vtable itself does not have to live in d3d9.dll: this d3d9 (and the
     * engine's Hw wrapper around it) allocates per-object thunk tables on the
     * heap whose *entries* are d3d9 code. Requiring the table to be inside the
     * module rejected the device the factory returned. */
    if (!vtbl || !readable(vtbl, 0x40)) return NULL;
    {
        int k, score = 0;
        for (k = 0; k < 16; k++)
            if (in_range(rd32(vtbl + 4u * (uint32_t)k), P.d3d9_lo, P.d3d9_hi)) score++;
        if (score < 8) return NULL;
    }
    o = &g_obj[g_nobj];
    memset(o, 0, sizeof *o);
    o->obj = candidate;
    o->vtbl = vtbl;
    o->is_device = obj_is_device(candidate, vtbl);
    g_nobj++;
    if (!o->is_device) return o;          /* cached negative: not worth a map */
    for (i = 0; i < 128; i++) {
        uint32_t fn = rd32(vtbl + 4u * (uint32_t)i);
        if (in_range(fn, P.d3d9_lo, P.d3d9_hi)) { o->m[i] = fn; nslots++; }
    }
    o->nslots = nslots;
    g_devices++;
    P.dev = candidate;
    P.have_dev = 1;
    memcpy(P.m, o->m, sizeof P.m);     /* dispatch by target address only */
    if (verbosity() >= 1) {
        fprintf(stderr, "[draw] DEVICE 0x%08X vtbl 0x%08X (Present=%08X EndScene=%08X "
                        "SetRenderState=%08X SetTexture=%08X DrawPrimitiveUP=%08X "
                        "SetFVF=%08X SetVS=%08X SetPS=%08X)\n",
                candidate, vtbl, o->m[17], o->m[42], o->m[57], o->m[65],
                o->m[83], o->m[89], o->m[92], o->m[94]);
        fflush(stderr);
    }
    return o;
}

/* ------------------------------------------------------------- primitives */

static unsigned vertex_count(uint32_t primtype, uint32_t prims) {
    switch (primtype) {
    case 1: return prims * 2;          /* D3DPT_LINELIST */
    case 2: return prims + 1;          /* D3DPT_LINESTRIP */
    case 3: return prims;              /* D3DPT_POINTLIST */
    case 4: return prims * 3;          /* D3DPT_TRIANGLELIST */
    case 5: case 6: return prims + 2;  /* TRIANGLESTRIP / TRIANGLEFAN */
    default: return 0;
    }
}

/* Bounding box over the POSITION/TEXCOORD stream of an UP draw. Cheap, and it
 * is the difference between "degenerate quad" and "never submitted". */
static void vertex_bbox(const unsigned char *v, unsigned n, unsigned stride,
                        unsigned pos_floats, unsigned pos_off, unsigned no_dwords,
                        float *out, unsigned *nan, uint32_t *first8) {
    unsigned i, k;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    *nan = 0;
    if (first8 && n && readable((uint32_t)(uintptr_t)v, (sizeof(uint32_t) * 8 < stride ? sizeof(uint32_t) * 8 : stride)))
        memcpy(first8, v, sizeof(uint32_t) * 8 < stride ? sizeof(uint32_t) * 8 : stride);
    for (i = 0; i < n; i++) {
        const float *f = (const float *)(const void *)(v + i * stride + pos_off);
        if (!readable((uint32_t)(uintptr_t)f, sizeof(float) * (pos_floats ? pos_floats : 4))) break;
        for (k = 0; k < 4; k++) {
            float x = k < pos_floats ? f[k] : 0.0f;
            if (x != x) { (*nan)++; continue; }
            if (i == 0 && k == 0) { out[0] = out[1] = x; }
            if (x < out[0]) out[0] = x;
            if (x > out[1]) out[1] = x;
            if (k == 1) {
                if (i == 0) out[2] = out[3] = x;
                if (x < out[2]) out[2] = x;
                if (x > out[3]) out[3] = x;
            }
        }
    }
    (void)no_dwords;
}

/* --------------------------------------------------------------- reporting */

static void report_draw(int kind, unsigned type, unsigned prims, unsigned stride,
                        uint32_t pverts, uint32_t caller) {
    uint32_t tex = P.tex0;
    const texrec_t *tr;
    int i, slot = -1;
    static int dump_frame = -2;
    static int dumped;
    static int dump_from = -2;

    if (dump_frame == -2) {
        const char *s = getenv("KORRA_DRAWPATH_FRAME");
        const char *f = getenv("KORRA_DRAWPATH_FROM");
        dump_frame = s ? atoi(s) : -1;
        dump_from = f ? atoi(f) : -1;
    }
    if ((dump_from >= 0 ? (int)P.frame >= dump_from : (dump_frame >= 0 && (int)P.frame == dump_frame))
        && dumped < 400) {
        const texrec_t *t = find_texture(tex);
        dumped++;
        fprintf(stderr, "[draw] dump f%u %s type=%u prims=%u stride=%u tex=0x%08X %s "
                        "vs=%08X ps=%08X rt=%08X caller=sub_%08X\n",
                P.frame, kind == 1 ? "DP" : kind == 2 ? "DIP" : kind == 3 ? "DPUP" : "DIPUP",
                type, prims, stride, tex,
                t ? "known" : "?", P.vs, P.ps, P.rt, caller);
    }

    /* Deduplicate on the shape of the call, not on the vertex data. */
    for (i = 0; i < g_nsig; i++) {
        sig_t *s = &g_sig[i];
        if (s->kind == kind && s->prims == prims && s->tex == tex &&
            s->fvf == P.fvf && s->caller == caller) { slot = i; break; }
    }
    if (slot < 0 && g_nsig < MAX_SIG) {
        slot = g_nsig++;
        g_sig[slot].used = 1;
        g_sig[slot].kind = kind;
        g_sig[slot].prims = prims;
        g_sig[slot].tex = tex;
        g_sig[slot].fvf = P.fvf;
        g_sig[slot].caller = caller;
    }

    P.draws++;
    P.prims += prims;
    if (kind == 3 || kind == 4) P.draws_up++;
    else P.draws_ib++;
    {
        int known = 0;
        for (i = 0; i < P.ntex_used; i++) if (P.tex_used[i] == tex) { known = 1; break; }
        if (!known && P.ntex_used < 16) P.tex_used[P.ntex_used++] = tex;
    }

    /* Once per texture, read its pixels back. Off by default: LockRect from
     * inside the bridge hook is the one operation that has killed a run, so it
     * needs its own switch (KORRA_DRAWPATH_TEX=1) while the census runs. */
    {
        static int tex_on = -1;
        if (tex_on < 0) tex_on = getenv("KORRA_DRAWPATH_TEX") != NULL;
        if (tex_on && tex) {
            for (i = 0; i < g_ntex; i++) {
                if (g_tex[i].tex == tex && !g_tex[i].sampled) {
                    char buf[256];
                    g_tex[i].sampled = 1;
                    if (sample_texture(tex, buf, sizeof buf))
                        fprintf(stderr, "[draw] contents tex=0x%08X %s (draw from sub_%08X)\n",
                                tex, buf, caller);
                    break;
                }
            }
        }
    }

    if (slot < 0 || verbosity() < 1) return;
    if (g_sig[slot].hits >= (verbosity() >= 3 ? 8u : 3u)) { g_sig[slot].hits++; return; }
    g_sig[slot].hits++;

    tr = find_texture(tex);
    {
        const char *name = kind == 1 ? "DrawPrimitive"
                         : kind == 2 ? "DrawIndexedPrimitive"
                         : kind == 3 ? "DrawPrimitiveUP"
                                     : "DrawIndexedPrimitiveUP";
        fprintf(stderr, "[draw] sig#%d #%u %s type=%u prims=%u", slot,
                g_sig[slot].hits, name, type, prims);
        if (stride) fprintf(stderr, " stride=%u", stride);
        fprintf(stderr, " fvf=0x%X vs=0x%08X ps=0x%08X",
                P.fvf, P.vs, P.ps);
        if (tex) {
            if (tr) fprintf(stderr, " tex=0x%08X %ux%u fmt=0x%X",
                            tex, tr->w, tr->h, tr->fmt);
            else    fprintf(stderr, " tex=0x%08X (device-created, unknown)", tex);
        } else {
            fprintf(stderr, " tex=none");
        }
        fprintf(stderr, " caller=sub_%08X\n", caller);
    }

    /* Vertex data: only the UP forms carry it in an argument. */
    if ((kind == 3 || kind == 4) && pverts) {
        unsigned n = vertex_count(type, prims);
        if (stride && n && readable(pverts, (size_t)stride * (n < 64 ? n : 64))) {
            unsigned pos = (P.fvf & 0x004) ? 4 : (P.fvf & 0x002) ? 3 : 0;
            float bb[4]; unsigned nan = 0; uint32_t first8[8];
            vertex_bbox((const unsigned char *)(uintptr_t)pverts, n < 64 ? n : 64,
                        stride, pos, 0, 0, bb, &nan, first8);
            fprintf(stderr, "[draw]   verts=%u x=[%.2f..%.2f] y=[%.2f..%.2f]"
                            " nan=%u v0=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                    n, bb[0], bb[1], bb[2], bb[3], nan,
                    first8[0], first8[1], first8[2], first8[3],
                    first8[4], first8[5], first8[6], first8[7]);
        } else {
            fprintf(stderr, "[draw]   vertex data 0x%08X not readable (stride %u n %u)\n",
                    pverts, stride, n);
        }
    }
}

static void report_frame(void) {
    if (verbosity() < 1) return;
    if ((P.frame % 60) != 0) return;
    fprintf(stderr, "[draw] frame %u draws=%u up=%u vb=%u prims=%u texs=%d"
                    " rt=%08X/%u begins=%u ends=%u"
                    " fvf=0x%X vs=0x%08X ps=0x%08X"
                    " abl=%u src=%u dst=%u zen=%u atest=%u afunc=%u\n",
            P.frame, P.draws, P.draws_up, P.draws_ib, P.prims, P.ntex_used,
            P.rt, P.rt_changes, P.begins, P.ends,
            P.fvf, P.vs, P.ps,
            P.rs_seen[27] ? P.rs[27] : 0,          /* ALPHABLENDENABLE */
            P.rs_seen[19] ? P.rs[19] : 0,          /* SRCBLEND */
            P.rs_seen[20] ? P.rs[20] : 0,          /* DESTBLEND */
            P.rs_seen[7] ? P.rs[7] : 0,            /* ZENABLE */
            P.rs_seen[15] ? P.rs[15] : 0,          /* ALPHATESTENABLE */
            P.rs_seen[24] ? P.rs[24] : 0);         /* ALPHAFUNC */
}

/* ------------------------------------------------- windowed device creation
 * The engine asks for an exclusive-fullscreen device when the machine has a
 * display mode it likes. Two consequences: the desktop is hijacked (a capture
 * cannot be relied on), and a second instance of the game cannot create a
 * device at all, which makes a parallel debug session impossible.
 *
 * KORRA_WINDOWED is meant to rewrite the presentation parameters, but the hook
 * that does it (quirk_probe.c) looks the call up by name, and the runtime has
 * no names for COM methods - only bridged *imports* carry one. So it never
 * fires. Here the factory object's identity comes from the vtable instead: any
 * call on IDirect3D9 whose 7th slot is a plausible D3DPRESENT_PARAMETERS with a
 * real window handle is CreateDevice (or CreateDeviceEx, same layout), and the
 * struct is rewritten in place before the call.
 *
 * D3DPRESENT_PARAMETERS: Windowed +0x20, FullScreen_RefreshRateInHz +0x30,
 * PresentationInterval +0x34; size 0x38 (the engine memsets exactly that). */
#define D3DPP_WINDOWED   0x20u
#define D3DPP_REFRESH_HZ 0x30u
#define D3DPP_INTERVAL   0x34u

static int plausible_present_params(uint32_t p, uint32_t hfocus, uint32_t *w,
                                    uint32_t *h, uint32_t *windowed) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || (p & 3)) return 0;
    if (!VirtualQuery((const void *)(uintptr_t)p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    if ((size_t)((const char *)mbi.BaseAddress + mbi.RegionSize -
                 (const char *)(uintptr_t)p) < 0x38) return 0;
    *w = rd32(p);
    *h = rd32(p + 4);
    *windowed = rd32(p + D3DPP_WINDOWED);
    if (*windowed > 1) return 0;
    if (rd32(p + 0x0C) > 8) return 0;                    /* BackBufferCount */
    if (rd32(p + 0x10) > 16) return 0;                   /* MultiSampleType */
    if (rd32(p + 0x18) > 8) return 0;                    /* SwapEffect */
    /* hDeviceWindow may legitimately be NULL on a fullscreen device (D3D then
     * uses CreateDevice's hFocusWindow), so either handle must be a window. */
    if (!IsWindow((HWND)(uintptr_t)rd32(p + 0x1C)) &&
        !IsWindow((HWND)(uintptr_t)hfocus))
        return 0;
    return 1;
}

static void probe_pre(uint32_t target, uint32_t *a) {
    static int on = -1;
    static int done;
    if (on < 0) on = getenv("KORRA_WINDOWED") != NULL;
    if (g_other_pre && g_other_pre != probe_pre) g_other_pre(target, a);
    if (!on || done || !P.d3d9_lo) return;
    if (a[0] != P.d3d) return;
    {
        static int tries;
        uint32_t w, h, windowed;
        int ok = plausible_present_params(a[5], a[3], &w, &h, &windowed);
        if (verbosity() >= 2 && tries < 8) {
            tries++;
            fprintf(stderr, "[draw] pre: factory call 0x%08X a3=0x%08X a5=0x%08X params=%d\n",
                    target, a[3], a[5], ok);
            fflush(stderr);
        }
        if (!ok) return;
        if (windowed) { done = 1; return; }
        *(uint32_t *)(uintptr_t)(a[5] + D3DPP_WINDOWED) = 1;
        *(uint32_t *)(uintptr_t)(a[5] + D3DPP_REFRESH_HZ) = 0;
        *(uint32_t *)(uintptr_t)(a[5] + D3DPP_INTERVAL) = D3DPRESENT_INTERVAL_IMMEDIATE;
        done = 1;
        fprintf(stderr, "[draw] device creation forced windowed (was %ux%u), "
                        "caller=sub_%08X\n", w, h, g_cur_func);
        fflush(stderr);
    }
}

/* -------------------------------------------------------------- the hook */

static void probe_post(uint32_t target, const uint32_t *a, uint32_t ret) {
    if (g_other_post && g_other_post != probe_post) g_other_post(target, a, ret);
    if (!P.on) return;

    /* d3d9.dll is loaded by native32_bind, after main() starts, so the module
     * range cannot be resolved at init time. */
    InterlockedIncrement(&g_calls);
    if (!P.d3d9_lo) {
        module_range("d3d9.dll", &P.d3d9_lo, &P.d3d9_hi);
        module_range("d3dx9_43.dll", &P.d3dx_lo, &P.d3dx_hi);
        if (P.d3d9_lo && verbosity() >= 1) {
            fprintf(stderr, "[draw] census on: verbosity %d, d3d9.dll %08X-%08X\n",
                    P.on, P.d3d9_lo, P.d3d9_hi);
            fflush(stderr);
        }
        if (!P.d3d9_lo) return;   /* not loaded yet: nothing to observe */
    }

    /* Imports we do know by name. */
    {
        const char *n = native32_name(target);
        if (!P.d3d && n && strstr(n, "d3d9.dll!Direct3DCreate9")) {
            P.d3d = ret;
            if (verbosity() >= 1) {
                static const GUID iid_unk =
                    { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
                uint32_t qi = rd32(rd32(ret));
                void *o = NULL, *u = NULL;
                long hr = -1, hru = -1;
                if (in_range(qi, P.d3d9_lo, P.d3d9_hi)) {
                    hr = ((long(__stdcall *)(void *, const GUID *, void **))qi)
                            ((void *)(uintptr_t)ret, &IID_D3D9_LOCAL, &o);
                    hru = ((long(__stdcall *)(void *, const GUID *, void **))qi)
                            ((void *)(uintptr_t)ret, &iid_unk, &u);
                }
                fprintf(stderr, "[draw] IDirect3D9* 0x%08X vtable 0x%08X"
                                " (QI self-test d3d9=0x%08lX/%08X unknown=0x%08lX/%08X)\n",
                        ret, rd32(ret), hr, (uint32_t)(uintptr_t)o,
                        hru, (uint32_t)(uintptr_t)u);
                fflush(stderr);
            }
            return;
        }
        /* D3DXCreate*: the four imports are bound by name, so each texture can
         * be identified later from the draw's texture pointer. */
        if (n && strstr(n, "d3dx9_43.dll!")) {
            if (strstr(n, "D3DXCreateTextureFromFileInMemoryEx")) {
                /* (dev, data, size, W, H, mips, usage, fmt, pool, filter,
                 *  mipfilter, colorkey, srcinfo, palette, ppTexture) */
                note_texture(rd32(a[14]), a[3], a[4], a[7], a[8], a[5],
                             ret == 0 ? "D3DXFromFileInMemory" : "D3DXFromFileInMemory(FAILED)");
            } else if (strstr(n, "D3DXCreateTexture")) {
                /* (dev, W, H, mips, usage, fmt, pool, ppTexture) */
                note_texture(rd32(a[7]), a[1], a[2], a[5], a[6], a[3],
                             ret == 0 ? "D3DXCreateTexture" : "D3DXCreateTexture(FAILED)");
            }
            return;
        }
    }

    if (!P.have_dev) {
        if (P.d3d && a[0] == P.d3d) {
            /* The factory's own calls, for reference: which call is
             * CreateDevice is decided from the object the call returns, not
             * from the slot number. */
            static uint32_t seen[16];
            static int nseen;
            int i, known = 0;
            for (i = 0; i < nseen; i++) if (seen[i] == target) known = 1;
            if (!known && nseen < 16) {
                seen[nseen++] = target;
                fprintf(stderr, "[draw] factory 0x%08X args %08X %08X %08X %08X %08X %08X %08X %08X ret=%08X\n",
                        target, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], ret);
                fflush(stderr);
            }
            /* CreateDevice is recognisable by its argument shape:
             * (adapter, DeviceType in {HAL,REF}, hFocusWindow,
             *  BehaviorFlags containing MULTITHREADED, ppPresentParams,
             *  ppDevice)  and it is the factory call that returns S_OK with an
             *  interface written to args[6]. */
            if (!P.have_dev && ret == 0 && (a[4] == 0x44u || a[4] == 0x24u) &&
                (a[2] == 1u || a[2] == 2u) && readable(a[5], 0x38) && readable(a[6], 4)) {
                uint32_t dev = rd32(a[6]);
                static int tries;
                if (tries++ < 4)
                    fprintf(stderr, "[draw] CreateDevice? ppdev=0x%08X -> 0x%08X params{%08X %08X %08X %08X %08X %08X %08X %08X}\n",
                            a[6], dev, rd32(a[5]), rd32(a[5] + 4), rd32(a[5] + 8),
                            rd32(a[5] + 12), rd32(a[5] + 16), rd32(a[5] + 20),
                            rd32(a[5] + 24), rd32(a[5] + 28));
                if (dev) {
                    objrec_t *d = obj_for(dev);
                    if (P.have_dev) {
                        if (verbosity() >= 1)
                            fprintf(stderr, "[draw] device 0x%08X confirmed by QueryInterface\n",
                                    dev);
                        return;
                    }
                    if (tries <= 4) {
                        fprintf(stderr, "[draw] candidate 0x%08X vtbl 0x%08X QI refused\n",
                                dev, rd32(dev));
                        fflush(stderr);
                    }
                }
            }
        }
    }

    {
        /* Dispatch by method address, not by object: the device pointer the
         * factory returns is authoritative, and every method call lands on one
         * of its vtable entries whatever `this` the engine passes. */
        int mi = -1, k;
        if (P.have_dev)
            for (k = 0; k < 128; k++) if (P.m[k] == target) { mi = k; break; }
        if (mi >= 0) {
            InterlockedIncrement(&g_d3d_calls);
            switch (mi) {
            case D3DDEV_PRESENT:
                report_frame();
                P.frame++;
                P.draws = P.draws_up = P.draws_ib = P.prims = 0;
                P.ntex_used = 0;
                P.begins = P.ends = P.rt_changes = 0;
                break;
            case D3DDEV_SETRENDERSTATE:
                if (a[1] < 256) { P.rs[a[1]] = a[2]; P.rs_seen[a[1]] = 1; }
                break;
            case D3DDEV_SETTEXTURE:
                if (a[1] == 0) {
                    if (a[2] != P.tex0 && verbosity() >= 2) {
                        const texrec_t *tr = find_texture(a[2]);
                        fprintf(stderr, "[draw] tex0 0x%08X -> 0x%08X%s%ux%u caller=sub_%08X\n",
                                P.tex0, a[2], tr ? " dims=" : "",
                                tr ? tr->w : 0, tr ? tr->h : 0, g_cur_func);
                    }
                    P.tex0 = a[2];
                }
                break;
            case D3DDEV_SETFVF:                P.fvf = a[1]; break;
            case D3DDEV_SETVERTEXSHADER:       P.vs = a[1]; break;
            case D3DDEV_SETPIXELSHADER:        P.ps = a[1]; break;
            case D3DDEV_SETVERTEXDECLARATION:  P.vdecl = a[1]; break;
            case D3DDEV_SETSTREAMSOURCE:       if (a[1] == 0) { P.stream0 = a[2]; P.stream0_stride = a[3]; } break;
            case D3DDEV_SETINDICES:            P.indices = a[1]; break;
            case D3DDEV_DRAWPRIMITIVE:         report_draw(1, a[1], a[3], 0, 0, g_cur_func); break;
            case D3DDEV_DRAWINDEXEDPRIMITIVE:  report_draw(2, a[1], a[6], 0, 0, g_cur_func); break;
            case D3DDEV_DRAWPRIMITIVEUP:       report_draw(3, a[1], a[2], a[4], a[3], g_cur_func); break;
            case D3DDEV_DRAWINDEXEDPRIMITIVEUP: report_draw(4, a[1], a[4], a[8], a[7], g_cur_func); break;
            case D3DDEV_CREATETEXTURE:
                /* (this, Width, Height, Levels, Usage, Format, Pool, ppTexture) */
                note_texture(rd32(a[7]), a[1], a[2], a[5], a[6], a[3], "device CreateTexture");
                break;
            case D3DDEV_CREATEVERTEXBUFFER:
                P.vbs++;
                break;
            case D3DDEV_CREATERENDERTARGET:
                /* (this, Width, Height, Format, MultiSample, Quality, Lockable,
                 *  ppSurface) */
                if (verbosity() >= 1)
                    fprintf(stderr, "[draw] render target %ux%u fmt=0x%X -> 0x%08X\n",
                            a[1], a[2], a[3], rd32(a[7]));
                break;
            case D3DDEV_CREATEDEPTHSTENCIL:
                if (verbosity() >= 1)
                    fprintf(stderr, "[draw] depth surface %ux%u fmt=0x%X\n", a[1], a[2], a[3]);
                break;
            case D3DDEV_SETRENDERTARGET:
                if (a[1] == 0) {
                    P.rt = a[2];
                    P.rt_changes++;
                }
                break;
            case D3DDEV_SETDEPTHSTENCIL:
                P.ds = a[1];
                break;
            case D3DDEV_BEGINSCENE:
                P.begins++;
                break;
            case D3DDEV_ENDSCENE:
                P.ends++;
                break;
            default:
                break;
            }
            return;
        }
        if (in_range(target, P.d3d9_lo, P.d3d9_hi)) {
            InterlockedIncrement(&g_d3d_calls);
            note_unmatched(target, a);
        }
        return;
    }
}

/* --------------------------------------------------------------- install */

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(500);
        if (P.on && native32_post_hook != probe_post) {
            if (native32_post_hook) g_other_post = native32_post_hook;
            native32_post_hook = probe_post;
        }
        if (getenv("KORRA_WINDOWED") && native32_pre_hook != probe_pre) {
            if (native32_pre_hook) g_other_pre = native32_pre_hook;
            native32_pre_hook = probe_pre;
        }
        if (P.on) {
            static unsigned tick;
            if (++tick % 10 == 0) {      /* every ~5 s */
                fprintf(stderr, "[draw] alive frames=%u calls=%ld d3d=%ld draws=%u unmatched=%d\n",
                        P.frame, g_calls, g_d3d_calls, P.draws, g_nunmatched);
                fflush(stderr);
            }
        }
    }
    return 0;
}

static void probe_start(void) {
    P.on = verbosity();
    if (P.on) {
        if (native32_post_hook && native32_post_hook != probe_post)
            g_other_post = native32_post_hook;
        native32_post_hook = probe_post;
    }
    if (getenv("KORRA_WINDOWED")) {
        if (native32_pre_hook && native32_pre_hook != probe_pre)
            g_other_pre = native32_pre_hook;
        native32_pre_hook = probe_pre;
    }
    if (P.on || getenv("KORRA_WINDOWED"))
        CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    /* No log line here: this runs at CRT init, before main() has pointed stderr
     * at the per-run log file. The first hook call reports the settings. */
}

/* Runs at CRT init, before main(): main() and the shared host files belong to
 * another work stream this round, and src\host\*.c is globbed into the build,
 * so this file installs itself. */
#pragma section(".CRT$XCU", long, read)
__declspec(allocate(".CRT$XCU")) static void (*const g_probe_init)(void) = probe_start;
