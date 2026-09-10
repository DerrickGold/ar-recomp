# Unicode fallback acceptance fonts

These are self-contained language-pack dependencies, never silently selected
from the operating system. The Japanese face is also bundled for the independent
system-interface font stack; the Arabic face remains test-only. Both remain
under the SIL Open Font License 1.1; the corresponding
license texts are stored beside the binaries in `fonts/`.

## Japanese

- file: `fonts/NotoSansJP-Bold.otf`
- upstream: `notofonts/noto-cjk`
- revision: `f8d157532fbfaeda587e826d4cd5b21a49186f7c`
- path: `Sans/SubsetOTF/JP/NotoSansJP-Bold.otf`
- SHA-256: `1b0edfb500b73a4fa8a4fcaae1bbbd403994e08e73e3e0da37e70d3853f42c5f`

## Arabic

- file: `fonts/NotoSansArabic[wdth,wght].ttf`
- upstream: `google/fonts`
- revision: `45b0855d499c093e4d1bd08926fec4e1a582e225`
- path: `ofl/notosansarabic/NotoSansArabic[wdth,wght].ttf`
- SHA-256: `63111b5b2e074dd48cc67692e0a2726d86ee94c1c37fe8598257b7b4e87e869e`

The Japanese face proves real primary-to-CJK fallback. The Arabic face proves
whole-run complex shaping, RTL direction, and mixed Arabic/European-digit
layout. Their catalogue is intentionally partial so the overlay tests also
exercise English string fallback in the same frame.
