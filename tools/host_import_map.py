#!/usr/bin/env python3
"""host_import_map.py - reproducible host-ABI evidence from LoK.exe.

Track D's shims (steam_api, XAudio2) were written against what the shipped
binary actually does, not against what the SDK headers say. This script
regenerates that evidence with pefile + capstone, so any claim in
docs/xaudio2_callsites.md can be re-checked without Ghidra:

  guid      where each audio/COM GUID appears in the image (count + VA)
  iat       the IAT slot of each named import, per DLL
  ff15      every "call dword ptr [<iat>]" site for a named import
  indirect  every "call [reg+disp]" in a VA range, with the displacement
  callers   direct "call rel32" sites that reach one or more VAs

Read-only. Writes nothing unless --json is given (put that under work/).

    python tools/host_import_map.py --exe work/LoK.analysis.exe --all
    python tools/host_import_map.py --exe ... --indirect 0x7b2200 0x7b3420
"""
import argparse
import json
import struct
import sys

GUID_NAMES = {
    "CLSID_XAudio2": "5a508685-a254-4fba-9b82-9a24b00306af",
    "CLSID_XAudio2Debug": "db05ea35-0329-4d4b-a53a-6dead03d3852",
    "IID_IXAudio2": "8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb",
    "CLSID_DirectSound8": "3901cc3f-84b5-4fa4-ba35-aa8172b8a09b",
    "IID_IDirectSound8": "c50a7e93-f395-4834-9ef6-7fa99de50966",
    "CLSID_MMDeviceEnumerator": "bcde0395-e52f-467c-8e3d-c4579291692e",
    "IID_IMMDeviceEnumerator": "a95664d2-9614-4f35-a746-de8db63617e6",
    "CLSID_WbemLocator": "4590f811-1d3a-11d0-891f-00aa004b2e24",
}


def guid_bytes(text):
    a, b, c, d1, d2 = text.split("-")
    return (struct.pack("<IHH", int(a, 16), int(b, 16), int(c, 16))
            + struct.pack(">H", int(d1, 16)) + bytes.fromhex(d2))


class PE:
    def __init__(self, path):
        import pefile
        self.path = path
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        with open(path, "rb") as f:
            self.data = f.read()

    def va_to_off(self, va):
        rva = va - self.base
        for s in self.pe.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize:
                return s.PointerToRawData + (rva - s.VirtualAddress)
        return None

    def read(self, va, n):
        off = self.va_to_off(va)
        return self.data[off:off + n] if off is not None else b""

    def imports(self):
        """[(dll, name, iat_va)] for the standard import table."""
        out = []
        if not hasattr(self.pe, "DIRECTORY_ENTRY_IMPORT"):
            return out
        for dll in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in dll.imports:
                if imp.name:
                    out.append((dll.dll.decode("ascii", "replace"),
                                imp.name.decode("ascii", "replace"),
                                self.base + imp.address - self.base))
        return out

    def iat_va(self, name):
        for _dll, n, va in self.imports():
            if n == name:
                return va
        return None

    def code_ranges(self):
        return [(self.base + s.VirtualAddress,
                 self.base + s.VirtualAddress + s.Misc_VirtualSize)
                for s in self.pe.sections if s.Characteristics & 0x20000000]


def scan_guids(pe):
    found = {}
    for name, text in GUID_NAMES.items():
        pat = guid_bytes(text)
        hits, i = [], -1
        while True:
            i = pe.data.find(pat, i + 1)
            if i < 0:
                break
            va = None
            for s in pe.pe.sections:
                lo = s.PointerToRawData
                hi = lo + s.SizeOfRawData
                if lo <= i < hi:
                    va = pe.base + s.VirtualAddress + (i - lo)
                    break
            hits.append(hex(va) if va else f"file+0x{i:x}")
        found[name] = {"guid": text, "hits": hits}
    return found


def find_ff15(pe, iat_va):
    """call dword ptr [iat_va] -> FF 15 <va32>"""
    needle = b"\xff\x15" + struct.pack("<I", iat_va)
    hits, i = [], -1
    while True:
        i = pe.data.find(needle, i + 1)
        if i < 0:
            break
        for s in pe.pe.sections:
            if s.PointerToRawData <= i < s.PointerToRawData + s.SizeOfRawData:
                hits.append(pe.base + s.VirtualAddress + (i - s.PointerToRawData))
                break
    return hits


def find_callers(pe, targets, limit=40):
    """Direct rel32 call/jmp sites that reach one of `targets`."""
    out = {t: [] for t in targets}
    with open(pe.path, "rb") as f:
        raw = f.read()
    for lo, hi in pe.code_ranges():
        start = pe.va_to_off(lo)
        end = start + (hi - lo)
        chunk = raw[start:end]
        for i in range(len(chunk) - 5):
            op = chunk[i]
            if op not in (0xE8, 0xE9):
                continue
            rel = struct.unpack_from("<i", chunk, i + 1)[0]
            va = lo + i
            dst = va + 5 + rel
            if dst in out and len(out[dst]) < limit:
                out[dst].append(hex(va))
    return out


def find_indirect(pe, lo, hi):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    off = pe.va_to_off(lo)
    if off is None:
        raise SystemExit(f"0x{lo:x} is not mapped")
    code = pe.data[off:off + (hi - lo)]
    out = []
    for ins in md.disasm(code, lo):
        if ins.mnemonic != "call" or not ins.operands:
            continue
        op = ins.operands[0]
        if op.type != capstone.x86.X86_OP_MEM:
            continue
        m = op.mem
        if m.base and not m.index:
            out.append({"at": hex(ins.address), "reg": ins.reg_name(m.base),
                        "disp": hex(m.disp & 0xFFFFFFFF), "text": ins.op_str})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--guid", action="store_true")
    ap.add_argument("--imports", nargs="*", metavar="NAME")
    ap.add_argument("--indirect", nargs=2, metavar=("LO", "HI"))
    ap.add_argument("--callers", nargs="+", metavar="VA")
    ap.add_argument("--json")
    args = ap.parse_args()

    pe = PE(args.exe)
    result = {"exe": args.exe, "image_base": hex(pe.base)}
    everything = args.all or not any([args.guid, args.imports, args.indirect, args.callers])

    if everything or args.guid:
        result["guids"] = scan_guids(pe)

    names = args.imports or (["CoCreateInstance", "SteamUser", "SteamUserStats",
                              "SteamAPI_Init", "SteamAPI_RegisterCallback"] if everything else [])
    if names:
        result["iat"] = {}
        for name in names:
            va = pe.iat_va(name)
            entry = {"iat_va": hex(va) if va else None}
            if va:
                entry["call_sites"] = [hex(x) for x in find_ff15(pe, va)]
            result["iat"][name] = entry

    if everything or args.indirect:
        lo, hi = (0x7B2200, 0x7B3420) if not args.indirect else (int(args.indirect[0], 0), int(args.indirect[1], 0))
        result["indirect"] = {"range": [hex(lo), hex(hi)], "calls": find_indirect(pe, lo, hi)}

    if everything or args.callers:
        targets = [int(v, 0) for v in (args.callers or
                                      ["0x7b22a8", "0x7b2e24", "0x8701f0", "0x8793f0", "0x88aae0"])]
        result["callers"] = {hex(t): v for t, v in find_callers(pe, targets).items()}

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=1)
        print(f"wrote {args.json}")
    else:
        json.dump(result, sys.stdout, indent=1)
        print()


if __name__ == "__main__":
    main()
