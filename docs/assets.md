# Assets — CPK archives & USM movies (Track B)

**Status:** verified 2026-11-19 against the retail Steam install.
Everything below was measured on the real files; claims that are *inferred* are
marked as such. Tools: `tools/cpk_manifest.py`, `tools/cpk_extract.py`,
`tools/usm_demux.py`, `tools/hexpeek.py` (all stdlib-only Python).

What this document answers:

| Question | Answer | § |
|---|---|---|
| How is game data packaged? | 2 CRI CPK archives (1.87 GB) + loose `movie\`, `movie_logo\`, `sound\` | 1 |
| Are the CPKs compressed? | Yes — CRILAYLA, 672 of 1,213 entries | 2, 3 |
| Can we read every file name without extracting? | Yes — 1,213 names come out of the two TOCs in ~1–2 s per archive | 4 |
| Does CRILAYLA decode correctly? | Yes — every compressed entry decodes to exactly its declared `ExtractSize` | 3.3 |
| Are the movies playable? | Yes — 1280×720 MPEG-2 + 48 kHz ADX; demuxed and transcoded to MP4 | 6 |
| Does the port need any of this tooling? | No. M2.10 only needs file I/O; the lifted CRI code decompresses itself | 9 |

---

## 1. Where the data lives

```
The Legend of Korra\
  LoK.exe                    6,764,032 B    (never committed; see §7 of the plan)
  GameData\
    data000.cpk            265,209,888 B    asset archive #1  (menu/core/characters)
    data001.cpk          1,600,934,448 B    asset archive #2  (stages, bulk textures)
    movie\  CS*.usm   30 files  777.5 MB    story cutscenes
    movie_logo\ *.usm  4 files   22.5 MB    Activision / Nickelodeon / PG5R / CRIWARE
    sound\  Init.bnk, WwiseInfo.wai, SeFootstep.bin, bgm\, stream\   (Track A, docs/audio.md)
