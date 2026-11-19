# Lift report — LoK.exe (2026-11-19)

Produced by `scripts/run_korra_lift.py` against `pcrecomp` `35548b8` + patch
series `0001–0014` + MP2 SEH patch (see `TOOLCHAIN.md`).

## Input

| | |
|---|---|
| Binary | `LoK.exe` (SHA-256 `497eedaf…4f683`), analysis copy is byte-identical |
| Code range | `.text`, 0x4FB9BA bytes virtual |
| Imports (IAT slots) | 194 in 12 DLLs |
| Seeds | **8,866** (8,569 vtable methods, 4,735 RTTI virtual methods, merged; + entry point) |

## Discovery (recursive descent, 6 rounds)

| Stage | Count |
|---|---|
| Call/prologue targets + seeds | → 6 rounds to a fixpoint |
| Functions disassembled | **37,029** |
| &nbsp;&nbsp;`start` entries | 25,188 |
| &nbsp;&nbsp;`alias` entries (labels inside another body, still dispatchable) | 11,841 |
| jmp-thunks | 107 |
| C++ EH stubs/funclets folded as aliases | 1,958 |
| Scan candidates that were labels inside a body | 2,721 |
| Rejected non-instruction-boundary candidates | 1,878 |
| Data-pointer-only functions found | 2,146 |
| Branch targets given a dispatchable body | 3,020 |
| Extents clamped to the next function start | 2,007 |
| Wall time | 622.7 s |

## Lift

| Stage | Result |
|---|---|
| Functions lifted | **37,029 / 37,029** |
| **Failures** | **0** |
| Output | 75 `recomp_NNNN.c` + `recomp_funcs.h` + `recomp_dispatch.c` |
| Lines of C | 12,016,872 |
| Size on disk | 613 MB |
| Wall time | 229.5 s |

Every function lowered with the unmodified patch series: no Korra-specific
lifter fix was needed. That is the expected (and hoped-for) outcome given the
codegen profile — MSVC 2012-era x87 + SSE2, no MMX-only loops were reported.

## Notes for the host stage

- 613 MB / 12M lines is about 35% more generated code than NFSU2's 8.9M lines.
  Expect a long first compile and consider `--split` tuning plus per-file
  opt-outs (`/O1`) if a single TU hits the MSVC 64k-section or compile-time wall.
- 11,841 aliases are intentional: they are branch targets inside other
  functions (shared epilogues, switch arms) that must stay dispatchable.
- The catalog (`work/catalog.json`, 27 MB) is regenerable and stays out of the
  repo; the seed set is committed as `config/seeds_all.json` so anyone with the
  same binary reproduces the same 37k functions.
