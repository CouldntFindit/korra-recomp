/* pe_android.c - map the guest image's data and bind its import table.
 *
 * Only data matters: the lifted C is the program and the image's .text is never
 * executed. But the image still has to be at its VAs, because the engine derives
 * GameData from its own module path, stores 32-bit pointers to its vtable images
 * and string literals, and reads its resources from .rdata/.data. Sections are
 * committed RW (not RX): nothing executes there and the guest reads/writes data
 * that lives in .text on disk (jump tables, string constants).
 *
 * No relocation pass: the image is mapped at its linker-chosen base 0x400000, so
 * every HIGHLOW absolute is already correct.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_android.h"
#include "pe_format.h"

static const uint8_t *g_file;
static uint32_t g_image = KA_IMAGE_BASE;
static uint32_t g_span;
static uint32_t g_entry;

static const pe_opt_header32 *opt(void) {
    const pe_dos_header *dos = (const pe_dos_header *)g_file;
    const uint8_t *nt = g_file + dos->e_lfanew;
    return &((const pe_nt_headers32 *)nt)->OptionalHeader;
}

int ka_pe_map(const char *exe_path) {
    FILE *f = fopen(exe_path, "rb");
    long size;
    uint8_t *buf;
    const pe_dos_header *dos;
    const pe_nt_headers32 *nt;
    const pe_section_header *sec;
    uint32_t i;

    if (!f) { ka_log("PE: cannot open %s", exe_path); return -1; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        ka_log("PE: read failed"); fclose(f); free(buf); return -1;
    }
    fclose(f);
    g_file = buf;

    dos = (const pe_dos_header *)buf;
    if (dos->e_magic != 0x5A4D) { ka_log("PE: no MZ"); return -1; }
    if (dos->e_lfanew <= 0 || (long)dos->e_lfanew + (long)sizeof(pe_nt_headers32) > size) {
        ka_log("PE: bad e_lfanew");
        return -1;
    }
    nt = (const pe_nt_headers32 *)(buf + dos->e_lfanew);
    if (nt->Signature != 0x00004550u) { ka_log("PE: no PE signature"); return -1; }
    if (nt->OptionalHeader.Magic != 0x10B) { ka_log("PE: not PE32"); return -1; }
    if (nt->OptionalHeader.ImageBase != KA_IMAGE_BASE) {
        ka_log("PE: image base 0x%08X != expected 0x%08X",
               nt->OptionalHeader.ImageBase, KA_IMAGE_BASE);
        return -1;
    }

    g_span  = nt->OptionalHeader.SizeOfImage;
    g_entry = g_image + nt->OptionalHeader.AddressOfEntryPoint;

    if (ka_commit(g_image, g_span, 0) != 0) {
        ka_log("PE: cannot commit image pages at 0x%08X", g_image);
        return -1;
    }
    memset(ka_host(g_image), 0, g_span);
    memcpy(ka_host(g_image), buf, nt->OptionalHeader.SizeOfHeaders);

    sec = (const pe_section_header *)((const uint8_t *)&nt->OptionalHeader
                                      + nt->FileHeader.SizeOfOptionalHeader);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t va = g_image + sec[i].VirtualAddress;
        uint32_t vs = sec[i].VirtualSize ? sec[i].VirtualSize : sec[i].SizeOfRawData;
        if (!vs || va < g_image || va + vs > g_image + g_span) continue;
        if (ka_commit(va, vs, 0) != 0) {
            ka_log("PE: cannot commit section %u at 0x%08X", i, va);
            return -1;
        }
        if (sec[i].SizeOfRawData && sec[i].PointerToRawData) {
            uint32_t n = sec[i].SizeOfRawData < vs ? sec[i].SizeOfRawData : vs;
            memcpy(ka_host(va), buf + sec[i].PointerToRawData, n);
        }
        ka_log("PE: section %.8s 0x%08X size 0x%05X", (const char *)sec[i].Name, va, vs);
    }

    ka_log("PE: mapped 0x%08X-0x%08X entry 0x%08X (%ld bytes on disk)",
           g_image, g_image + g_span, g_entry, size);
    return 0;
}

uint32_t ka_pe_span(void) { return g_span; }
uint32_t ka_pe_entry(void) { return g_entry; }

/* Ordinal imports Korra has: XINPUT1_3 ord 2 = XInputGetState, ord 3 =
 * XInputSetState. Anything else ordinal is named "dll#n" and gets a stub. */
static const char *ordinal_name(const char *dll, uint32_t ord) {
    if (!strcasecmp(dll, "xinput1_3.dll")) {
        if (ord == 2) return "XInputGetState";
        if (ord == 3) return "XInputSetState";
    }
    /* OLEAUT32 exports its most-used functions by ordinal in this binary. */
    if (!strcasecmp(dll, "oleaut32.dll")) {
        if (ord == 2)   return "SysAllocString";
        if (ord == 6)   return "SysFreeString";
        if (ord == 418) return "OleLoadPicture";
    }
    if (!strcasecmp(dll, "d3dx9_43.dll")) {
        if (ord == 1) return "D3DXCreateTexture";     /* not used; names only */
    }
    return NULL;
}

int ka_pe_bind(void) {
    const pe_opt_header32 *o = opt();
    uint32_t imp_rva = o->DataDirectory[PE_DIR_IMPORT].VirtualAddress;
    uint32_t imp_sz  = o->DataDirectory[PE_DIR_IMPORT].Size;
    const pe_import_descriptor *d;
    int unresolved = 0;

    if (!imp_rva) { ka_log("PE: no import directory"); return 0; }

    for (d = (const pe_import_descriptor *)ka_host(g_image + imp_rva);
         (uint8_t *)d < ka_host(g_image + imp_rva + imp_sz) && d->Name;
         d++) {
        const char *dll = (const char *)ka_host(g_image + d->Name);
        const uint32_t *ilt = (const uint32_t *)ka_host(g_image + (d->OriginalFirstThunk
                                                                   ? d->OriginalFirstThunk
                                                                   : d->FirstThunk));
        uint32_t *iat = (uint32_t *)ka_host(g_image + d->FirstThunk);
        uint32_t i;
        int bound = 0, missing = 0;
        for (i = 0; ilt[i]; i++) {
            uint32_t name_va = 0;
            char sym[160];
            uint32_t va;
            if (ilt[i] & PE_ORDINAL_FLAG) {
                uint32_t ord = ilt[i] & 0xFFFFu;
                const char *n = ordinal_name(dll, ord);
                if (n) snprintf(sym, sizeof sym, "%s", n);
                else   snprintf(sym, sizeof sym, "%s#%u", dll, ord);
            } else {
                name_va = g_image + ilt[i] + 2;   /* skip the hint word */
                snprintf(sym, sizeof sym, "%s", (const char *)ka_host(name_va));
            }
            va = ka_shim_va(sym);
            if (!va) {
                char full[200];
                snprintf(full, sizeof full, "%s!%s", dll, sym);
                va = ka_shim_stub(full);
                if (missing < 6) ka_log("PE: unresolved import %s!%s", dll, sym);
                missing++; unresolved++;
            }
            iat[i] = va;
            bound++;
        }
        ka_log("PE: %-14s %3d imports (%d unresolved)", dll, bound, missing);
    }
    return unresolved;
}