```

Totals: **1.87 GB** in the two CPKs, **800 MB** of movies, plus the Wwise tree.
The install ships no loose data files — every asset except movies and audio
streams is inside a CPK.

---

## 2. CPK container format (verified field by field)

Both archives are CRI **CPK 2.42** (`Tvers = "CPKMC2.42.00, DLL3.12.00"`,
`Version = 7`, `Revision = 2`, `Align = 2048`, `Codec = 0` = CRILAYLA,
`CpkMode = 1`). All integers are **big-endian**.

```
0x0000  "CPK "                 u32 0x000000FF        u32 utf_table_size+8   u32 0
0x0010  "@UTF" table  (CpkHeader, 1 row of 35 columns)
0x0800  "TOC  " chunk header (u32 size_field, u32 0) → "@UTF" table at +0x10
...     padding to ContentOffset
ContentOffset  file payload area (0x800-aligned entries, size is the *stored* size)
EtocOffset     "ETOC" chunk (4,128 B) — extended attributes, not needed for a manifest
```

### UTF table layout

20-byte header, then the column schema, then rows, then the string table:

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | char[4] | `@UTF` |
| +0x04 | u32 | table size; **real size = value + 8** |
| +0x08 | u16 | version (0 or 1) |
| +0x0A | u16 | rows offset; **+8** |
| +0x0C | u32 | string-table offset; **+8** |
| +0x10 | u32 | data offset; **+8** |
| +0x14 | u32 | table-name offset (relative to the string table) |
| +0x18 | u16 | column count |
| +0x1A | u16 | row width in bytes |
| +0x1C | u32 | row count |

Column entry = `u8 info` + `u32 name offset` (+ optional default value, + row slot):

* `info` low nibble = type: `0` u8, `1` s8, `2` u16, `3` s16, `4` u32, `5` s32,
  `6` u64, `7` s64, `8` f32, `9` f64, `A` string (u32 offset into the string
  table), `B` VLData, `C` GUID.
* `info` high nibble = flags: `0x10` has a name, `0x20` has a default value,
  `0x40` is stored in the row (`0x50` = nested-table pointer).

**Every offset in the header is relative to `table_start + 8`, not to
`table_start`** — this is the one detail that breaks naive parsers. The TOC
table itself is wrapped in a `"TOC "` chunk, so the `@UTF` magic sits at
`TocOffset + 0x10`, not at `TocOffset`.

### TOC schema (`CpkTocInfoEx`, 8 columns × 1,213 rows)

| Column | Type | Notes |
|---|---|---|
| `DirName` | string | e.g. `em/`, `core/`, `st4/` — usually with a trailing slash, sometimes empty |
| `FileName` | string | base name with extension |
| `FileSize` | u32 | **stored** size (what you read from the archive) |
| `ExtractSize` | u32 | size after CRILAYLA decode; `> FileSize` means compressed |
| `FileOffset` | u64 | offset of the stored data |
| `ID` | u32 | dense per archive, doubles as a sort key |
| `UserString` | string | always `"<NULL>"` here |
| `CRC` | u32 | per-entry CRC (algorithm not identified; not needed) |

**Offset quirk (verified, not guessed):** `FileOffset` is *not* relative to
`ContentOffset`. The correct base is

```
absolute = FileOffset + (ContentOffset - min(FileOffset)) = FileOffset + 0x800
```

With that base the entries form a **gapless, 0x800-aligned chain** (0 gaps over
331 and 882 entries) whose last entry ends **exactly at `EtocOffset`**. The check
is in `tools/cpk_manifest.py` and reproduced by `--manifest-only` in
`tools/cpk_extract.py`.

`CpkMode = 1` on both archives does **not** mean the UTF tables are encrypted —
the header and TOC parse as plaintext. Its meaning is still unidentified
(possibly "ID-sorted TOC" or a CRC flag); nothing in the pipeline depends on it.

---

## 3. CRILAYLA

### 3.1 Layout

```
0x00  "CRILAYLA"
0x08  u32le decompressed_body_size   (≡ ExtractSize − 0x100)
0x0C  u32le compressed_size          (≡ FileSize − 0x110)
0x10  … compressed_size bytes        backwards bitstream
tail  … 0x100 bytes                  verbatim prefix → output[0x00:0x100]
```

### 3.2 Decoder

The bitstream is read **from the end of the payload backwards**, MSB-first, and
the output is written **backwards** from its last byte down to `0x100`:

```
marker = read(1)
  marker == 0 → literal : out[pos] = read(8), pos -= 1
  marker == 1 → backref : offset = read(13), len = read(2)
                          len == 3  → len += read(3)
                          len == 10 → len += read(5)
                          len == 41 → len += read(8) while that byte == 0xFF
                          len += 3
                          copy len bytes from out[pos+offset+3] downwards
