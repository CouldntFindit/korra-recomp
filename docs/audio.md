# Audio — Wwise stack inventory & M4 host-mixer spec

**Status:** Track A complete (2026-11-19) · feeds **M4** (host mixer behind the
XAudio2 COM shim) and the M2.9 silent stub.
**Inputs read (read-only, nothing extracted to the repo):**
`GameData\sound\` (783 files, 171.6 MB), `LoK.exe` string table (FNV name
recovery), `data000.cpk` TOC strings (bank discovery only).
**Tools:** `tools/audio_inventory.py` (ours, stdlib-only), `vgmstream-cli`
(decode oracle, v2026 build), `wwiser` (HIRC parser, v20260808).

Convention below: **[V]** = verified against the shipped data in this session,
**[H]** = hypothesis, **[?]** = open.

---

## 1. Verdict for M4

1. **All shipped audio is Wwise 2013.1/2 content** — 779 `.wem` files, two banks
   (`Init.bnk`, `BGM.bnk`), one event table (`WwiseInfo.wai`), one Platinum
   footstep table (`SeFootstep.bin`). **[V]**
2. **There is no CRI audio content.** No `.hca`, `.adx`, `.acb`, `.awb` anywhere
   in `sound\`; only Wwise Vorbis/ADPCM/PCM. The `CRI NcVoice XAudio2 1.04.01 +
   HCA-MX` strings in `LoK.exe` (§2 of the plan) are **linked but unused for
   content** — so M4 needs **no HCA decoder**. (CPK interior still to be
   confirmed with Track B — see §8.) **[V for `sound\`]**
3. **Music is streamed with bank prefetch**: `BGM.bnk`'s 120 media entries are
   byte-identical *prefixes* (~35–55 KB ≈ 1 s) of the real files in
   `stream\*.wem`; only the one in-memory source is complete. Any host that
   serves bank media without the stream files will play ~1 s of every cue. **[V]**
4. **Recommended M4 route: keep the engine's own Wwise runtime** (it is lifted C
   like everything else) and implement the **XAudio2 COM object** it creates
   through `CoCreateInstance`. The engine decodes Vorbis/ADPCM and mixes; the
   host only provides a sink. Re-implementing Wwise (route B) is now *possible*
   because the bank format and the event graph are mapped here, but it is a much
   larger job than M4 needs. **[H — recommendation, cost-based]**

---

## 2. What ships

| Path | Files | Bytes | Audio | Codec | Notes |
|---|---:|---:|---:|---|---|
| `sound\Init.bnk` | 1 | 9,496 | — | — | global bank: buses/FX/states, **no events, no media** |
| `sound\bgm\BGM.bnk` | 1 | 5,231,414 | ≈120 s (prefetch heads) | Vorbis | 120 media = byte-prefixes, + full HIRC event graph |
| `sound\stream\*.wem` | 189 | 146,240,119 | 4,161.3 s (69.4 min) | Vorbis ×187, IMA ADPCM ×1, PCM16 ×1 | music, ambience, stingers |
| `sound\stream\English(US)\*.wem` | 590 | 20,110,682 | 2,095.8 s (34.9 min) | Vorbis | voice-over, all 32 kHz |
| `sound\WwiseInfo.wai` | 1 | 51,636 | — | — | 2,581-entry event table (§5) |
| `sound\SeFootstep.bin` | 1 | 1,108 | — | — | 44 footstep selection rules (§6) |
| **total** | **783** | **171.6 MB** | **6,257 s (104.3 min)** | | |

Channel / rate spread of the 779 `.wem` **[V]**

| Bucket | Channels | Rates | Codecs |
|---|---|---|---|
| `stream\` (189) | 1 ch ×2, 2 ch ×55, **4 ch ×50, 5 ch ×79, 6 ch ×3** | 48 kHz ×130, 32 kHz ×59 | Vorbis 187, IMA ADPCM 1, PCM16 1 |
| `stream\English(US)\` (590) | 1 ch ×580, 2 ch ×10 | 32 kHz ×590 | Vorbis 590 |

* The 4/5/6-channel files are **surround music** (quad / 5.0 / 5.1). Wwise
  downmixes to whatever output the host declares — declaring **2.0** keeps the
  Android path simple and is what the port should do first. **[V content, H mixer]**
* Voice is one language folder only (`English(US)`), mono, 32 kHz, 0.5–26 s
  (median 3.0 s). Language switching is therefore a no-op for this build. **[V]**
* 5 of 779 files disagree with vgmstream on duration (it reports 0–0.6 s for
  them); by data-size math they are ~10–30 s, so this table uses the computed
  value. The other 774 match vgmstream within 2 %. **[V]**

Decode proof (samples actually decoded, not just parsed):

```
vgmstream-cli -m  stream\82801749.wem      -> 16-bit PCM, 2ch, 48 kHz, 25.000 s
vgmstream-cli -m  stream\1012706428.wem    -> Wwise Vorbis, 4ch, 48 kHz, 94.884 s
vgmstream-cli -m  stream\English(US)\266085530.wem -> Vorbis, 1ch, 32 kHz, 3.755 s
vgmstream-cli -m  bgm\BGM.bnk              -> 120 subsongs, each ≈1 s, 5ch 48 kHz
```
WAV decode of the same files succeeds (`-o out.wav`), i.e. the containers and the
Vorbis setup headers are intact and fully decodable outside the game.

---

## 3. Containers

### 3.1 `.wem` — RIFF + Wwise codec tag **[V]**

Plain little-endian RIFF/WAVE. `RIFF size + 8 == file size` for all 779 files
(no truncation). Chunk sets seen:

| Chunk set | Count | Meaning |
|---|---:|---|
| `fmt `, `data` | 669 | plain |
| `fmt `, `data`, `cue `, `LIST` | 103 | has marker/loop table (music) |
| `fmt `, `data`, `smpl` | 5 | loop points |
| `fmt `, `JUNK`, `data` | 2 | alignment padding |

`fmt ` uses **Wwise's own tag values**, not the Microsoft ones:

| Tag | Count | Codec |
|---|---:|---|
| `0xFFFF` | 777 | **Wwise Vorbis** (custom, `"Custom Vorbis"` in vgmstream) |
| `0xFFFE` | 1 | PCM16 (`82801749.wem`, 2ch/48 kHz/25 s) |
| `0x0002` | 1 | **Wwise 4-bit IMA ADPCM** (`1055586713.wem`, 2ch/48 kHz/25 s) |

`fmt ` layout for the codec-tagged files (fmt size 0x42 Vorbis / 0x18 IMA):

```
+0x00 u16  wtFormatTag   0xFFFF Vorbis | 0xFFFE PCM16 | 0x0002 IMA
+0x02 u16  channels
+0x04 u32  sampleRate     32000 or 48000 here
+0x08 u32  avgBytesPerSec
+0x0C u16  blockAlign     0 for Vorbis, 0x48 (72) for stereo IMA
+0x0E u16  bitsPerSample  0 for Vorbis, 4 for IMA
+0x10 u16  cbSize         0x30 for Vorbis
+0x14 u32  (4 observed)
+0x18 u32  total sample frames   <-- Vorbis: matches vgmstream exactly
+0x20 u32  data-chunk size (duplicate)
```
IMA frames are derived from block geometry, not that field:
`frames = (data / blockAlign) * ((blockAlign - 4*channels) * 2 / channels)`
(= 1,200,000 frames for the shipped file ⇒ 25.000 s). **[V]**

Consequence for the host: **three decoders** are theoretically needed if the host
decoded audio itself (Vorbis, IMA-ADPCM, PCM16). Under the recommended route the
engine decodes all three, so the host sees only PCM.

### 3.2 `.bnk` — chunked bank **[V]**

```
"BKHD" u32 size  u32 generatorVersion(0x58) u32 bankID u32 languageID ...
"DIDX" u32 size  N × {u32 mediaID, u32 offset, u32 size}   (offsets into DATA)
"DATA" u32 size  raw media bytes, 16-byte aligned gaps
"HIRC" u32 size  u32 objectCount, then {u8 type, u32 sectionSize, payload}
"STID" u32 size  u32 ?, u32 count, {u32 bankID, u8 len, name[]}
"STMG" u32 size  global settings (volume thresholds, state groups)
"ENVS" u32 size  legacy environment/obstruction curves
```

* Generator version **0x58 (88) = Wwise 2013.1/2** (wwiser's table; same family
  as Bayonetta 2 / Devil's Third). **[V]**
* **Bank IDs are `FNV-1` 32-bit of the lowercase bank name** — verified twice:
  `fnv1("init") = 1355168291` = `Init.bnk` id, `fnv1("bgm") = 412724365` =
  `BGM.bnk` id. This trick also unlocks event names (§7). **[V]**

**`Init.bnk`** — 9,496 B: `BKHD(32) STMG(1998) HIRC(7322) ENVS(112)`.
80 HIRC objects, all global: **7 State, 47 Bus, 4 FxShareSet, 14 FxCustom,
8 AuxBus**. Zero events, zero media. It is the bus/FX/state graph the rest of
the mix hangs from — the engine loads it first and everything else references it.

**`BGM.bnk`** — 5,231,414 B: `BKHD(24) DIDX(1440) DATA(5090336) HIRC(139558) STID(16)`.
1,456 HIRC objects:

| Object | Count | Role |
|---|---:|---|
| Event | 295 | the music events (§4) |
| Action | 695 | 187 Play, 183 Stop, 155 SetState, 102 SetSwitch, 38 Pause, 12 Resume, 10 SetGameParameter, 6 SetAkProp, 2 UseState |
| MusicTrack | 192 | each exactly 1 source; plugin `0x00040001` Vorbis ×190, `0x00020001` ADPCM ×1, `0x00010001` PCM ×1 |
| MusicSegment | 187 | 1.0–104.3 s, median 20.7 s, 5,740 s total |
| MusicRanSeqCntr | 69 | playlists |
| MusicSwitchCntr | 18 | adaptive layers driven by game-sync |

**Prefetch evidence (important):** of 120 DIDX media, **119 have a RIFF header
that claims more bytes than the bank stores**, and for all **95** that also exist
as `stream\<id>.wem` the bank copy is a **byte-identical prefix** of the file
(`tools/audio_inventory.py prefix`). Cross-checked against `StreamType`:

| StreamType | tracks | media | in DIDX | complete in bank |
|---|---:|---:|---:|---:|
| 0 in-memory | 15 | 1 | 1 | **1** |
| 1 streaming | 48 | 45 | 15 | 0 (prefixes) |
| 2 prefetch-streaming | 129 | 119 | 119 | 0 (prefixes) |

So the single complete media is the in-memory one (`95719221`, 338 B, 0.1 s) and
everything else in `DATA` is a prefetch head. Because the prefix is byte-identical
to the file prefix, a host may either serve the bank copy and continue at
`offset = bankSize`, or ignore the bank copy and read the whole file — both
produce the same audio.

### 3.3 Music event → media chain **[V]**

```
Event (id, N actions)
  └─ ActionPlay.idExt ──► MusicSwitchCntr ─┐
                          MusicRanSeqCntr ─┤─► Children[] ─► MusicTrack
                          MusicSegment ────┘                 └─ AkBankSourceData
                                                              ├─ ulPluginID (codec)
                                                              ├─ StreamType (0/1/2)
                                                              └─ sourceID == media id
