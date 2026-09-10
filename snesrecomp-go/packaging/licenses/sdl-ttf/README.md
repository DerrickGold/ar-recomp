# SDL3_ttf bundled dependency notices

These verbatim notices correspond to external dependencies pinned by the
[official SDL3_ttf 3.2.2 release](https://github.com/libsdl-org/SDL_ttf/tree/release-3.2.2/external).
The applicable SDK's SDL3_ttf `LICENSE.txt` is copied from its hash-verified
redistributable; Steam Deck also retains Valve's Debian copyright file.
Keep this directory when slimming the compiler SDK or sharing a playable copy.

| Notice | Upstream revision and original file |
| --- | --- |
| FreeType-FTL.txt | [FreeType `9973564c`](https://github.com/libsdl-org/freetype/blob/9973564cfa63763a3e4ac67c09147899539b1e07/docs/FTL.TXT) |
| HarfBuzz-COPYING.txt | [HarfBuzz `564bf981`](https://github.com/libsdl-org/harfbuzz/blob/564bf9818a18709776856533829c0c04950773d6/COPYING) |
| PlutoSVG-LICENSE.txt | [PlutoSVG `2983eb69`](https://github.com/libsdl-org/plutosvg/blob/2983eb6919feea272d793bc386384e3f5b97b03c/LICENSE) |
| PlutoVG-LICENSE.txt | [PlutoVG `3e6f922f`](https://github.com/libsdl-org/plutovg/blob/3e6f922f453da1c9e7d1d7f66cac1d9724a18b47/LICENSE) |

Portions of this software are copyright © 2024 The FreeType Project
(https://freetype.org/). All rights reserved. FreeType is used under the
FreeType License, not its alternative GPL terms.

The macOS redistributable is flattened from a universal framework to a dylib;
its library identity and SDL3 reference are relocated and the changed binary
is ad-hoc signed. Its source code is not modified. Platform redistributables
may enable different subsets of these upstream dependencies; all notices are
retained. None of these files are from a game ROM or a user's language pack.