```

Implemented in `tools/cpk_extract.py` (`crilayla_decompress`). Two gotchas bit me
and are worth keeping in mind if this is re-implemented in C:

1. the accumulator must drop consumed bits (`bits &= (1<<count)-1`) or the
   Python bigint grows without bound and the decode becomes quadratic;
2. the loop must stop when the write position reaches `0x100`, even mid-match.

### 3.3 Verification result

Every entry in both archives was decoded in memory and compared against its TOC
`ExtractSize` (no bulk dump to disk):

| Archive | Entries | CRILAYLA | Non-zero mismatches | Decode errors |
|---|---:|---:|---:|---:|
| `data000.cpk` | 331 | 182 | **0** | **0** |
| `data001.cpk` | 882 | 490 | **0** | **0** |

(57 of the 1,213 entries are zero-byte placeholders — `em/emb001.dtt`,
`bm/bm1000.dtt`, … — and decode trivially.)

Spot checks that a size check alone cannot fake (decompressed payloads start
with a format magic): `autoshadereff.dat` 18,808 → 188,416 B, `core/coreeff.dat`
931,288 → 3,637,248 B and `core/coreph.dat` 2,328 → 4,096 B all begin with
`DAT\0`, `autoshadereffInfo.bxm` begins with `BXM\0`.

**CriCodecs could not be used for this check** — see §7.

---

## 4. File manifest (names, extensions, counts)

Neither archive stores a flattened path list; the path is `DirName + FileName`
(e.g. `DirName="core/"`, `FileName="core.dat"` → `core/core.dat`).

### 4.1 Totals

| | `data000.cpk` | `data001.cpk` | both |
|---|---:|---:|---:|
| entries | 331 | 882 | **1,213** |
| stored bytes | 264,984,074 | 1,600,357,724 | 1,865,341,798 |
| extracted bytes | 308,958,195 | 1,744,289,792 | **2,053,247,987** |
| CRILAYLA entries | 182 (50.1 MB stored) | 490 (77.9 MB stored) | 672 |
| zero-byte placeholders | 4 | 53 | 57 |
| largest entry | `ui/ui_manual.dtt` 67.2 MB | `st4/r4a2.dtt` 30.8 MB | |
| archive size | 265,209,888 | 1,600,934,448 | 1.87 GB |

Compression is modest overall (1.87 GB → 2.05 GB, ~9 %) because the biggest
`.dtt` blobs are stored raw on purpose (`FileSize == ExtractSize`); the files that
*are* compressed shrink a lot (e.g. 300 B → 4,096 B, 931 KB → 3.6 MB).

### 4.2 Extensions

| ext | `data000` | `data001` | total | extracted bytes |
|---|---:|---:|---:|---:|
| `.dat` | 169 | 499 | 668 | 333,054,816 |
| `.dtt` | 140 | 383 | 523 | 1,707,253,760 |
| `.evn` | 12 | – | 12 | 7,855,040 |
| `.bxm` | 5 | – | 5 | 31,175 |
| `.evt` | 4 | – | 4 | 5,038,080 |
| `.xml` | 1 | – | 1 | 15,116 |

### 4.3 Directory / naming scheme

`data000.cpk` (331):

| dir | files | name pattern | what it is (inferred) |
|---|---:|---|---|
| `em/` | 104 | `em####.dat`+`.dtt`, `emb###.*` | enemy models + textures |
| `ui/` | 62 | `ui_tex_pl##.dat`+`.dtt`, `ui_*.dat` | UI textures/atlases |
| `et/` | 61 | `et####.dat`+`.dtt`, `eta####.*` | environment/effects sets |
| `pl/` | 37 | `pl####.dat`+`.dtt`, `pl####f#.*` | player (Korra) models, alternate forms |
| `event/` | 16 | `ev####.evn`, `ev####.evt` | event/script data |
| `wp/` | 12 | `wp####.dat`+`.dtt` | weapons |
| `it/` | 11 | `it####.dat`+`.dtt` | items |
| `core/` | 7 | `core*.dat`+`.dtt` | core tables: `core`, `coreeff`, `coreph`, `coreui` |
| `st8/` | 6 | `r####.dat`, `r####a#.*` | stage/room data |
| `ph8/` | 2 | `p####.dat`+`.dtt` | physics/physics colliders |
| root | 13 | `shader*.dat`, `vfxShader*.dat`, `*.bxm`, `vfxShadersetting.xml` | shader bundles (BXM containers) |

`data001.cpk` (882):

| dir | files | pattern | what it is (inferred) |
|---|---:|---|---|
| `bm/` | 579 | `bm####.dat`/`.dtt`, `bma###.*` | boss + large environment models |
| `ba/` | 105 | `ba####.dat`/`.dtt`, `ba####a.*` | background/architecture |
| `st4`,`st5`,`stb`,`st2`,`st7`,`stf`,`st6`,`st3`,`st1`,`sta` | 5–42 each | `r####.dat`, `r####a#.dtt`, `rb####.*` | per-stage rooms, each stage dir holds its own set |
| `ph*` (`phc`,`phf`,`phe`,`ph7`,`ph2`…`phb`,`pha`,`phd`,`ph1`,`ph5`) | 1–8 each | `p####.dat`/`.dtt` | stage colliders |

