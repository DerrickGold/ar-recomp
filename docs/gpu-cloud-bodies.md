# Continuous GPU globe clouds — 2026-09-12

Status: default navigation and Sky Palace path after the shared mountain source
(`84fbb39d`). The existing `AR_SIM3D_WORLD_GPU_GRID=0` selects complete compatibility;
there is no additional shipping experiment switch or saved setting. Existing
cloud visibility, opacity, drift, altitude and quality/effect settings still apply.

## Rendering and ownership

The original 48-ring, 96-sector camera-tangent cap retains its radius, altitude,
limb-opacity profile, diagonal and three baked cloud banks. One compact source
contains unit directions plus opacity: 18,432 vertices, 288 KiB. Its shape changes
only with shell radius or eye distance. Camera orientation, viewport, world
orientation and wind use copied transforms, not CPU projection, clipping or UV
streams. A bounded, grow-once CPU staging buffer services shape changes. The
existing helper pool remains intact for terrain, models and compatibility work.

Texture directions are transformed in the vertex shader, perspective interpolated
and normalized in the fragment shader. Continuous spherical coordinates avoid
interpolating across longitude seams or texture poles; moving CPU chart splits
are unnecessary. This is deliberately **not bit-identical** to the old affine
per-vertex texture approximation. It preserves art and sampling resolution,
rather than replacing the cloud atlas or reducing shell density.

The private depth-pass value contract adds a spherical-body mesh kind. One of
the existing 16 effect handles holds the shared source. Three ordered samples
use the existing Cloud atlas, linear sampler, alpha discard, depth test and
no-depth-write material. Opacity remains screen-linear across hardware clipping.
The shader defines the degenerate zero-direction case rather than normalizing
zero. Immutable embedded MSL/SPIR-V/DXIL blobs require no runtime compiler.

Update copies input before returning; Ready survives viewport/camera changes.
Reset invalidates storage but leaves the caller's handle valid for republishing,
including lazy pipeline recreation. Queued updates reject; destroying a queued
handle invalidates the pass. All source/sample budgets remain bounded. Append
validates the entire sample group before queueing anything: matrices, finite
overflow bounds, unit normals/rotations, orthonormal bases, atlas extents and
opacity. Ordinary Cloud geometry cannot mix with retained samples. Optional
setup failure logs once, latches until reset and uses complete compatible clouds,
not authentic scene fallback or partially duplicated cloud banks.

No game state, clock, scene identity, native resource handle or borrowed pointer
crosses the adapter boundary. No runner ABI, render-device vtable, fence, readback,
compute queue or multicore setting changes.

## Correctness

All 165 local CTests pass (30.53 seconds). The subsequent expanded depth fixture
also passes: 36 independent analytic cases cover seams/poles **inside** faces,
wind rotations, offsets, atlas-bank selection, perspective W, viewport changes,
opaque occlusion and resource reset. Separate checks cover ordered near/far
transparency without depth writes, copied lifetime, invalid/overflowing inputs,
atomic 64-sample capacity and queued destruction. The world fixture exercises
the default body path across its existing 21 geography/light/relief/Palace/
navigation/viewport/Advent states, cold/warm and zero/three helpers.

Full-game frozen-cloud comparisons keep strict failures and pixel histograms:
two local Palace images differ by at most two channel levels; 16 moving/held
navigation images differ by at most two, except three pixels in one frame at
three levels. No geometry/art changes are visible at normal size. Final WRAM
matches in every pair. Two further Palace captures show the final default build
is byte-identical to the inspected prototype (including the staging reuse and
degenerate-direction guard). These are not claims of universal pixel identity.

The actual Deck/Vulkan depth fixture passes too. D3D12 blobs compile offline;
Windows runtime validation is unavailable.
Two Deck Palace captures (1080x672, GF600/900) differ by at most two channel
levels, with 9,377/9,392 changed pixels out of 725,760 and matching final WRAM.
Their strict byte-comparison failures and histograms are retained as well.

## Repeated performance