```
Numbers: 295 events → **187 Play references** over **86 distinct targets** (51
segments, 60 playlists, 76 switch containers, counting multiplicity) → 192 tracks
→ **150 distinct media**. 125 of those 150 exist in `stream\`; 74 are reachable
from an event (the rest are alternative switch/playlist branches).

Nothing in this chain has to be re-implemented by the host: the lifted Wwise code
does this walk itself once the banks load. It is documented here so the event
names/IDs in §4 can be trusted, and so route B (§9.2) has a spec if it is ever
needed.

---

## 4. Bank → event map (music, `BGM.bnk`)

* 295 events; **183 carry at least one Play action** (187 Play actions over 86
  distinct targets; 4 events have 2 targets), **112 are pure control events**
  (SetState / SetSwitch / Stop / Pause — adaptive-music switching, no audio of
  their own). 175 events reach ≥1 media; 138 of those have a length that can be
  measured from local files. **[V]**
* **97 of 295 names recovered** from `LoK.exe` literals via `FNV-1(lower(name))`
  (§7). The remaining 198 are unnamed in this build's string table (built at
  runtime, or posted by ID from data tables).
* **18 game-sync groups** drive the switch containers; only one is named in the
  exe: `bgm_sync_ssbgm1`. Values include `NORMAL`/`Normal` and `Prologue`
  (recovered by hash). These are what the game posts during play — useful as M5
  hooks when checking "is the right cue playing". **[V ids, H meaning]**
  *Refined in §12.2d: the switch containers use **14 distinct** groups (18
  argument instances), none named; `bgm_sync_ssbgm1` is one of the 11 global
  **state** groups from `Init.bnk`, not a switch group.*

### 4.1 Named music events

`media` = distinct media reachable from the event; `len` = sum of those
media's lengths on disk. `media 0` rows are control-only events
(SetState / SetSwitch / Stop / Pause) with no audio of their own.
Rows with `media >= 1` but `len 0.0` point at a media that is only a bank
prefetch head (no `stream\<id>.wem`), so no local length exists.

| event name | short ID | media | length s |
|---|---|---:|---:|
| `AvatarState_FadeOut_01` | 1671504648 | 0 | 0.0 |
| `AvatarState_FadeOut_02` | 1671504651 | 0 | 0.0 |
| `AvatarState_FadeOut_03` | 1671504650 | 0 | 0.0 |
| `AvatarState_FadeOut_04` | 1671504653 | 0 | 0.0 |
| `AvatarState_FadeOut_05` | 1671504652 | 0 | 0.0 |
| `AvatarState_FadeOut_Reset` | 3082003462 | 0 | 0.0 |
| `bgm_Bending1_Water` | 2021308404 | 1 | 2.5 |
| `bgm_Bending2_Earth` | 334121622 | 1 | 2.8 |
| `bgm_Bending3_Fire` | 336903273 | 1 | 2.8 |
| `bgm_Bending4_Air` | 3598504040 | 1 | 2.9 |
| `bgm_ChapterResult_c8` | 3713340630 | 1 | 10.0 |
| `bgm_ChapterSelect_Shop_enter` | 115085201 | 0 | 0.0 |
| `bgm_ChapterSelect_Shop_exit` | 833732749 | 0 | 0.0 |
| `bgm_Continue` | 1163414739 | 2 | 87.8 |
| `bgm_Continue_GameOver` | 3281798590 | 1 | 5.6 |
| `bgm_Continue_NagaRush` | 2137131987 | 2 | 87.8 |
| `bgm_Continue_Shop_enter` | 1564199325 | 0 | 0.0 |
| `bgm_Continue_Shop_exit` | 3734852561 | 0 | 0.0 |
| `bgm_Continue_Yes` | 4267379007 | 0 | 0.0 |
| `bgm_Dieing_cancel` | 2477027635 | 0 | 0.0 |
| `bgm_Dieing_enter` | 2654706393 | 0 | 0.0 |
| `bgm_Dieing_exit` | 1665573141 | 0 | 0.0 |
| `bgm_FallDown_enter` | 4036729748 | 0 | 0.0 |
| `bgm_FallDown_exit` | 2963669494 | 0 | 0.0 |
| `bgm_JetStreamNaga_Scene_03` | 1119773661 | 0 | 0.0 |
| `bgm_JetStreamNaga_Scene_04` | 1119773658 | 0 | 0.0 |
| `bgm_Levelup` | 3367338161 | 1 | 4.7 |
| `bgm_Levelup_Nightmare` | 894971183 | 1 | 4.7 |
| `bgm_MissionResult` | 817876405 | 1 | 0.0 |
| `bgm_MissionResult_NagaRush` | 2709486549 | 0 | 0.0 |
| `bgm_MissionResult_NagaRush_Nightmare` | 1328926131 | 0 | 0.0 |
| `bgm_MissionResult_Nightmare` | 3986204627 | 1 | 0.0 |
| `bgm_MissionResult_PBL` | 3816327818 | 0 | 0.0 |
| `bgm_MissionResult_PBL_Nightmare` | 2057356244 | 0 | 0.0 |
| `bgm_Movie_enter` | 3674116419 | 0 | 0.0 |
| `bgm_Movie_exit` | 3194762787 | 0 | 0.0 |
| `bgm_NagaRush_CheckPoint` | 3022586002 | 1 | 0.0 |
| `bgm_NagaRush_Stop_Clear` | 3405522030 | 0 | 0.0 |
| `bgm_NagaRush_Stop_Miss` | 3000273819 | 0 | 0.0 |
| `bgm_Nightmare_FadeOut` | 276370560 | 0 | 0.0 |
| `bgm_PBL_FaceOff` | 3483098677 | 1 | 25.6 |
| `bgm_PBL_FaceOff_OpeningCamera` | 1829480389 | 0 | 0.0 |
| `bgm_PBL_Flash` | 3507850641 | 0 | 0.0 |
| `bgm_PBL_KnockOut` | 1180291533 | 0 | 0.0 |
| `bgm_PBL_Omake_Final` | 984554699 | 0 | 0.0 |
| `bgm_PBL_Omake_Normal` | 1980665308 | 0 | 0.0 |
| `bgm_PBL_OpeningCamera` | 514578334 | 1 | 19.5 |
| `bgm_PBL_OpeningCamera_Epilogue` | 1459756357 | 1 | 25.0 |
| `bgm_PBL_OpeningCamera_Skip` | 3485335788 | 0 | 0.0 |
| `bgm_PBL_Result_Skip` | 3857578996 | 0 | 0.0 |
| `bgm_PBL_Round` | 3034137611 | 1 | 0.0 |
| `bgm_PBL_RoundsOver` | 1249935540 | 0 | 0.0 |
| `bgm_PBL_TimesUp` | 408137602 | 0 | 0.0 |
| `bgm_PBL_YouLose` | 584248649 | 1 | 6.8 |
| `bgm_PBL_YouWin` | 3849305908 | 1 | 16.0 |
| `bgm_PausedCancel_enter` | 594713711 | 0 | 0.0 |
| `bgm_PausedCancel_exit` | 189932023 | 0 | 0.0 |
| `bgm_Paused_enter` | 34902601 | 0 | 0.0 |
| `bgm_Paused_exit` | 91636901 | 0 | 0.0 |
| `bgm_Slow_enter` | 2918756080 | 0 | 0.0 |
| `bgm_Slow_exit` | 3152952186 | 0 | 0.0 |
| `bgm_Sync_Pause` | 803349658 | 0 | 0.0 |
| `bgm_Sync_Resume` | 2335748301 | 0 | 0.0 |
| `bgm_Title` | 737535402 | 1 | 21.2 |
| `bgm_TitleMenu` | 60855595 | 1 | 49.7 |
| `bgm_Title_exit` | 4244263889 | 0 | 0.0 |
| `bgm_cs11_skip` | 1835020578 | 1 | 0.0 |
| `bgm_cs12_skip` | 2840658039 | 1 | 53.3 |
| `bgm_cs21_skip` | 583381957 | 1 | 86.7 |
| `bgm_cs22_skip` | 2000770892 | 0 | 0.0 |
| `bgm_cs31_skip` | 3924820156 | 1 | 8.3 |
| `bgm_cs32_skip` | 2088883189 | 1 | 0.0 |
| `bgm_cs33_skip` | 3178288570 | 1 | 16.9 |
| `bgm_cs41_skip` | 1165345047 | 1 | 94.9 |
| `bgm_cs42_skip` | 159707842 | 0 | 0.0 |
| `bgm_cs43_skip` | 841887965 | 1 | 0.0 |
| `bgm_cs44_skip` | 1017461536 | 1 | 15.6 |
| `bgm_cs51_skip` | 3522596790 | 0 | 0.0 |
| `bgm_cs52_skip` | 1405174571 | 1 | 0.0 |
| `bgm_cs54_skip` | 541021269 | 0 | 0.0 |
| `bgm_cs55_skip` | 1630323482 | 1 | 0.0 |
| `bgm_cs56_skip` | 2099763567 | 1 | 18.5 |
| `bgm_cs61_skip` | 2473772425 | 1 | 0.0 |
| `bgm_cs62_skip` | 3959347968 | 0 | 0.0 |
| `bgm_cs63_01_stop` | 1592259554 | 0 | 0.0 |
| `bgm_cs63_02_play` | 3504518085 | 1 | 53.9 |
| `bgm_cs63_skip` | 1019321779 | 1 | 53.9 |
| `bgm_cs71a_skip` | 1611014247 | 1 | 27.4 |
| `bgm_cs71b_skip` | 1254583890 | 1 | 90.5 |
| `bgm_cs72_skip` | 1075947257 | 1 | 39.1 |
| `bgm_cs81_skip` | 2362927259 | 1 | 0.0 |
| `bgm_cs82_skip` | 1269389222 | 1 | 18.8 |
| `bgm_cs83_skip` | 177527121 | 0 | 0.0 |
| `bgm_csp1_skip` | 2244919171 | 1 | 0.0 |
| `bgm_csp2_skip` | 4040808526 | 0 | 0.0 |
| `bgm_pend` | 407420911 | 0 | 0.0 |
| `bgm_pstart` | 1551659340 | 0 | 0.0 |

Verified count: **97 named events** (of 295). The six `AvatarState_*` rows are
the only non-`bgm_` names in this bank. Regenerate with the commands in §10;
do not hand-edit this table.

---

## 5. `WwiseInfo.wai` — the master event table **[V]**

```
+0x00 u32  magic 0x20140808   (reads as a 2014-08-08 build stamp) **[H]**
+0x04 u32  0
+0x08 u32  record size = 20
+0x0C u32  record count = 2581
+0x10      2581 × { u32 id, f32 value, u32 flagA, u32 flagB, u32 flagC }
```
`16 + 2581*20 = 51,636` = exact file size; IDs are **ascending** (a binary
search table, which is how the engine uses it).

* It contains **all 295 `BGM.bnk` events** — so it is the game-wide event list,
  not a music-only table. The other 2,286 events belong to banks that are **not**
  in `sound\` (see §8).
* `value`: `0.0` or a round number of seconds (30, 40, 50, 70, 80, 100, 110, 150,
  210, 250, 410). Best reading: **cue/loop length in seconds**, 0 = unknown or
  not applicable. **[H]**
* `flagA`: `1` for all 1,303 rows that have a non-zero `value`, plus 886 rows
  with `0.0`; `0` for 392 rows, all of which have `value == 0`. So `flagA == 0 ⇒
  value == 0` (necessary, not sufficient) — it looks like a class bit ("this id
  has audio"), with `value` the known length. **[H]**
* `flagB` ∈ {0 ×2007, 0x200000 ×570, 0x100000 ×4}, `flagC` ∈ {0 ×2552,
  0x30000 ×15, 0x10000 ×14} — too rare to be a length/streaming rule; treat as
  category bits until the engine's lookup is read. **[H — meanings unproven]**
* Names are **not** stored in the file (no ASCII at all). They are recoverable
  from the executable — see §7.

Named-entry categories of the 977 resolvable names:

| prefix | count | reads as |
|---|---:|---|
| `cp*` | 366 | checkpoint / script cue |
| `pl*` | 132 | player (Korra) movement & bending SFX |
| `*_vo_*` | 102 | voice-over |
| `bgm_*` | 91 | music |
| `em*` | 72 | enemy SFX |
| `se_*` | 28 | UI / system SFX |
| other | 186 | props, ambience, stingers |

---

## 6. `SeFootstep.bin` — Platinum footstep selector **[V layout, H meaning]**

1,108 B, magic `"sfs\0"`:

```
+0x00 char[4] "sfs\0"
+0x04 u32  table1 offset = 0x14
+0x08 u32  table1 count  = 2
+0x0C u32  table2 offset = 0x34
+0x10 u32  table2 count  = 44