`.dat`/`.dtt` pairing: 523 of 668 `.dat` files have a same-stem `.dtt`
(140/169 in data000, 383/499 in data001). The `.dtt` is always the bigger one —
`.dat` carries the structure, `.dtt` the payload (textures/geometry).

Regenerate the full 1,213-line manifest locally (never committed):

```powershell
python tools\cpk_manifest.py "The Legend of Korra\GameData\data000.cpk" --json data000.json --markdown data000.md
python tools\cpk_manifest.py "The Legend of Korra\GameData\data001.cpk" --json data001.json --markdown data001.md
```

---

## 5. What is inside a `.dat` / `.dtt`

Decompressed payload magic histogram over all 1,213 entries: `DAT\0` × 1,148,
`BXM\0` × 5, zero-byte × 57, and 3 outliers — `ShaderSign.dat`
(`80 01 80 00 …`, a shader-signature table), `VfxShaderSign.dat`
(`e1 89 80 06 …`) and `vfxShadersetting.xml` (plain `<root>` XML). So essentially
every asset is a **DAT container** and the shader bundles are `BXM`
(binary XML, "BXM\0" magic).

Observed `DAT\0` header (first 0x40 bytes of `core/coreph.dat` and
`ui/ui_tex_pl01.dat`):

```
43 44 41 54 00 | "DAT\0"
u32 version    | 0 = plain, 1 = with subfile table, 2 = with uvd/wta table, 0xC4 seen on autoshadereff
u32 0x20       | header size
u32 [5]        | section offsets (in 8-byte units? values are +0x20 after shifting)
...
u32 names_off  |
"bxm\0" u32 len "PhaseInfo.bxm\0"     ← sub-file 4CC + name, then data
"uvd\0" "wta\0" "tex_pl01.uvd\0"      ← texture: uv data + weight table
```

That is enough to confirm `.dat` is a named sub-file bundle (4CC + name + length
+ payload), which is what the engine's `Hw::` asset loader walks. Full DAT
semantics are Ghidra work (Track C) — not needed for M2.

---

## 6. USM movies

### 6.1 Container

```
0x0000 "CRID"  u32 size_field (= block length − 8)      → first chunk at 8+size_field
0x0020 "@UTF"  CRIUSF_DIR_STREAM directory table (stream ids, names, sizes)
chunks: (length = u32@+0x04 + 8), header 0x20 B, big-endian
   +0x00 "@SFV" | "@SFA" | "@ALP" | "@CUE" | "@SBT" | "@STA" | "@USR" | "@AHX"
   +0x04 u32 size_field          +0x08 u16 payload_offset (0x18)
   +0x0A u16 pad                 +0x0C u8 channel (always 0 here)
   +0x0E u16 type: 0 data · 1 stream table · 2 "#HEADER"/"#METADATA" · 3 metadata
   +0x10 u32 frame_time          +0x14 u32 frame_rate (×100, e.g. 2397 = 23.97)
payload = chunk + 8 + payload_offset;  for data chunks the last `pad` bytes are
padding (verified: 100 % of stripped bytes are 0x00/0xFF across all 34 files)
```

`frame_time` is **not milliseconds** — the tick is 1/3000 s. Cross-check: 125
ticks × 23.976 fps = 2997 ≈ 3000, and the last video tick (8,000) matches
65 frames ÷ 23.976 (2.71 s), i.e. the same duration ADX declares. Durations in
this document are therefore taken **from the codec** (picture count ÷ fps,
ADX samples ÷ rate), not from the timeline field.

### 6.2 Codec inventory (all 34 movies)

Video: **MPEG-1/2 elementary stream** (`00 00 01 B3` sequence header, no MPEG-2
sequence extension → ffmpeg reports `mpeg1video`), all **1280×720 yuv420p**.
Audio: **ADX** (`0x8000` magic, 4-bit ADPCM, 18-byte blocks), 48 kHz, 5.1 on all
story cutscenes, stereo on the Nickelodeon logo, absent on the CRIWARE logo.

