# Shared straw-hut refinement — 2026-09-27

Both early-house facings now read as thatched huts. Short, slightly tapered
reed walls support a rounded roof with a closed overhanging underside and an
uneven cut-straw edge. The front's exposed crossed poles have become a bound
straw peak. The alternate retains its broad raised crest and five dark bands.

Warmer ochre walls are distinct from the golden roof. Higher detail levels
add overlapping thatch courses and strands following the roof slope, plus
vertical reed marks on the walls. The crest uses fewer broad faces to leave
room for the roof and wall structure at Low detail. Tile occupancy is unchanged;
the front's taller roof now measures 14.6 authored units, and the alternate
measures 12.8. These bounds include the peak/crest.

Construction stages use short wall posts, an eave binding and rafters, followed
by partial wall and roof covering. Ground contact marks follow the wall/posts
instead of the former ground-level cone perimeter, removing detached dots
outside the hut. Snow uses the existing roof treatment.

- [Before / after, oblique, Low and construction views](../runs/sim-straw-huts-2026-09-27/comparison.png)
- [Refreshed regional art index](../runs/sim-straw-huts-2026-09-27/art-index/index.html)

These are production-renderer diagnostics, not live gameplay captures.
Generated images remain in ignored `runs/`.

Validation: the application builds, and four focused CTests pass (models,
palette, region, biome). Model checks cover thatched roofs, overhangs, no
exposed framing on either finished hut, retained crest bands and relocated
ground contacts. All four detail levels were rendered, with normal/oblique
views and reversed face submission. Across the two huts and four construction
panels, reversal produced only thin boundary differences, with no solid 3×3
changed interiors.

The finished/construction sweeps report zero overflow across 18,816 cases.
Maximum authored faces for these huts are:

| Variant | Low | Balanced | High | Ultra |
|---|---:|---:|---:|---:|
| Front | 54 | 85 | 116 | 147 |
| Alternate | 62 | 93 | 124 | 155 |
| Front construction | 46 | 46 | 46 | 46 |
| Alternate construction | 61 | 61 | 61 | 61 |
