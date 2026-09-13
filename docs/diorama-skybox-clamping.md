# Finite scrolling skybox bounds

Implemented 2026-09-12; local macOS verification, not Steam Deck timing data.

## Cause and behavior

Fillmore's BG2 camera scrolls at a different rate from BG1. The old skybox
sampled the gameplay capture's shared horizontal margins. At the reproduced
start position, BG1 had 120 pixels available on the left but BG2 had only 60;
the remaining unrendered columns became a black strip in the full-screen sky.

Finite, live-world captured skyboxes now use a fixed-width source window:

`left = clamp(BG camera X + live raster delta - left budget, 0, world width - view width)`

The window stops at either world edge without changing image scale or moving
the gameplay camera/planes. Each row reuses overlapping primary-capture pixels
and resolves only newly exposed edge tiles while that scanline's palette,
character data and scroll state are still live. There is no second PPU scanout.

## Boundaries

- The generic runner service is an appended, size/capability-checked API entry.
  Existing scanout requests/results and API offsets are unchanged. Caller-owned
  view buffers are validated for capacity and output aliasing, never retained.
- The app publishes the optional surface through FrameSlot. Presentation owns
  upload, initialized texture padding, blur caching and resource reset.
- Interpolation has an independent skybox history slot; the gameplay plane
  count, priority masks and existing frame-generation entry points are unchanged.
- Named-ROM, repeated/mirrored, wrapping and classified/row-banded skyboxes keep
  their existing policies. Unsupported raster features fall back to the normal
  capture; a partially resolved independent view is never published.

## Verification

- Runtime suite: 36/36 tests pass. New pixel comparisons cover left/interior/
  right clamping, raster scroll, brightness/palette changes, flipped tiles,
  transparent fill, and priority separation against an independently positioned
  normal capture. Gameplay/main/priority outputs remain byte-identical.
- API tests cover invalid sizes, pitch/capacity, geometry, aliasing, unsupported
  views, top/bottom scanout rows, and not retaining a previous request's buffer.
- Application suite: 165 tests passed across the normal run and display-enabled
  reruns of five graphics tests initially skipped/blocked by sandbox access.
  The new independent-skybox interpolation case passes on SDL GPU/Metal.
- Fillmore replay: `saves/fillmore-act.rec`, warp 0101 at GF400, snapshot GF500.
  Fixed close view: `runs/20260912-155244/shot.ppm`; fixed zoomed-out view with
  initialized padding: `runs/20260912-155537/shot.ppm`. The left gap is gone.
  Final WRAM matches the pre-fix replay byte-for-byte.

The separate Steam Deck scanout optimization plan remains an investigation;
this correctness fix does not claim a performance improvement.
