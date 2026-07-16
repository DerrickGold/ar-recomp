# N-x RGBA pipeline — design (2026-07-15)

The general substitution architecture for HD graphics replacement, replacing
paste composition for everything that participates in depth. Doctrine agreed
2026-07-15: **paste for placement, pipeline for depth** — post-flatten host
composition remains permanently correct for re-composition (HUD anchoring +
independent scale) and for topmost static art (screen plane, sharpest at any
output size); everything with priority relationships renders in-pipeline.

## Why

The host-overlay paste model breaks on priority in every direction: BG-vs-BG
tile priority bits, sprites interleaving between BG priority levels (sprite
z 2/6/10/14 against BG 1-15 in this renderer — priority-0 sprites render
BELOW the Mode-7 canvas), and eventually sprite-vs-sprite once OBJ art is
substitutable. Every paste-path scene accrues bespoke caveats (the mode7
`wrap` flag; the rejected world-map OBJ-promotion idea). In-pipeline
substitution settles all interactions with zero per-scene policy: HD pixels
enter the existing per-pixel priority contest at their authentic priority.

## Architecture

Three orthogonal extensions to the scanline renderer (ppu.c):

1. **N-x rendering.** Internal scale N (1/2/4, chosen at PpuBeginDrawing
   time). Each authentic scanline renders N sublines using THAT line's
   register state (HDMA-correct by construction — same trick the frozen
   mode7 sampler proved). Zbuf line buffers and the resolve pass become N-x
   wide; authentic tile sources sample at 1x and replicate; transformed
   sources (Mode 7) sample at fractional matrix increments. With no packs
   loaded this alone ships as "enhanced rendering" (supersampled Mode 7).

2. **RGBA sideband.** A per-line RGBA buffer plus a Zbuf pixel tag marking
   "direct color": the priority word stays authentic, but the low bits
   reference the sideband instead of CGRAM. The resolve/color-math pass
   reads sideband RGB where tagged (SNES color math operates on resolved
   RGB, so math still works). This removes every SNES color limit for
   substituted art: truecolor tiles, per-pixel alpha, no 15-bit
   quantization.

3. **Tile-hash substitution.** Identity = FNV-1a of raw tile bitplanes
   (+ palette group for recolors; census 2026-07-15: only 15/806 tiles are
   multi-palette). Draw-time lookup must NOT hash per fetch: a VRAM-write-
   invalidated cache maps tile VRAM addresses -> pack entries. HD tiles
   occupy exactly their authentic 8x8 game-pixel footprint at N-x density —
   placement, sprite sizing, flips, hitboxes, and all game state untouched.

## Build phases (each gated)

| Phase | Deliverable | Gate |
|---|---|---|
| 1 | N-x pipeline, no substitution | N=1 output byte-identical to current renderer (screenshot + oracle harness); N=4 visual regression; perf measured |
| 2 | Mode-7 override migrated in-pipeline | Title swirl identical to frozen paste version; pasted m7 surface + compositor path deleted; world map now unblocked (sprites resolve above canvas authentically) |
| 3 | RGBA sideband | Synthetic direct-color test tiles resolve with correct priority + color math |
| 4 | Tile-hash cache + pack loader | Pack format frozen only after census breadth (sim/boss/late-game palette-variance data) |
| 5 | Authoring pipeline | census sheets -> paint-over -> pack; docs |

## Widescreen compatibility

The WRAM/VRAM/OAM-level widescreen HLE (margin tilemap refresh, Sky Palace
BG2 synthesis, sprite margin emission, activation-window extension) operates
on emulated state upstream of the renderer and is untouched by N-x — it
fixes what the renderer samples, not how densely. The renderer-level
widescreen features (layer clamp/mirror/repeat, repeat bands, margin gaps,
extra-columns budget, negative-x windows) live inside the loops being
N-x-ified and port as game-pixel-space logic (subpixel x / N); they are
covered by the phase-1 N=1 byte-identical gate, and the N>1 visual
regression matrix MUST include the AR_WS_HEADLESS widescreen scenes (action
margins, Sky Palace, sim towns, HUD split). Known 1x-pitch assumptions to
make N-aware: the bounded-world margin memsets in actraiser_rtl.c write raw
renderBuffer rows, and the host PPM/F2 capture paths assume g_snes_width.

## Risks / open questions

- **Performance**: N=4 is ~16x the fill of a scalar renderer that is fast
  because it is 1x. Measure at phase 1; N=2 default or optimization passes
  are acceptable outcomes. Consider rendering N-x only while a pack/mode7
  entry is active.
- **Palette dynamics** (the one open format question): CGRAM fades,
  hit-flashes, and color cycling recolor tiles at runtime; truecolor art is
  baked. Plan: store reference palette per pack entry, derive a runtime
  modulation from live-vs-reference CGRAM. Needs boss/fade census data
  before freezing — gather via AR_TILE_CENSUS=1 during normal play.
- **Snapshot/savestate**: N-x buffers and sideband are render-only state,
  excluded from PPU_SAVESTATE regions (same as overlay surfaces).
- The frozen mode7 paste path (engine aa39714/6ffdb62) is the reference
  implementation for fractional matrix sampling, wrap semantics, and
  INIDISP handling — port its math, then delete it in phase 2.
