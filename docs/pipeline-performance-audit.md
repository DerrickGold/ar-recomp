# CPU preparation and cross-mode telemetry — 2026-09-11

Follow-up to `sim-town-cpu-gpu-audit.md` and commit `d293adb2`.
User instructions and metric semantics are in `performance-overlay.md`.
The subsequent row-worker/capture-cache pass is documented in
`pipeline-optimization-followup.md`.

## Retained optimizations

- **Sparse town HUD restoration.** The captured HUD rectangle can cover the
  entire 256×224 screen. Pixels without BG3 or promoted objects already contain
  the authentic PPU result, including hardware color math; they now avoid the
  ten-plane rebuild. BG3 winner lookup rejects empty coverage early and stops
  at the first opaque plane in reverse painter order.
- **Decoded town characters and palette.** Changed 4bpp characters decode once,
  not at every canvas occurrence. The additional fixed cache is 64.5 KiB.
  Palette expansion is independent of character indices and reused by cleaned
  terrain metatiles. Flips, transparency, brightness, source revisions, dirty
  rectangles and town-reset ownership remain unchanged.
- **Row-level terrain replacement sources.** Mixed-mask 16-pixel chunks resolve
  ground/bridge source rows once per row, then select by the existing per-pixel
  ownership mask. No new cache, dirty-consumer dependency or worker lifetime.
- **Retained diagnostic panel.** Formatting and glyph submission happen once
  per sample; warm frames draw one premultiplied target. Optional target failures
  use the direct renderer without retrying allocations every frame. Failed
  target-state restoration propagates as a presentation failure.

These do not reduce visual quality, change shaders or add a new compute queue.
Existing 0–3-helper rendering remains available. The panel cache reduces CPU
submission; it is not an async-compute implementation.

## Repeated measurements

Mac `RelWithDebInfo` (`-O2`), actual hidden GPU compositor, immutable copied D7
Aitos SRAM/replay, clouds enabled, three helpers, Unlimited policy. The hidden
swapchain still settles near 120 Hz. These are **CPU wall scope measurements,
not GPU execution or Steam Deck FPS estimates**.

Every comparison uses ABBAABBA (four runs per variant), with no competing build,
test suite or second replay. Per-run values average the last five settled Town
3D reporting windows; the table gives medians of those per-run values. The
windows have approximately 120–122 presents and are not frame-count-weighted.

Concurrent audio work exists in this checkout. To avoid attributing its changes
to rendering, `control`, `hud-only`, `tile-cache` and `candidate` were linked from
identical non-optimization objects, with input hashes verified before/after.
Earlier exploratory mixed-build comparisons are excluded from these claims.

| Comparison | CPU scope | Before median (range), ms | After median (range), ms |
| --- | --- | --- | --- |
| Matched control → all three preparation changes | Render preparation + presentation | 3.6222 (3.6096–3.6566) | 2.9087 (2.7764–2.9623) |
| Same combined comparison | HUD restoration | 1.1147 (1.0912–1.1195) | 0.2191 (0.1966–0.2407) |
| HUD-only → decoded cache | Canvas raster | 0.1745 (0.1741–0.1762) | 0.0933 (0.0818–0.1003) |
| Decoded cache → row sources | Canvas enhancement | 0.5948 (0.5415–0.6229) | 0.5609 (0.5554–0.5633) |

The first row sums non-overlapping PPU, map, metadata, canvas, frame snapshot,
upload and presentation scopes within each run: **19.7% less measured CPU time**.
It excludes emulation, events and waits. The approximately 80% HUD reduction and
47% canvas-raster reduction are stage-specific, not whole-game improvements.

The row-source change's separate whole-render comparison was inconclusive:
2.9340 → 2.9913 ms, with overlapping ranges and variation in unrelated PPU work.
An additional isolated `-O2` CPU refresh benchmark uses real Aitos classification,
synthetic pixels, 200 warmup revisions and 10,000 measured revisions per run.
ABBAABBA gives **0.32724 → 0.27532 ms (15.9%)**, ranges 0.32605–0.32731 and
0.27527–0.27540. All eight ground/atlas hashes match (`c313e8e6e2bf59b0`). This
supports keeping the local simplification, not claiming a proven aggregate gain.

## Observer cost and limitations

