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
#include "native32.h"

/* ---------------------------------------------------------------- D3D9 vtable
 * IDirect3DDevice9 slot order (d3d9.h). Only the slots the census needs are
 * named; the rest are resolved but ignored. */
#define D3D9_CREATEDEVICE           13
#define D3DDEV_PRESENT              17
#define D3DDEV_CREATETEXTURE        23
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
    size = lfanew[0x18];                     /* OptionalHeader.SizeOfImage */
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

/* ---------------------------------------------------------- method capture */

/* Number of the first 48 vtable slots that point into d3d9.dll: a real COM
 * interface of the device family fills them, a random object does not. */
static int vtable_score(uint32_t vtbl) {
    int i, n = 0;
    for (i = 0; i < 48; i++)
        if (in_range(rd32(vtbl + 4u * (uint32_t)i), P.d3d9_lo, P.d3d9_hi)) n++;
    return n;
}

static void capture_methods(uint32_t obj) {
    uint32_t vtbl = rd32(obj);
    if (!vtbl) return;
    for (int i = 0; i < 128; i++) {
        uint32_t fn = rd32(vtbl + 4u * (uint32_t)i);
        if (!in_range(fn, P.d3d9_lo, P.d3d9_hi)) { P.m[i] = 0; continue; }
        P.m[i] = fn;
    }
    P.have_dev = 1;
    if (verbosity() >= 1)
        fprintf(stderr, "[draw] device 0x%08X vtable 0x%08X captured "
                        "(Present=0x%08X DrawPrimitiveUP=0x%08X)\n",
                obj, vtbl, P.m[D3DDEV_PRESENT], P.m[D3DDEV_DRAWPRIMITIVEUP]);
}

static int method_index(uint32_t target) {
    int i;
    if (!P.have_dev) return -1;
    for (i = 0; i < 128; i++) if (P.m[i] == target) return i;
    return -1;
}

/* A COM object whose vtable is inside d3d9.dll is one of ours. Called with
 * args[0] (the `this` slot of every COM method) so the device is captured even
 * if CreateDevice itself was missed. The IDirect3D9 factory has its own vtable
 * and is not a device, so it is excluded. */
static void sniff_device(uint32_t candidate) {
    uint32_t vtbl, d3d_vtbl;
    if (P.have_dev || !candidate || (candidate & 3)) return;
    if (!readable(candidate, 4)) return;
    vtbl = rd32(candidate);
    if (!in_range(vtbl, P.d3d9_lo, P.d3d9_hi)) return;
    d3d_vtbl = P.d3d ? rd32(P.d3d) : 0;
    if (vtbl == d3d_vtbl) return;
    if (vtable_score(vtbl) < 40) return;   /* 48 slots: a device fills nearly all */
    P.dev = candidate;
    capture_methods(candidate);
}

/* An interface out-parameter: a pointer to a slot holding an object whose
 * vtable is d3d9 code (CreateDevice's ppDevice is the one that matters). */