| file | size | frames | fps | video | audio | audio |
|---|---:|---:|---:|---:|---|---:|
| `movie/CS0001.usm` | 24.4 MB | 528 | 23.976 | 22.02 s | ADX 6ch/48k | 22.00 s |
| `movie/CS0011.usm` | 45.3 MB | 947 | 23.976 | 39.50 s | ADX 6ch/48k | 39.50 s |
| `movie/CS0012.usm` | 52.0 MB | 1116 | 23.976 | 46.55 s | ADX 6ch/48k | 46.55 s |
| `movie/CS0021.usm` | 32.8 MB | 673 | 23.976 | 28.07 s | ADX 6ch/48k | 28.07 s |
| `movie/CS0022.usm` | 25.5 MB | 526 | 23.976 | 21.94 s | ADX 6ch/48k | 21.94 s |
| `movie/CS0031.usm` | 53.1 MB | 1091 | 23.976 | 45.50 s | ADX 6ch/48k | 45.50 s |
| `movie/CS0032.usm` | 10.3 MB | 214 | 23.976 | 8.93 s | ADX 6ch/48k | 8.93 s |
| `movie/CS0033.usm` | 14.3 MB | 295 | 23.976 | 12.30 s | ADX 6ch/48k | 12.26 s |
| `movie/CS0041.usm` | 3.2 MB | 65 | 23.976 | 2.71 s | ADX 6ch/48k | 2.71 s |
| `movie/CS0042.usm` | 9.1 MB | 186 | 23.976 | 7.76 s | ADX 6ch/48k | 7.76 s |
| `movie/CS0043.usm` | 10.2 MB | 213 | 23.976 | 8.88 s | ADX 6ch/48k | 8.88 s |
| `movie/CS0044.usm` | 14.7 MB | 302 | 23.976 | 12.60 s | ADX 6ch/48k | 12.60 s |
| `movie/CS0051.usm` | 13.3 MB | 270 | 23.976 | 11.26 s | ADX 6ch/48k | 11.26 s |
| `movie/CS0052.usm` | 8.8 MB | 185 | 23.976 | 7.72 s | ADX 6ch/48k | 7.70 s |
| `movie/CS0053.usm` | 11.5 MB | 236 | 23.976 | 9.84 s | ADX 6ch/48k | 9.84 s |
| `movie/CS0054.usm` | 22.0 MB | 448 | 23.976 | 18.68 s | ADX 6ch/48k | 18.67 s |
| `movie/CS0055.usm` | 7.7 MB | 161 | 23.976 | 6.71 s | ADX 6ch/48k | 6.71 s |
| `movie/CS0056.usm` | 10.2 MB | 205 | 23.976 | 8.55 s | ADX 6ch/48k | 8.55 s |
| `movie/CS0061.usm` | 9.0 MB | 184 | 23.976 | 7.67 s | ADX 6ch/48k | 7.67 s |
| `movie/CS0062.usm` | 12.0 MB | 230 | 23.976 | 9.59 s | ADX 6ch/48k | 9.59 s |
| `movie/CS0063.usm` | 57.6 MB | 1183 | 23.976 | 49.34 s | ADX 6ch/48k | 49.34 s |
| `movie/CS0071A.usm` | 10.8 MB | 227 | 23.976 | 9.47 s | ADX 6ch/48k | 11.36 s |
| `movie/CS0071B.usm` | 17.5 MB | 417 | 23.976 | 17.39 s | ADX 6ch/48k | 18.50 s |
| `movie/CS0072.usm` | 36.1 MB | 742 | 23.976 | 30.95 s | ADX 6ch/48k | 30.95 s |
| `movie/CS0081.usm` | 61.8 MB | 1271 | 23.976 | 53.01 s | ADX 6ch/48k | 52.99 s |
| `movie/CS0082.usm` | 37.0 MB | 759 | 23.976 | 31.66 s | ADX 6ch/48k | 31.63 s |
| `movie/CS0083.usm` | 71.7 MB | 1476 | 23.976 | 61.56 s | ADX 6ch/48k | 61.52 s |
| `movie/CS00EP.usm` | 60.1 MB | 1239 | 23.976 | 51.68 s | ADX 6ch/48k | 51.40 s |
| `movie/CS00P1.usm` | 24.8 MB | 507 | 23.976 | 21.15 s | ADX 6ch/48k | 21.15 s |
| `movie/CS00P2.usm` | 10.7 MB | 226 | 23.976 | 9.43 s | ADX 6ch/48k | 11.01 s |
| `movie_logo/Activision.usm` | 8.3 MB | 451 | 59.94 | 7.52 s | ADX 6ch/48k | 7.54 s |
| `movie_logo/Nickelodeon.usm` | 6.7 MB | 222 | 29.97 | 7.41 s | ADX 2ch/48k | 7.41 s |
| `movie_logo/PG5R.usm` | 4.7 MB | 150 | 29.97 | 5.00 s | ADX 6ch/48k | 5.00 s |
| `movie_logo/criware.usm` | 2.7 MB | 150 | 30.0 | 5.00 s | – | – |

