# Noto Sans bundled primary font

`NotoSans-SemiCondensedExtraBold.ttf` is the default Latin, Greek, and Cyrillic
face for localized game text and the host overlay.

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
Chinese, and Korean fonts are especially large and will be packaged as
optional script assets rather than imposed on every base install.

The semi-condensed extra-bold silhouette was selected to preserve the compact,
heavy proportions of ActRaiser's retail 8x8 font. The game's blue/white banded
style is applied by the renderer to the glyph alpha mask; it is not a modified
font file.