table1: 2 × 16 B { u32 a, u32 mask, u32 unk, u32 count }
    { 0x00010000, 0xFFFF0000, 0x0020, 14 }
    { 0x00020000, 0xFFFF0000, 0x0160, 30 }      <- 14 + 30 = 44 = table2 count

table2: 44 × 24 B { u32 value, u32 mask, u32 selector, u32 0, u32 0, u32 wem_id }
    e.g. { 0x00010000, 0xFFFF0000, 0x51, 0, 0, 0x2CC97CFE }
```
`value`/`mask` form a (value, mask) pair in three groups that match table1's split:

| `value` | `mask` | records | selector range |
|---|---|---:|---|
| `0x00010000` | `0xFFFF0000` | 14 | `0x00`–`0x0C` |
| `0x00025000` | `0xFFFFF000` | 15 | `0x50`–`0x52` |
| `0x00023000` | `0xFFFFFF00` | 15 | `0x50`–`0x52` |

So the lookup is `(id & mask) == value` over a 32-bit surface/character id, with
`selector` (0x00–0x52, 83 values) picking a variant inside the group; the last two
words are always zero.

**11 distinct `wem_id`s** are the footstep sounds (`0x2CC97CFE`, `0x48896D1C`,
`0x2FE99727`, `0x2A7DF2DD`, `0x2EF661D4`, `0xFD3D2ED9`, `0x6ED0F683`, `0x7AAB588A`,
`0x9E3C3DFA`, `0xB003822A`, `0x40DCC1A1`) and **none of them is in
`stream\`**, so they live inside `core.bnk` in the CPK (§8). `0x48896D1C` is the
default (12 of 44 rules). This file is a
Platinum gameplay table read by the lifted engine code — the host does nothing
with it, but it proves the SFX media live in the CPK-hosted bank.

---

## 7. Recovering Wwise names from the exe **[V]**

Wwise object IDs are `FNV-1` 32-bit over the **lowercase** name:

```python
def fnv1(name):
    h = 0x811C9DC5
    for c in name.lower().encode():
        h = ((h * 0x01000193) & 0xFFFFFFFF) ^ c
    return h
