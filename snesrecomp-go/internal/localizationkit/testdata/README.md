# Native extraction regressions

These compressed JSON fixtures contain invented instruction streams, generated
dictionary/glyph data and synthetic author text only: no retail ROM or prose.
They preserve independently derived regression expectations from the retired
extractor. Normal Go tests read them directly; Python is not needed to generate
test inputs or build profile data.

- `native-reader-regressions.json.gz`: 16,340 record decodes across five
  encodings/profiles; three source-discovery layouts, 539 source mutations,
  three synthetic catalogues and 21 catalogue mutations.
- `destination-regressions.json.gz`: 306 destination/asset/native decompression
  cases, including 36 explicit failures. The invented asset tables include the
  credits sentinel (08/01), after the final boss (07/08). The old incomplete
  credits stub is replaced by the Go ending producer/timing/pixel tests.
- `author-regressions.json.gz`: 18 synthetic author cases. The extra-page fixed
  HUD case and the fixed HUD case with an opaque U+2028 line separator are
  rejected according to the current Go/game presentation contract. These two
  acceptance expectations supersede the retired extractor; original synthetic
  inputs and parse records are retained. Explicit authored breaks must not be
  bypassed by Unicode separators that the font backend now renders as lines.

Add new cases as ordinary Go tests rather than regenerating expectations from
the implementation being tested. Declarative `../data/*-profiles.json` facts are
now Go-owned inputs, not generated from a second extractor. The local five-ROM
gate (`AR_LOCALIZATION_GUI_ROM_ROOT`) rediscovers code/data proofs, checks full
coverage and rejects altered proofs. `AR_AUTHOR_RUNTIME_PROBE` compares packs
directly with the game's C loader; `AR_NATIVE_GRAPHICS_PROBE` compares every
native pixel for both atlases and all credits pages with the game's C decoder.