static int iface_out(uint32_t outptr, uint32_t *obj_out) {
    uint32_t obj, vtbl;
    if (!outptr || !readable(outptr, 4)) return 0;
    obj = rd32(outptr);
    if (!obj || (obj & 3) || !readable(obj, 4)) return 0;
    vtbl = rd32(obj);
    if (!in_range(vtbl, P.d3d9_lo, P.d3d9_hi)) return 0;
    *obj_out = obj;
    return 1;
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

static void report_draw(int kind, const uint32_t *a, uint32_t caller) {
    unsigned prims = a[2];
    unsigned stride = 0;
    uint32_t tex = P.tex0;
    const texrec_t *tr;
    int i, slot = -1;

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

    /* Once per texture, read its pixels back. Run with KORRA_DRAWPATH=2. */
    if (verbosity() >= 2 && tex) {
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

    if (slot < 0 || verbosity() < 1) return;
    if (g_sig[slot].hits >= (verbosity() >= 3 ? 8u : 3u)) { g_sig[slot].hits++; return; }
    g_sig[slot].hits++;

    tr = find_texture(tex);
    {
        const char *name = kind == 1 ? "DrawPrimitive"
                         : kind == 2 ? "DrawIndexedPrimitive"
                         : kind == 3 ? "DrawPrimitiveUP"
                                     : "DrawIndexedPrimitiveUP";
        fprintf(stderr, "[draw] sig#%d #%u %s prims=%u type=%u", slot,
                g_sig[slot].hits, name, prims, a[1]);
        if (kind == 3) fprintf(stderr, " stride=%u", a[4]);
        if (kind == 4) fprintf(stderr, " stride=%u", a[8]);
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
    if (kind == 3) {
        uint32_t pv = a[3];
        unsigned n = vertex_count(a[1], prims);
        stride = a[4];
        if (pv && stride && n && readable(pv, (size_t)stride * (n < 64 ? n : 64))) {
            unsigned pos = (P.fvf & 0x004) ? 4 : (P.fvf & 0x002) ? 3 : 0;
            float bb[4]; unsigned nan = 0; uint32_t first8[8];
            vertex_bbox((const unsigned char *)(uintptr_t)pv, n < 64 ? n : 64,
                        stride, pos, 0, 0, bb, &nan, first8);
            fprintf(stderr, "[draw]   verts=%u x=[%.2f..%.2f] y=[%.2f..%.2f]"
                            " nan=%u v0=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                    n, bb[0], bb[1], bb[2], bb[3], nan,
                    first8[0], first8[1], first8[2], first8[3],
                    first8[4], first8[5], first8[6], first8[7]);
        } else if (pv) {
            fprintf(stderr, "[draw]   vertex data 0x%08X not readable\n", pv);
        }
    }
}

static void report_frame(void) {
    if (verbosity() < 1) return;
    if ((P.frame % 60) != 0) return;
    fprintf(stderr, "[draw] frame %u draws=%u up=%u vb=%u prims=%u texs=%d"
                    " fvf=0x%X vs=0x%08X ps=0x%08X"
                    " abl=%u src=%u dst=%u zen=%u atest=%u afunc=%u\n",
            P.frame, P.draws, P.draws_up, P.draws_ib, P.prims, P.ntex_used,
            P.fvf, P.vs, P.ps,
            P.rs_seen[27] ? P.rs[27] : 0,          /* ALPHABLENDENABLE */
            P.rs_seen[19] ? P.rs[19] : 0,          /* SRCBLEND */
            P.rs_seen[20] ? P.rs[20] : 0,          /* DESTBLEND */
            P.rs_seen[7] ? P.rs[7] : 0,            /* ZENABLE */
            P.rs_seen[15] ? P.rs[15] : 0,          /* ALPHATESTENABLE */
            P.rs_seen[24] ? P.rs[24] : 0);         /* ALPHAFUNC */
}

/* -------------------------------------------------------------- the hook */

static void probe_post(uint32_t target, const uint32_t *a, uint32_t ret) {
    int mi;
    if (g_other_post && g_other_post != probe_post) g_other_post(target, a, ret);
    if (!P.on) return;

    /* d3d9.dll is loaded by native32_bind, after main() starts, so the module
     * range cannot be resolved at init time. */
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
                fprintf(stderr, "[draw] IDirect3D9* 0x%08X vtable 0x%08X\n",
                        ret, rd32(ret));
                fflush(stderr);
            }
            return;
        }
    }

    if (!P.have_dev) {
        if (P.d3d && a[0] == P.d3d) {
            /* IDirect3D9::CreateDevice(this, adapter, type, hwnd, flags,
             * ppPresentParams, ppDevice) - scan the pointer slots so the
             * interface is captured whatever the slot numbering is. */
            int k;
            uint32_t dev = 0;
            for (k = 4; k <= 8 && !dev; k++)
                if (iface_out(a[k], &dev)) { /* found */ }
            if (dev) {
                P.dev = dev;
                capture_methods(dev);
            }
            if (verbosity() >= 2)
                fprintf(stderr, "[draw] factory call target=0x%08X slot=%d ret=0x%08X\n",
                        target, method_index(target), ret);
            if (P.have_dev) return;
        }
        sniff_device(a[0]);
        if (P.have_dev && verbosity() >= 1) {
            fprintf(stderr, "[draw] device from first COM call, caller=sub_%08X\n", g_cur_func);
            fflush(stderr);
        }
    }

    mi = method_index(target);
    if (mi < 0) return;

    switch (mi) {
    case D3DDEV_PRESENT:
        report_frame();
        P.frame++;
        P.draws = P.draws_up = P.draws_ib = P.prims = 0;
        P.ntex_used = 0;
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
    case D3DDEV_SETVERTEXDECLARATION: P.vdecl = a[1]; break;
    case D3DDEV_SETSTREAMSOURCE:       if (a[1] == 0) { P.stream0 = a[2]; P.stream0_stride = a[3]; } break;
    case D3DDEV_SETINDICES:            P.indices = a[1]; break;
    case D3DDEV_DRAWPRIMITIVE:         report_draw(1, a, g_cur_func); break;
    case D3DDEV_DRAWINDEXEDPRIMITIVE:  report_draw(2, a, g_cur_func); break;
    case D3DDEV_DRAWPRIMITIVEUP:       report_draw(3, a, g_cur_func); break;
    case D3DDEV_DRAWINDEXEDPRIMITIVEUP: report_draw(4, a, g_cur_func); break;
    case D3DDEV_CREATETEXTURE:
        /* (this, Width, Height, Levels, Usage, Format, Pool, ppTexture) */
        note_texture(rd32(a[7]), a[1], a[2], a[5], a[6], a[3], "device CreateTexture");
        break;
    default:
        /* D3DX lives in its own DLL and is reached through the same bridge. */
        break;
    }

    /* D3DXCreate*: name is available because the four imports are bound by
     * name; log each creation so a draw's texture pointer can be identified. */
    {
        const char *n = native32_name(target);
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
        }
    }
}

/* --------------------------------------------------------------- install */

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(500);
        if (native32_post_hook != probe_post) {
            if (native32_post_hook) g_other_post = native32_post_hook;
            native32_post_hook = probe_post;
        }
    }
    return 0;
}

static void probe_start(void) {
    P.on = verbosity();
    if (!P.on) return;
    if (native32_post_hook && native32_post_hook != probe_post)
        g_other_post = native32_post_hook;
    native32_post_hook = probe_post;
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    /* No log line here: this runs at CRT init, before main() has pointed stderr
     * at the per-run log file. The first hook call reports the settings. */
}

/* Runs at CRT init, before main(): main() and the shared host files belong to
 * another work stream this round, and src\host\*.c is globbed into the build,
 * so this file installs itself. */
#pragma section(".CRT$XCU", long, read)
__declspec(allocate(".CRT$XCU")) static void (*const g_probe_init)(void) = probe_start;