```

Verified on bank IDs (`init` → 1355168291, `bgm` → 412724365). Applying it to
every printable string in `LoK.exe` names **977 of 2581** events and **97 of 295**
music events. This is how the tables above were produced and it is mechanical —
re-run with:

```powershell
python tools\audio_inventory.py wai GameData\sound\WwiseInfo.wai `
    --names "The Legend of Korra\LoK.exe" --csv work\wai_events.csv
python tools\audio_inventory.py names "The Legend of Korra\LoK.exe" --ids work\ids.txt
```
(The exe itself is never copied or committed — only the resulting hashes/names.)

---

## 8. What is *not* in `sound\` (hand-off)

* **`core.bnk` exists inside `data000.cpk`** — found by scanning the CPK TOC
  string table (33 `.usm` occurrences and one `.bnk` name in the first 4 MB; the
  bank name is `core.bnk`). It is almost certainly the gameplay bank that owns the
  remaining ~2,286 events (SFX, footsteps, most voice) — the `SeFootstep.bin`
  media IDs and the 25 bank-only music media should be there too.
  **→ Track B: please publish the CPK manifest, and if convenient extract just
  `core.bnk` (a few MB) for `docs/assets.md` neighbours.**
* 25 `BGM.bnk` media have **no** `stream\` file (prefetch head only). Presumably
  `stream\<id>.wem` equivalents inside the CPK, or unused branches.
* 64 `stream\*.wem` files are **not** referenced by any `BGM.bnk` track — they
  belong to events in the CPK-hosted bank (ambience, stingers, cutscene beds).
* `data001.cpk` (1.6 GB) TOC was not located in the first 4 MB; nothing about it
  was assumed here.

---

## 9. What the M4 host mixer must implement

### 9.1 Route A (recommended): keep Wwise, provide XAudio2

The engine creates its audio device through **COM**, not through an import: the
2014 triage (§2 of the plan) found no `XAudio2_*.dll` import and flagged
`ole32!CoCreateInstance` as the instantiation path. So M4 = a COM server.

**Resolved (§12.2a): the binary asks for the XAudio2 2.7 pair** —
`CLSID_XAudio2 {5a508685-a254-4fba-9b82-9a24b00306af}` (and its `_Debug`
variant), `IID_IXAudio2 {8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb}`, all three present
in `.rdata`; the 2.8 IID is absent. The 2.7 vtable has three extra head methods,
see the layout in §12.2a - laying it out from a 2.8 header shifts every call by
three slots. Still log every call by vtable index at M2.9 to pin the voice
interfaces.

Minimum surface to expect from Wwise 2013 + CRI NcVoice (verify each by logging
the vtable index on first run rather than guessing):

| Interface | Methods to implement for real |
|---|---|
| `IUnknown` | `QueryInterface`, `AddRef`, `Release` (refcount must be correct — the engine releases in `StopEngine`) |
| `IXAudio2` | `Initialize`, `GetDeviceCount`, `GetDeviceDetails`, `CreateMasteringVoice`, `CreateSourceVoice`, `CreateSubmixVoice`, `StartEngine`, `StopEngine`, `CommitChanges`, `RegisterForCallbacks`, `UnregisterForCallbacks`, `SetDebugConfiguration` |
| `IXAudio2MasteringVoice` | `GetVoiceDetails`, `DestroyVoice`, `SetVolume`, `SetOutputMatrix`, `GetChannelMask`, `GetState` |
| `IXAudio2SourceVoice` | `Start`, `Stop`, `SubmitSourceBuffer`, `FlushSourceBuffers`, `Discontinuity`, `ExitLoop`, `GetState`, `SetFrequencyRatio`, `GetFrequencyRatio`, `SetVolume`, `SetChannelVolumes`, `SetOutputMatrix`, `SetSourceSampleRate` |
| `IXAudio2VoiceCallback` | **`OnBufferEnd` is load-bearing** — Wwise re-arms the next buffer from it. `OnStreamEnd`, `OnBufferStart`, `OnLoopEnd`, `OnVoiceProcessingPassStart/End`, `OnVoiceError` may be no-ops. |

Format rules the sink must honour:

* Accept the format the engine asks for; do not force one. Expect **16-bit PCM
  or float32**, mono→5.1, **32 kHz or 48 kHz** (`WAVEFORMATEX`/`EXTENSIBLE` in
  `CreateMasteringVoice`/`CreateSourceVoice`).
* Declare **2.0 at 48 kHz** on the mastering voice first: the engine then
  downmixes the 5.1/quad music itself and the Android sink stays trivial. **[H]**
* `SubmitSourceBuffer` buffers are 10–21 ms slices of already-decoded PCM. A
  ring buffer of **3–4 buffers** covers Android scheduling jitter without
  audible latency.
* `GetState`/`OnBufferEnd` ordering must be honest: reporting a buffer free
  before it has been consumed causes dropouts, reporting late causes stalls.

### 9.2 Route B (fallback): HLE the Wwise runtime

Only if the lifted Wwise code cannot be made to run. Everything needed is now
documented: bank chunk layout (§3.2), object graph (§3.3), event table (§5),
codecs (§3.1). A minimal implementation needs:

1. `Init.bnk` bus graph (47 buses / 8 aux / 18 FX = 4 share sets + 14 custom) —
   or ignore buses and mix flat.
2. `BGM.bnk` HIRC parse → event → action → track → media (as in §3.3).
3. **Vorbis decoder** (libvorbis already ships in the vgmstream bundle we used as
   an oracle, but on Android it must be vendored: `libvorbis`+`libogg`, ~200 KB).
4. IMA-ADPCM + PCM16 paths (2 files only — could be skipped with a warning).
5. Prefetch handling: read the bank prefix, continue at `offset = prefixSize` in
   `stream\<id>.wem` (or just read the whole file).
6. The 2,286 events in `core.bnk` still need the CPK-hosting bank.

Estimated real cost: weeks. Route A is days. **Choose A unless M2 proves the Wwise
runtime itself is unmappable.**

### 9.3 Sink

| Platform | API | Notes |
|---|---|---|
| Windows (M2 validation) | WASAPI shared-mode, or `waveOut` for the first frame | silent stub first (M2.9), real sink at M4 |
| Android (M4) | **AAudio** (API 26+), low-latency/exclusive first, fallback to shared | 48 kHz, 2 ch, 16-bit or float; buffer 2×256 frames ≈ 10.7 ms; `Oboe` if AAudio misbehaves |
| thread model | one output thread owned by the sink; engine calls arrive on its own thread | the ring buffer is the only shared state; no locks inside `SubmitSourceBuffer` |

### 9.4 Acceptance tests for M4

Every ID below is from the verified table in §4.1 / §5.

1. **Silence is not success.** With the M2.9 stub the game must run; with the M4
   sink, `KORRA_AUDIO_DUMP=out.wav` must produce a non-silent file (RMS > -40 dBFS).
2. Menu music: post `bgm_TitleMenu` (60855595) → 49.7 s cue, plays to its end.
3. Longest cue: post `bgm_cs41_skip` (1165345047) → 94.9 s, proves a 5.1 source
   streams and downmixes to 2.0 without gaps.
4. One-shot over a running cue: `bgm_Levelup` (3367338161) → 4.7 s stinger, the
   underlying cue keeps playing (two concurrent voices on the same bus).
5. Control-only event must **not** silence anything: post `bgm_Sync_Pause`
   (803349658) then `bgm_Sync_Resume` (2335748301) → music pauses/resumes in sync.
6. Voice: `korra_vo_cont_yes` (14045797, from §5) → 32 kHz mono line plays once
   the CPK file layer (M2.10) serves the bank that owns it.
7. Log and check: every `CreateSourceVoice` format, 0 `OnVoiceError`, buffer
   underruns < 1/min.

---

## 10. Reproduction recipes

```powershell
# inventory (no third-party packages)
python tools\audio_inventory.py scan  "The Legend of Korra\GameData\sound" --json work\audio_scan.json
python tools\audio_inventory.py bank  "The Legend of Korra\GameData\sound\Init.bnk" "The Legend of Korra\GameData\sound\bgm\BGM.bnk"
python tools\audio_inventory.py prefix "The Legend of Korra\GameData\sound\bgm\BGM.bnk" --stream "The Legend of Korra\GameData\sound\stream"

