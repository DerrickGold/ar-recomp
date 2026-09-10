# Bundled Noto fonts

`NotoSans-SemiCondensedExtraBold.ttf` is the default Latin, Greek, and Cyrillic
face for localized game text. It is a broad Latin/Greek/Cyrillic face, not a
universal Unicode font: a pack in another script must supply its own font.

The settings overlay owns an independent stack using this face plus the
Japanese fallback below. Its English bitmap lettering stays native; non-ASCII
runs are shaped together at output resolution and cached. Selecting a game
language pack never replaces the interface's fonts.

Provenance:

- family: Noto Sans SemiCondensed ExtraBold
- font version: 2.008
- upstream: `notofonts/noto-fonts`
- upstream commit: `ffebf8c1ee449e544955a7e813c54f9b73848eac`
- upstream path:
  `hinted/ttf/NotoSans/NotoSans-SemiCondensedExtraBold.ttf`
- SHA-256:
  `c3c65645ed2c76892b0bf72c714783885117f2aa4695d89e87ea0b2f1c8798fe`
- license: SIL Open Font License 1.1; see `OFL.txt`

The font is not relicensed under the project's code license. OFL 1.1 permits
the unmodified font to be bundled and redistributed with the application when
the copyright notice and license accompany it.

This one face covers the official English, French, and German requirements and
provides broad Latin, Greek, Cyrillic, Vietnamese, phonetic, punctuation, and
combining-mark support. It is intentionally not described as a universal
Unicode font. Noto is a coordinated family collection, and language packs add
script-specific faces to the ordered fallback stack when needed. Japanese,
Chinese, and Korean coverage is not promised by the primary face. The interface
ships the Japanese face below to support its official-release languages;
game packs still declare their own required script-specific dependencies.

The semi-condensed extra-bold silhouette was selected to preserve the compact,
heavy proportions of ActRaiser's retail 8x8 font. The game's blue/white banded
style is applied by the renderer to the glyph alpha mask; it is not a modified
font file.

## Japanese interface fallback

- file: `NotoSansJP-Bold.otf` (unmodified upstream Japanese subset OTF)
- upstream: `notofonts/noto-cjk`
- revision: `f8d157532fbfaeda587e826d4cd5b21a49186f7c`
- path: `Sans/SubsetOTF/JP/NotoSansJP-Bold.otf`
- SHA-256: `1b0edfb500b73a4fa8a4fcaae1bbbd403994e08e73e3e0da37e70d3853f42c5f`
- license: SIL Open Font License 1.1, `NotoSansJP-OFL.txt`; original copyright
  and author information are retained in the font's name table.

The same bytes are retained in the Unicode regression fixtures. This licensed
host font is not an extraction from the Japanese game ROM and does not include
retail text or artwork. It is not a universal Unicode font.