Eight logging-versus-Detailed runs put the retained panel's own CPU scope at
**0.00160 ms/present**, per-run range 0.00156–0.00164, including periodic glyph
refreshes. Individual refresh peaks are around 0.12–0.17 ms. This is submission
time, not the GPU cost of blending the panel.

Whole-render medians varied 2.8612 → 2.9566 ms in that comparison, with substantial
variation in unrelated stages. An additional eight Off-versus-logging runs give
whole-process CPU medians 7.199 → 7.144 seconds. Neither supports a precise
whole-frame overhead percentage or a guaranteed sub-1% observer cost on Deck.
Disabling the collector avoids diagnostic clock reads; recording allocates
nothing. Detailed intentionally excludes the legacy action triangle-coverage
estimator, which is materially more expensive.

The 59 CPU stages and ten work counters are application-owned diagnostics.
They do not modify runner ABI, emulated state, render-device vtables, graphics
quality or synchronization semantics. Existing fork/join callbacks publish
atomic timing before their completion semaphore; the owner drains only after
joining. Helpers are parallel wall sums, never added to serial frame time.
The renderer takes a snapshot value and frame-captured overlay setting.

GPU timestamp queries are **not implemented** in the portable renderer contract.
Present/wait can mean VSync, driver blocking or GPU backpressure. Separate audio
callback utilization is also not measured; owner-thread emulation includes
runner/APU work, with the existing APU profiler available for further diagnosis.

## Validation and remaining work

Regression tests cover sparse HUD footprints, decoded pixels against the
original decoder across flips/palettes/brightness/revisions, terrain replacement,
atomic worker recording, stale epochs, sample/context resets, interval overflow,
settings persistence, panel bounds, actual game-font rendering, retained-target
reuse/reset and injected graphics failures. Portable performance sources are
covered by the rendering-boundary checker and negative dependency probes.

All **159 application CTests passed**. The final label changes also passed the
focused overlay and boundary rerun. Five address/undefined-behavior sanitizer
tests passed: metrics/overlay, parallel work, town canvas, background voxels and
PPU render pipeline. SDL/driver leak detection was disabled, so this is not a
leak-check claim. Release and debug builds succeeded; only the existing linker
common-section alignment warning remained.

Nine full-game town composites are byte-identical between matched control and
candidate. The detailed panel was also exercised in actual native/menu, town,
navigation, Sky Palace and action presentations. A 1280×800 software-rendered
shared-font test verifies Deck-sized layout; its synthetic sample is **not a
Steam Deck performance measurement**. Real Metal captures run at 1792×1344.

The split pipeline identifies remaining PPU scanout, emulation, upload and model
projection work; it does not establish that every remaining optimization is
below a 1% cutoff. Model-space instancing/GPU projection still needs immutable
model ownership, bridge contact/depth handling, clipping and pixel rounding.
Blindly dispatching the current cache-backed model loop onto workers is unsafe.
Deck screenshots and run logs can now identify which of these is actually
limiting each mode before adding backend-specific complexity.

## Evidence

Temporary root: `/private/tmp/actraiser-pipeline.ODgRQj/`. It contains pinned
binaries, matched-link and ABBA scripts, per-run logs, `control-candidate.json`,
`hud-only-tile-cache.json`, `tile-cache-candidate.json`, `logging-detailed.json`,
`off-logging.json`, the isolated ground harness/log, visual captures and test
logs. Temporary evidence may be cleaned by the host; regression tests remain
in the repository.

SRAM SHA-256: `26ec2474882a69dff576f518f614a094f58f1428c807faf83230d72fe4c13568`.

| Binary | SHA-256 |
| --- | --- |
| `control` | `3efc03c44b835a8a2ce179aac5d97a00ea76b5e9eb42787a38477921f8eb3bbc` |
| `hud-only` | `1d53a9b84c2515a5d6708ecb5081f14fa796458cad854cfc40bb2bc47dcd099a` |
| `tile-cache` | `78daa90026b13d280e0ec4bbf097654a75ebccba406f7b4124234999001f12bd` |
| `candidate` | `0c0ed872c89b85ccc202f74fc13603c7842b19576f50c492acae850218e8526c` |
| `overlay-final` (observer comparison) | `cb8126dee5a32f2fee4f34733cd3d1733b5feecb533faf7c298efdf4891c0f7b` |