# decode oracle (download once, keep out of the repo)
vgmstream-cli.exe -m  "…\sound\stream\<id>.wem"
vgmstream-cli.exe -o  out.wav "…\sound\stream\<id>.wem"

# HIRC graph (download once)
python wwiser.py -d xml -dn bgm.xml "…\sound\bgm\BGM.bnk"
```

**Dead ends, recorded so nobody repeats them:**

* `pip install cricodecs` **fails** on this machine: no wheel for Python 3.15 and
  the source build needs CMake + Windows SDK (both absent). Not needed — all
  shipped audio is Wwise, and CriCodecs targets CRI formats that do not occur
  here. If it is ever needed, build it in a Python 3.12 venv with CMake installed.
* `pip install wwiser` — not on PyPI. Run it from the GitHub zip (`wwiser.py`).
* Reading the `.wai` `value` field as a duration only works as a *cue length*
  hint; do not use it to size PCM buffers.

---

## 11. Open questions

1. **`core.bnk`** (in `data000.cpk`): size, event count, whether it uses the same
   Wwise version. Blocks the other 2,286 events → Track B.
2. Do the 25 bank-only music media (the `stream\` files that do not exist) exist
   inside the CPK as `<id>.wem`?
3. `.wai` flag bits (`flagB`/`flagC`) — unproven meanings; only matters if the
   engine's lookup is reimplemented (route B).
4. Does the engine create **more than one** mastering voice (e.g. a CRI one for
   `NcVoice`, never used because it has no content)? Check the COM call log during
   M2.9 — if a second device is created and never fed, stub it immediately and log it.
   *(The CLSID/interface family is settled in §12.2a — 2.7, normal + debug; the
   "how many devices" half still needs the runtime log.)*

---

## 12. Second-session verification (2026-11-19)

This section was added by a **second, independent** session before M4 starts
using this document. Nothing above was edited except three pointers in §11.
Method differs on purpose - own HIRC resolver over a `wwiser -d xml` dump, own
RIFF/DIDX/`.wai` parsers, `vgmstream-cli r2168` as the only decode oracle - so
that agreement is evidence and not a copy.

Units: MiB = 1,048,576 B (the per-folder table in §2 is MiB; the 171.6 MB total
there is the same folder in decimal MB = 163.7 MiB, which is why the two do not
add up at first glance).

### 12.1 Reproduced exactly (independent run)

| Claim from §2-§7 | This session's value |
|---|---|
| `sound\` contents | 783 files / 171,644,455 B; 779 `.wem`, 2 `.bnk`, 1 `.wai`, 1 `.bin` |
| `.wem` codecs | 777 Wwise Vorbis, 1 IMA-ADPCM (`1055586713`), 1 PCM16 (`82801749`) |
| `.wem` rates / channels | 32 kHz x649, 48 kHz x130; 1ch x582, 2ch x65, 4ch x50, 5ch x79, 6ch x3 |
| channel masks | 0x4 x582 (voice), 0x3 x65, 0x33 x50 (quad), 0x3b x79 (5.0), 0x3f x3 (5.1) |
| `BGM.bnk` HIRC | 1,456 = 295 Event / 695 Action / 187 Segment / 192 Track / 69 RanSeq / 18 Switch |
| `Init.bnk` HIRC | 80 = 47 Bus / 8 AuxBus / 4 FxShareSet / 14 FxCustom / 7 State, no events, no media |
| action histogram | Play 187, Stop 183, SetState 155, SetSwitch 102, Pause 38, Resume 12, SetGameParameter 10, SetVolume/ResetVolume 4, Use/UnuseState 2, SetBusVolume 1 |
| graph | 150 distinct media reachable, 125 present in `stream\`, 25 bank-only, 0 Play-only events without media |
| `stream\` root | 189 files, 125 reachable from `BGM.bnk`, 64 unreferenced (as §8 states) |
| prefetch gotcha | 119/120 media over-claim in their RIFF header; **95/95** bank copies are byte-identical prefixes (`bank == file[:bankSize]`), 0 exceptions |
| FNV-1 ids | `init` -> 1355168291, `bgm` -> 412724365, `bgm_sync_ssbgm1` -> 1190818745 |
| names | 978 events named from exe literals (987 if `%d`/`%02d` format strings are expanded, e.g. `HUD_CON_BASE_%02d`); 97/295 music events |
| decode | 7 files decoded to WAV incl. 4ch/5ch music; the IMA and PCM16 25 s files both decode non-silent |

Decoded sample set (peak / RMS of the whole file, 16-bit WAV from vgmstream):

| file | format | peak | rms | note |
|---|---|---:|---:|---|
| `stream\1012706428.wem` | Vorbis 4ch 48k 94.9 s | 0.940 | 0.136 | quad music |
| `stream\English(US)\986060765.wem` | Vorbis 1ch 32k 25.9 s | 0.933 | 0.094 | voice line |
| `stream\English(US)\94870230.wem` | Vorbis 1ch 32k 20.6 s | 0.949 | 0.105 | voice line |
| `stream\1055586713.wem` | IMA-ADPCM 2ch 48k 25.0 s | 1.000 | 0.641 | placeholder cue (below) |
| `stream\82801749.wem` | PCM16 2ch 48k 25.0 s | 1.000 | 0.654 | placeholder cue (below) |
| `stream\English(US)\416126653.wem` | Vorbis 1ch 32k 2.4 s | **0.000** | **0.000** | genuinely silent asset |

### 12.2 New facts and corrections

**(a) The XAudio2 version is 2.7 - GUIDs located in `LoK.exe` [V].** This closes
the `[H until then]` in §9.1 and most of §11 item 4 without runtime logging:

| constant | GUID | file offset |
|---|---|---|
| `IID_IXAudio2` (2.7) | `8bcf1f58-9fe7-4583-8ac6-e2adc465c8bb` | 0x507C50 |
| `CLSID_XAudio2` (2.7) | `5a508685-a254-4fba-9b82-9a24b00306af` | 0x507C60 |
| `CLSID_XAudio2_Debug` (2.7) | `db05ea35-0329-4d4b-a53a-6dead03d3852` | 0x507C70 |

The three sit adjacent in `.rdata`, so they come from one `XAudio2.h` include
(the June-2010 DirectX SDK pair). The 2.8 IID `60d8dac8-5aa1-4e8e-b597-2f5e2883d484`
is **absent**, i.e. no fallback path to a newer interface exists in the binary.

Two consequences the M4 shim must get right:

1. **The 2.7 vtable has three methods that 2.8 does not**: `GetDeviceCount`,
   `GetDeviceDetails`, `Initialize` come *first*, right after `IUnknown`. An
   implementation laid out from a 2.8 header will be shifted by three slots and
   every call lands on the wrong method. Order to implement (§9.1's method list
   already contains all of them, this fixes the layout):
   `[0-2] IUnknown`, `[3] GetDeviceCount`, `[4] GetDeviceDetails`,
   `[5] Initialize`, `[6] RegisterForCallbacks`, `[7] UnregisterForCallbacks`,
   `[8] CreateSourceVoice`, `[9] CreateSubmixVoice`, `[10] CreateMasteringVoice`,
   `[11] StartEngine`, `[12] StopEngine`, `[13] CommitChanges`,
   `[14] GetPerformanceData`, `[15] SetDebugConfiguration`.
2. **Both CLSIDs must be answered.** A debug-engine request uses
   `CLSID_XAudio2_Debug`; return the same object (and log which one was asked).

**(b) `CoCreateInstance` on a stock Windows 10/11 will probably fail.**
`CLSID_XAudio2` (2.7) is only registered when the DirectX June-2010 runtime is
installed; 2.8/2.9 are registered under *different* CLSIDs that this binary does
not ask for. Expect `REGDB_E_CLASSNOTREG` on a clean machine. So M2.9/M4 should
**not** plan on "real XAudio2 does the work on Windows" - the Windows host needs
the same `CoCreateInstance` hook as Android, and both then share one mixer. This
also makes the M2.9 silent stub and the M4 sink the *same* code path with a
different output stage. [H on the registry outcome, V on the GUIDs]

**(c) `Init.bnk` global settings (STMG) - numbers for the mixer budget [V].**

| field | value | why it matters |
|---|---|---|
| `fVolumeThreshold` | -80 dB | voices below this are culled by the engine, not by the host |
| `maxNumVoicesLimitInternal` | **256** | upper bound of concurrent voices the graph can ask for; size ring buffers/pools for this |
| `ulNumStateGroups` | 11 | global state groups (§12.2d names one) |
| transitions | 9 explicit, defaults 1000 ms, one group 3000 ms, one 0 ms | music bed crossfades are engine-side |

**(d) Correction to §4: `bgm_sync_ssbgm1` is a State group, not a switch group.**
`fnv1("bgm_sync_ssbgm1") = 1190818745`, which is one of the 11 STMG *state*
groups above (it is the group driven by the `bgm_Paused_*` / `bgm_Sync_*`
events). The music **switch** containers use **14 distinct game-sync groups**
(18 argument instances across 18 containers), and **none of them is named** in
the exe. So: 14 unnamed switch groups + 1 named state group, not "18 named-by-one
groups". The state *values* that do resolve are the ones already listed in §4:
`Normal` (1160234136), `Prologue` (4203473902), `ON` (1651971902),
`OFF` (930712164), `None` (748895195).

**(e) The two non-Vorbis 25 s files are a matched placeholder pair [V].**

| media | codec | track | segment | event |
|---|---|---|---|---|
| `1055586713` | IMA-ADPCM 4-bit | 260164643 | 2169841 (25.000 s) | 4198189333 (unnamed) |
| `82801749` | PCM16 | 933817339 | 38066807 (25.000 s) | 823604040 (unnamed) |

Both segments are isolated (no parent container), both tracks are the only
source of their segment, and both files are 25.000 s of a loud (RMS -3.9 dBFS)
tonal signal. That is a shipped dev/test cue pair, not production music. If M4
hears exactly these two, the music data is being followed literally - it is not
a decoder bug.

**(f) `stream\English(US)\416126653.wem` is a silent asset, not a broken one.**
528 B, 2.407 s, Vorbis, every decoded sample is 0 (peak 0.000). Do not use it as a
decode smoke test, and expect it in the "silence" acceptance case - the same
applies to the 14 events in `BGM.bnk` that carry zero actions.

**(g) Small count deltas, for anyone comparing runs.** Media-reachable events
come out as **183** here (every event with a Play action resolves at least one
media id; none resolves only to bank-only media), vs 175 in §4 - the difference is
reachability through non-default switch branches. Both agree that 0 Play events
are media-less. Names: 978/987 above vs 977 in §7 (one string accepted by the
`%d`-expansion pass). Neither changes any M4 decision.

**(h) The state objects carry the mix overrides [V].** The 7 `CAkState` objects
are volume/pitch overrides, including a **-96 dB** state (the mute/pause state
behind `bgm_Paused_enter`) and -12/-6 dB duck states. That is how the music
"pause" works - as a state, not as a voice stop - which is exactly the behaviour
acceptance test 5 in §9.4 checks.

### 12.3 The 25 bank-only media ids (checklist for Track B)

Media that `BGM.bnk` prefetches but that has **no** `stream\<id>.wem` on disk.
If the CPK manifest finds `<id>.wem` inside `data000/001.cpk`, they are ordinary
streams; if not, they are dead branches and safe to ignore.

```
95719221 343469247 804953904 270223848 742038440 981971968 416888964
550134730 974380557 336134214 1068033235 642122720 751865138 892357526
508409409 639870417 760211043 291869469 1031870517 702230346 698412628
514160359 1015534854 894179784 731157682
```

`95719221` (338 B, 0.1 s) is the one complete in-memory source and the only one
that is not a prefix head. The other 24 are ~34-56 KB heads.

### 12.4 What this changes for M2.9 / M4

* Implement the **2.7** `IXAudio2` layout from §12.2a verbatim, with a
  name-per-slot dispatch table so every call logs `index + name + args`. That
  turns the first M2.9 run into a call trace instead of a debug session, and it
  is where any remaining uncertainty about the voice interfaces is settled.
* Answer both `CLSID_XAudio2` and `CLSID_XAudio2_Debug`; log `QueryInterface`
  IIDs and return `E_NOINTERFACE` for unknown ones (voicing is created through
  `Create*Voice`, so the voice IIDs should never be asked for by name).
* Budget for 256 voices and a -80 dB cull threshold, and expect the engine to
  hand over **already downmixed PCM** - the host never sees Vorbis.
* Silence cases to expect and not chase: event `bgm_Sync_Pause` (control only),
  the 14 zero-action events, media `416126653` (all-zero samples), and the two
  25 s placeholder cues.
* Still open after this session: `core.bnk` contents/size (Track B), the `.wai`
  flag bits, and whether the engine constructs a second (CRI) mastering voice
  that is never fed - all three stay as §11 items 1-4.