Total 800 MB, 697 s of video. Four files (`CS0071A/B`, `CS00P2`, `CS00EP`) have
up to 1.9 s of audio beyond the last video frame — the audio stream is simply
longer than the picture; a player should follow the *video* length.

Nothing is encrypted: no `@ALP`, `@CUE` or `@SBT` chunks appear, and every video
payload starts directly with an MPEG start code.

### 6.3 Demux + transcode proof

```powershell
python tools\usm_demux.py "The Legend of Korra\GameData\movie\CS0041.usm" --out out\usm --json out\usm\CS0041.json
#   @SFA ch0 adx      439,542 B  84 chunks  29.97 fps  2.71 s (codec) -> CS0041.sfa.ch0.adx
#   @SFV ch0 mpeg2  2,714,149 B  65 chunks  23.98 fps  2.71 s (codec) -> CS0041.sfv.ch0.m2v

ffmpeg -i out\usm\CS0041.sfv.ch0.m2v -i out\usm\CS0041.sfa.ch0.adx `
       -c:v libx264 -preset veryfast -crf 23 -pix_fmt yuv420p `
       -c:a aac -b:a 256k -shortest out\usm\CS0041.mp4
```

`ffprobe` on the result:

```
h264, 1280x720, 24000/1001, 65 frames, duration 2.711 s, 678 kb/s
aac,  48000 Hz, 5.1 (6 ch)            duration 2.709 s
mp4 duration 2.711 s
```

