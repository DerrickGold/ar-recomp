# Localization fixture provenance

These compressed JSON fixtures contain invented instruction streams, generated
dictionary/glyph data, and synthetic author text only. They contain no retail ROM
or prose.

- `native-reader-regressions.json.gz` — synthetic record encodings, source
  discovery layouts, and catalogues.
- `destination-regressions.json.gz` — synthetic destination tables, asset data,
  and decompression inputs.
- `author-regressions.json.gz` — independently authored language-pack inputs.

The fixtures retain independently derived expectations from the original
extractor. Go tests read them directly; no retail ROM is required.
