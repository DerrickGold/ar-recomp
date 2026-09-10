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

The Japanese face exercises real primary-to-CJK fallback. The Arabic face
exercises joined shaping and, through the production bidi layout adapter,
explicit Latin/digit/Arabic ordering assertions under auto/LTR/RTL bases,
isolates, hard/soft wrapping and logical versus physical alignment. The tests
add the shipped Hebrew Bold face for exact Hebrew order/pointed-name isolation,
representative Persian/Urdu mixed runs, and CRLF/NEL/line/paragraph separators
without changing logical reveal endpoints. Separators are layout controls, not
missing glyphs; fixed game fields require explicit structural authoring breaks.
The tests also cover numeral slant, palette bands and pixel ownership across fallback
fonts. These are focused regressions, not full game/host RTL qualification or
a substitute for native-speaker visual review. The catalogue is intentionally
partial so overlay tests also exercise English string fallback in the same frame.