Frame count, resolution, channel count and both durations agree with the
container's own metadata, so the demux → transcode path is proven end to end.
ffmpeg decoded the raw ES with zero errors (only "estimating duration from
bitrate", expected for a raw elementary stream).

---

## 7. Tooling: why not CriCodecs

The plan (§4) names `Youjose/CriCodecs` as the CPK/USM tool. It does **not run on
this machine**, for two independent reasons (both reproduced 2026-11-19):

1. **No wheels.** `pip download cricodecs --only-binary=:all:` →
   *"from versions: none"*. PyPI only publishes `cricodecs-1.2.0.tar.gz`, so any
   install compiles the C++ core (pybind11). Not Python-version specific.
2. **The source does not build with MSVC 14.44.** Configure succeeds with
   CMake 4.x + VS 2022 Build Tools (`-G "Visual Studio 17 2022" -A x64
   -DCRICODECS_BUILD_CLI=ON`), the build then aborts on C++23 gaps:
   * `fatal error C1083: 'flat_map': No such file or directory` — this MSVC STL
     install ships neither `<flat_map>` nor `<flat_set>`. (Working around it with
     `std::map`/`std::set` aliases got past this.)
   * `error C3615 / C7595: 'cricodecs::util::exp2' / 'sine_series' constexpr
     function cannot result in a constant expression` in
     `utilities/numeric.hpp` → `hca/hca_tables.hpp`, `hca_transform.cpp`. The
     library's `consteval` trig fallback is not MSVC-constexpr-clean; its static
     HCA tables need constant evaluation.
   * `wav_container.cpp: constexpr float_to_pcm16` also fails (uses `isfinite`).

   MSVC 14.51 (the reference toolchain in TOOLCHAIN.md) is untested for this;
   a clang/LLVM build would likely work (the code paths are libc++-shaped).

So the verification in this document uses our own stdlib-only readers, which
were checked *against* the CriCodecs source semantics (`crilayla.cpp`,
`cpk_reader.cpp`, `usm_container.hpp`) and against the data itself: every
`ExtractSize`, every `bad_padding_chunks == 0`, and every payload magic agree.

**If a real CriCodecs is wanted later**, the cheap routes are (a) build it with
clang-cl/LLVM, or (b) use the vgmstream CLI for audio only. Neither is blocking.

---

## 8. Reproduce

```powershell
$py = "%LOCALAPPDATA%\Python\pythoncore-3.15-64\python.exe"
$g  = "The Legend of Korra\GameData"

# manifest (no extraction, < 0.5 s per archive; data001 is mmap'd, 1.6 GB stays on disk)
& $py tools\cpk_manifest.py "$g\data000.cpk" --json data000.json --markdown data000.md
& $py tools\cpk_manifest.py "$g\data001.cpk" --json data001.json --markdown data001.md

# single-entry extraction / CRILAYLA proof
& $py tools\cpk_extract.py "$g\data000.cpk" --out out\cpk --smallest 12
& $py tools\cpk_extract.py "$g\data000.cpk" --out out\cpk --path core/coreeff.dat

# one movie, demuxed and transcoded
& $py tools\usm_demux.py "$g\movie\CS0041.usm" --out out\usm
```

`--manifest-only` prints `offset size extract_size path` for every entry;
`--dry-run` on the demuxer scans a movie without writing streams.

---

## 9. What this means for the port

* **M2.10 (CPK file layer)** needs *nothing* from this document except paths:
  the host only has to serve `GameData\*.cpk` byte ranges through
  `CreateFileA`/`ReadFile`/`SetFilePointer`. The lifted CRI File System code
  parses the UTF tables and calls the CRILAYLA decoder itself — so no host-side
  decompression is required. Verified prerequisite: the archives are plaintext
  (`CpkMode` quirk aside) and their TOC is parseable with big-endian UTF parsing.
* **M2.12 (USM movies)** can stay "skip" for the gate, but the cheap version is
  now known: demux → decode MPEG-1 + ADX. ADX is the same codec family the game
  already decodes for its streams, so the audio side is not new work.
* **M3 (install from own copy)** should plan for **2.05 GB extracted** or
  **1.87 GB** if the CPKs are copied as-is. Copying the CPKs verbatim is the
  better default: it keeps names/offsets intact and avoids a 1,213-file tree.
* **Asset work later** (higher-res UI, cutscene re-encode) only needs
  `tools/cpk_extract.py` + `tools/usm_demux.py`; nothing here is committed.

---

## 10. Not verified / open questions

* `CpkMode = 1` semantics (parsing works; meaning unknown).
* Per-entry `CRC` algorithm (not needed for the port).
* The `DAT\0` sub-file table layout is only sketched (4CC + name + length); the
  `u32 [5]` section-offset fields were not fully decoded.
* `CS0071A/B`, `CS00P2`, `CS00EP`: reason for the audio/video length mismatch.
* Whether the game plays the movies through Sofdec (in-engine, `@SFV`) or drops
  to an external MPEG path — decides how much of M2.12 is real work.
* CriCodecs on MSVC 14.51 / clang not attempted beyond the failures in §7.
