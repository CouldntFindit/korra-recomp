# Third-Party Notices

This repository contains only original code plus the credits below. No game
content, and no generated recompiled code, is distributed here.

| Component | Upstream | License | How we use it |
|---|---|---|---|
| pcrecomp | https://github.com/sp00nznet/pcrecomp | MIT | x86-32 → C static recompiler. Pinned at upstream commit `35548b8` with local patches (see `docs/TOOLCHAIN.md`). Runtime headers are vendored by the build, not copied into this repo. |
| nfsu2-recompiled | https://github.com/codepdbh/nfsu2-recompiled | No LICENSE file at time of study; README states MIT for authored code | **Studied only.** Bring-up ladder, headless-validation patterns and the pcrecomp patch series `0001–0014` originate here. Code is not copied wholesale; any reuse is credited here and written for this project. |
| maxpayne2pc-androidevolved | https://github.com/codepdbh/maxpayne2pc-androidevolved | No LICENSE file at time of study | **Studied only.** `desktop/pc_host/` validation pattern, one pcrecomp patch (SEH frames + or/xor conditions). |
| ngbrecomp | https://github.com/gamboozino/ngbrecomp | GPL-3.0 | Methodology, AGENTS workflow and Android arm64 notes (`docs/TECHNICAL.md`). GPL-3.0 → safe to adapt into this GPL-3.0 repo with credit. |
| DXVK (and DXVK-Native) | https://github.com/doitsujin/dxvk | zlib | Direct3D 9 → Vulkan translation on Windows and Android. Built as a dependency; not vendored here. |
| libadrenotools | https://github.com/bylaws/libadrenotools | BSD-3-Clause (see upstream) | Loading Turnip/custom Vulkan drivers on Adreno devices. Submodule/dependency only on a fork by the nfsu2 author. |
| CriCodecs | https://github.com/Youjose/CriCodecs | MIT | CPK / USM / HCA inspection during development (offline tooling, not linked into the game). |
| vgmstream | https://github.com/vgmstream/vgmstream | ISC | Reference decoder used as an **oracle** for Wwise/HCA/ADX output validation. Not linked into the game. |
| Goldberg Steam Emu lineage (gse_fork) | https://github.com/alex47exe/gse_fork | LGPL-3.0 (upstream Goldberg) | Only behavioural reference for the 7-function `steam_api.dll` stub shipped here. Stubs are hand-written. |
| SIMDe | https://github.com/simd-everywhere/simde | MIT | Optional helper if SIMD lowering needs portable intrinsics. |
| Turnip / Mesa | https://gitlab.freedesktop.org/mesa/mesa | MIT | Vulkan driver fallback on Adreno. Shipped as a prebuilt driver blob at build time, not in this repo. |

## Trademarks

The Legend of Korra is a trademark of Viacom/Nickelodeon. The 2014 PC game was
developed by PlatinumGames and published by Activision. This project is an
unofficial interoperability research effort, not affiliated with, endorsed by or
supported by any of them.
