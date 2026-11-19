/*
 * thiscall.h - vtable entries a lifted guest can call.
 *
 * Why this exists
 * ---------------
 * native32 hands a COM-style call to the host with `this` in ECX and the
 * remaining arguments on the guest stack (native_bridge: "mov ecx, edi; call
 * eax"). A plain C function would look for `this` on the stack instead, so
 * every vtable entry here is a naked thunk that moves ECX onto the stack and
 * calls a normal cdecl body:
 *
 *   entry:  [esp]=ret  [esp+4]=a1  [esp+8]=a2 ...
 *   push ecx            -> this at [esp], ret at [esp+4]
 *   call body           -> body(self, a1, a2) with self at [esp+4]
 *   add  esp, 4         -> drop self (body is cdecl: it pops nothing)
 *   ret  purge          -> pop ret + the guest's arguments
 *
 * native_bridge measures how far the callee moved esp and adds that to g_esp,
 * so PURGE_BYTES must be exactly 4 + 4*<argument dwords>. A wrong value
 * desynchronises the guest stack, therefore every use is checked against the
 * decompiled call site in docs/xaudio2_callsites.md before it is written.
 *
 * Example: IXAudio2SourceVoice::Stop(UINT32 flags, UINT32 opset)
 *   HOST_VT_ENTRY(xa2_vt_Stop, xa2_Stop, 12)   // ret + 8 bytes of arguments
 *   static HRESULT __cdecl xa2_Stop(xa2_voice *self, UINT32 flags, UINT32 opset);
 *
 * The thunk and the body must be declared before the vtable array that names
 * them. The guest reaches them through recomp_lookup_import(), which treats a
 * VA outside every mapped guest module as a native function and bridges it.
 */
#ifndef KORRA_HOST_THISCALL_H
#define KORRA_HOST_THISCALL_H

#define HOST_VT_ENTRY(name, body, purge_bytes)              \
    static void __declspec(naked) name(void) {              \
        __asm { push ecx }                                  \
        __asm { call body }                                 \
        __asm { add  esp, 4 }                               \
        __asm { ret  purge_bytes }                          \
    }

/* A vtable is an array of these; the object's first dword points at it. */
typedef void (*host_vt_entry)(void);

#endif /* KORRA_HOST_THISCALL_H */