CPU values are non-overlapping render wall scopes, not GPU timestamps or visible
FPS estimates. Mac runs use pinned release binaries, Quality, three helpers,
2160x1344 Metal and serial ABBAABBA, with no own build, test, capture or image
analysis overlapping timing. Summaries weight settled windows by frames before
taking run medians. Every replay completes and final WRAM matches.

| Scene / host | Control CPU, ms [range] | GPU bodies CPU, ms [range] | Reduction | Depth upload MiB/present |
| --- | --- | --- | --- | --- |
| Palace / Mac, 8 runs | 2.810 [2.767–2.839] | 2.610 [2.599–2.625] | 7.1% | .740 → .065 (−91.3%) |
| Navigation / Mac, 8 runs | 2.132 [2.071–2.229] | 1.969 [1.898–1.993] | 7.6% | 1.482 → .570 (−61.6%) |
| Palace / Deck, 4 runs | 4.982 [4.950–5.014] | 3.902 [3.886–3.918] | 21.7% | .737 → .063 (−91.5%) |
| Navigation / Deck, 4 runs | 4.216 [4.214–4.218] | 2.968 [2.950–2.986] | 29.6% | 1.410 → .594 (−57.8%) |

GPU work increases: two additional draws (Palace 26→28, navigation 25→27),
continuous per-fragment mapping and roughly 3–5% more submitted vertices because
the GPU clips the complete shell. No GPU elapsed-time or power savings are
claimed. Local cadence remains approximately 8.34 ms (120 Hz); Deck hidden
cadence changes Palace 6.211→5.112 ms and navigation 5.451→4.179 ms. Scene/window
mix changes with throughput in navigation, so its differing vertex counts are
not an exact-work GPU comparison. Palace source shape is held; navigation still
republishes the compact source when the cap shape changes.

The Deck uses bundled SDL/Wayland/Vulkan at 1080x672. Two warmup runs precede
timing. The first 1,000-tick Palace cohort is retained but **not used for a CPU
percentage**: candidate runs finish before enough settled reporting windows.
The valid repeat uses 1,600 ticks; navigation uses 2,000. All probes keep the
8-GiB start/6-GiB abort RAM floor, 2-GiB GPU-growth bound, 20-second fixture and
45-second game guard with bounded process-group termination. No guard fires,
no source rejection or authentic fallback occurs, and installed game/settings/
saves are untouched. GPU allocation growth is approximately 242 MiB in timed
Palace runs for both binaries (includes driver cycling, not just mesh storage).

## Evidence and remaining work

Local: `/private/tmp/actraiser-gpu-cloud-body.izvDMV/`. Deck:
`/home/deck/argame/cloud-body-probe-20260912.PfiPXt/`. The existing
`tools/compare_pipeline_performance.py` uses config
`/private/tmp/actraiser-deck-profile.2Y9hIv/benchmark-defaults.ini` and manifest
`/private/tmp/actraiser-world-surfaces.NDSzGz/checkpoints.json`. Palace runs 1,800
ticks with `AR_REPLAY_NOSTOP=1`; navigation runs 2,000. The first Palace cohort
uses the prototype switch in that pinned binary; the final binary needs none.

- Mac control: `266cd89cf194941321c30daa2054f9def2731eea1b88e6adf6fc1f18e9bc73c7`.
- Mac prototype: `926bf8c5edc6e60ea82498fb14a9ea602344bef626867327d08047678fe91f07`.
- Mac final: `731f169807e7a9d48fdd2b2a137b9b17eab4641bda5b39757ce704d5e75092ea`.
- Linux control: `66c7d103f4a93e64c80176e3adc0a44da282d8a948a0b986541004b9de62d81c`.
- Linux candidate: `4a2c57818f59a11e238714439c344a06126005e64dde77b2fa8c31b0b11de4c0`.
- Linux final depth fixture: `ec93e183eae2cc453e4b4d25926a905d568776a899957892aa465b8e45cb1e70`.

Palace volume slices still upload a small sorted stream, and atmospheric halo
geometry stays in the ordinary 2D pass. Native/composite texture uploads and
their exact change-detection scans are independent remaining costs. These
results do not establish that all remaining optimizations are below 1%.
