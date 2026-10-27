# Marahna Act 2 environment pass

Implemented 2026-10-27 in `action-effects.ini` for US, JP and EU terrain.
These are ordinary editable recipes, gated by the existing Environmental
effects setting. No new shader, texture asset, render pass or readback was added.

| Room | Treatment |
| --- | --- |
| 05:04 | Cool temple fill, warmer shrine torches, light in the two lower window recesses, dust and low floor wisps. |
| 05:05 | Silver lattice-window spill, warm torches, a dim descent, and mist at the base of the spike pit. |
| 05:06–07 | Shared world positions for the arch gallery, lattice openings and inner sanctuary; localized dust, supported wet ledges and short drips. |
| 05:08 | Warm light around the original gold relief, cooler side windows and thin arena-floor mist. Existing Viper lightning also illuminates both actor classes. |

## Authoring and rendering

- All new positions use BG1/world coordinates. The window, torch and ambient
  lighting explicitly enable scenery, player and enemy receivers. Dimming uses
  those same three receiver classes.
- Window recipes reuse the castle family, with Marahna's own frame/sill tile
  witnesses and raw metatile material words. The light does not require a BG2
  plane or a visible moon. Generic soft lights provide reflected fill in front
  of the openings.
- The torch field retains native source identity `54000000`, intensity 4 and
  reach 2. Only its spill radius, spill strength and receiver mask change;
  existing per-torch edits and ember phases keep their identities.
- Floor wisps occupy selected supported spans. In room 05:05 they sit below the
  spikes, including EU's differently flagged lower spike tiles. Wet highlights
  sit on exposed platform tops; drips do not pass through collision surfaces.
- The 05:06/07 shared map keeps the same light positions. Fog and dust remain
  sparse enough to leave its carvings visible.
- Copied castle fields now validate their member edits against the destination
  definition. Drag, resize and rotation no longer require a native Bloodpool
  source count. Torch members still reject rotation; invalid member references
  and edits in the wrong room/profile are rejected atomically.
- Whole-project capacity is 1024 records / 512 KiB in native, WASM and ZIP
  import/export. This pass uses 777 records / 274196 bytes. The existing 96
  emitters per room/profile and 4096 authored vertices per batch remain intact.

## Validation

- Native release game and standalone WASM editor rebuilt successfully.
- Native recipe, capacity and copied-window tests pass in the Debug test
  configuration. The new copied-member test also runs before the legacy oracle
  in optimized builds.
- The existing optimized castle bit-hash oracle fails on this Mac with the
  unmodified HEAD test as well (`f516dfea` vs expected `d6a30baa`). This pass does
  not change that oracle or its rendering code.
- Real WASM tests cover all five rooms in all three regional profiles: window
  capture on actual room materials, scenery/player/enemy lighting, editable
  catalogue counts, exposed wet ledges, unobstructed drips, supported floor
  wisps, timeline scrubbing and texture reuse. Existing editor interaction and
  project ZIP tests also pass, including the larger shipped document and the
  exact capacity boundary.
- Native GPU comparison: all 135 room/camera/background/motion combinations
  render. Retained cold/warm draws, fade restoration and invalid-slot fallback
  match exactly. The strict CPU/GPU two-value tolerance still reports tiny
  color differences: at most four pixels per 720×448 frame, each differing by
  at most four channel values. This is not a bit-exact CPU/GPU parity claim.
- A 1300-frame isolated native-game run loaded all 777 records, entered 05:04
  and completed without projection fallback, GPU metadata downloads or late
  shader compilation after boot preparation.

The local review and test artifacts are in `runs/marahna-act2-20261027/`.
`marahna-act2-preview-640x360.mp4` is a 24-second shared-renderer camera tour with
the player stand-in, not a recording of boss gameplay. It is approximately 1 MB.

The 1080p Mac timing check uses the optimized native shared renderer, two runs
before and two after per scene, 300 frames per run with the first ten excluded.
It skips screenshot readback. CPU submission and completion after a GPU-idle
wait are measured separately in `benchmark.json`; neither measures display
latency or native-game frame pacing. No Steam Deck/D3D12 performance claim is
made from these numbers.

Apple M2 / Metal, 1920×1080, final definitions (milliseconds, paired-run means):

| View | CPU before | CPU after | CPU + GPU wait before | CPU + GPU wait after |
| --- | ---: | ---: | ---: | ---: |
| Shrine | 2.307 | 2.346 | 4.683 | 4.374 |
| Gallery | 3.146 | 3.327 | 5.335 | 5.885 |
| Viper arena | 3.158 | 3.075 | 5.103 | 5.088 |

The gallery shows the clearest additional cost: about 0.18 ms CPU and 0.55 ms
including the completion wait. Small gains in the other samples are run
variation, not an optimization claim.
