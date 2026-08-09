# Action-background HLE evidence census

Living evidence ledger for `SPEC-bg-hle.md` BH1/BH2. This separates what the
provider has actually matched from what remains an architectural expectation.

## Ordinary act-entry matrix — 2026-08-09

Command:

```sh
python3 tools/bg_hle_matrix.py
```

The runner uses the verified raw warp table from `docs/manual.md`, stages each
target from the replay's transition-capable world-map window at game frame 400,
and captures game frames 900 and 1200. Every child run uses a generated partial
`settings.ini` with Diorama off and vertical extension zero. This isolation is
load-bearing: an earlier probe accidentally inherited the developer's live
Diorama setting, whose headless PPM is only the residual/HUD plane even though
the tile-word comparator remains valid.

Inputs and output contract:

- ROM SHA-256:
  `b8055844825653210d252d29a2229f9a3e7e512004e83940620173c57d8723f0`;
- replay: `saves/fillmore-act-2.rec` (used to reach the transition-capable
  world-map window and provide deterministic post-entry input);
- runtime observer: `AR_ACTION_BG_HLE_COMPARE=1`, rendering still native;
- two full WRAM/VRAM/CGRAM/OAM/PPU snapshots per target;
- one authentic 256x224 flat framebuffer per target;
- machine-readable local manifest:
  `runs/bg-hle-matrix-20260809-114024.json`.

| Target | Scene | BG1 at entry | BG2 at entry | Runtime tile comparisons |
| --- | --- | --- | --- | ---: |
| `0101` | Fillmore act 1 | eligible | eligible | 2,177,811 |
| `0102` | Fillmore act 2 | eligible | eligible | 2,181,020 |
| `0201` | Bloodpool act 1 | eligible | native 32x32 decorative layer | 841,609 |
| `0202` | Bloodpool act 2 | eligible | eligible | 2,172,373 |
| `0301` | Kasandora act 1 | eligible | eligible | 2,200,655 |
| `0303` | Kasandora act 2 | eligible | disabled at both samples | 1,077,828 |
| `0401` | Aitos act 1 | eligible | native 32x32 decorative layer | 1,068,029 |
| `0404` | Aitos act 2 | eligible | eligible | 1,910,425 |
| `0501` | Marahna act 1 | eligible | eligible | 2,200,578 |
| `0504` | Marahna act 2 | eligible | disabled at both samples | 1,084,990 |
| `0601` | Northwall act 1 | eligible | native 32x32 decorative layer | 1,071,428 |
| `0605` | Northwall act 2 | eligible | native 32x32 decorative layer | 1,086,077 |

Totals:

- 12/12 target runs passed;
- 19,072,823 authentic runtime tile-word comparisons, zero mismatches and zero
  unexpected finite-world exits;
- 24 PPU-complete snapshots and 43,999 independent offline ring comparisons,
  zero mismatches;
- zero wrong-mode, invalid-source, allocation, or comparison failures;
- expected fail-closed states: 7,752 layer-frames during load force-blank,
  2,370 disabled-layer frames, and 4,439 native-tilemap frames;
- every two-snapshot map/table/descriptor set is stable (one exact map hash,
  definition hash, and descriptor variant per target/layer);
- 12 valid 256x224 framebuffers, 12 distinct hashes. The 4x3 contact sheet was
  visually inspected in target order and every entry shows its intended stage.

All twelve BG1 entry layers are world-provider eligible. Six BG2 layers are
eligible. Four entry BG2 layers explicitly expose a 32x32 PPU tilemap and remain
native/decorative policy, consistent with the existing mirror/repeat path. A
disabled BG2 sample is not a permanent native-only classification: Kasandora
act 2 and Marahna act 2 still require later-room/transition captures before the
scene plan can say the layer never activates.

## What this does not prove

This is an entry census, not the complete BH1 gate:

- the warp is a real game transition from non-action state, but deterministic
  replay input after entry was recorded for Fillmore; this is visual/source
  evidence, not a gameplay acceptance run for every target;
- each target samples the early room at two times. Mid-level BGSC handoffs,
  bosses, narrow mixed-policy scanline bands, and ending transitions remain;
- Northwall boss target `0608` and Death Heim `$07/$01-$08` remain uncaptured;
- the framebuffers are authentic flat composites, but individual priority-plane
  captures and deliberate visual positive controls remain open;
- full-level rasterisation from resident WRAM still needs CHR-aware visual
  review outside Fillmore;
- no HLE tile source is bound and no margin lookup is displayed.

The provider seam therefore remains default-off/unimplemented. The evidence is
enough to proceed with special-room census work, not enough to claim BH1/BH2 or
delete any legacy background path.
