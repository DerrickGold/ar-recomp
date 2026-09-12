# Shared GPU surface prototype — 2026-09-12

Status: **default shared land/cliff renderer** in navigation and Sky Palace.
Ground, blur, haze and cloud-shadow samples share GPU placement, including the
[registered cliff source](gpu-cliff-source.md). The hybrid cliff switch is removed.
`AR_SIM3D_WORLD_GPU_GRID=0` selects the complete compatibility renderer, also used
automatically if source setup fails. Ocean, mountains and cloud bodies remain
on their existing paths. Model offload and multicore support remain available.
The historical experiments/results below predate this default promotion.

## Contract and ownership

`Sim3DDepthPass_Create/Update/AppendSurfaceMesh` owns a copied, camera-independent
quad stream. The scene supplies chart directions, anchor/extra elevations,
source-space lighting normals, color and base UVs. Append supplies a radial
transform, light and optional spherical cloud samples. The renderer knows no
town identity, game clock, map policy, settings or runner internals.

- One source drives Ground and its CloudShadow/blur/haze samples with exactly the same
  homogeneous positions, triangulation and depth mapping. Hardware clips them
  together; shadows test depth but never write it.
- Append validates the entire group before queuing anything. Invalid final
  shadow, exhausted budget or ordinary-shadow conflict cannot strand half a
  surface in the pass. Source updates are forbidden while queued. Reset
  invalidates storage while preserving handles for explicit republication.
- Source survives camera, wind and viewport changes. Those changes need only
  copied uniforms, not new source projection, clipping or geometry upload.
  The caller still owns source revisions, coarse visibility/LOD and fallback.
- The existing four opaque handles, 64 geometry samples, 64 effect samples and
  256-Ki-vertex per-handle bounds are unchanged. Integration must **replace**
  the existing world surface handle, not quietly add a fifth one.
- At the original checkpoint an instance was one original quad: 224 bytes, fourteen float4 attributes,
  six canonical indices and four vertex invocations. Ground and every shadow
  reuse the same buffer. The source buffer ceiling is 14 MiB plus a 14-MiB
  transfer buffer, before backend cycling (screen geometry was 10+10 MiB).
  This tradeoff must be measured in real scenes, not described as free memory.
  The subsequent independent cliff-overlay mask increases the packed layout
  to 256 bytes / sixteen float4 attributes, including when cliffs are disabled:
  16 MiB each for source, transfer and optional selection buffers at the existing
  maximum, before backend cycling. See the cliff report for measured allocation.
- Transform uniforms are 256 bytes, including the optional overlay mask. Shader blobs are generated offline for
  Metal, Vulkan and D3D12; no new runtime compiler, native handle in a public
  contract, runner ABI change, readback, CPU wait or compute queue is added.
- Per-sample transform payloads now share a union. Screen-UV/color transfer
  buffers are only mapped/uploaded when a screen-mesh sample actually needs
  them; spherical/surface-only samples no longer upload unused tint data.

## Vulkan clipping issue found during validation

The previous committed adapter (`ba147864`) failed three existing flat-color
hardware-clipping fixtures on Deck/Vulkan: a quad with W=x+0.25 and Z=0 crosses
behind the eye. At 32x16 the CPU reference covered 164 pixels, but the native
`noperspective` shader retained only 10. At 64x16 it retained 24 of 324. A
constant-alpha diagnostic restored the coverage, isolating attribute loss
from geometric clipping. Removing the unused texture varying alone did not
fix it. Metal did not reproduce the failure.

The hardware-clipped model, radial model and new surface shaders now encode
each affine attribute as `attribute * clip.w`, use ordinary perspective
interpolation, and decode with fragment reciprocal W. Algebraically this
restores screen-linear interpolation while avoiding that backend's failing
native path: the perspective denominator cancels the fragment reciprocal W,
leaving the screen-barycentric sum of the original attributes. Positions and
hardware clipping remain unchanged; there is no
CPU polygon fallback. Ground UVs follow the same rule as colors. Validation
also bounds the new attribute-times-W products against overflow.

The existing models are untextured, so their fragment shader now consumes
only color and the unchanged alpha threshold, with no texture sampler or UV
sentinel. This model-shader correction applies to the **current default game
path**. This correction was committed separately before the live integration.

## Boundary checkpoint verification (`7f393741`)

- All 165 local app tests pass, including actual Metal, layering and ABI tests.
- The complete standalone GPU fixture passes on both Metal and Deck/Vulkan.
  The old Deck clipping failure is fixed without relaxing its exact oracle.
- New tests cover 39 exact surface/model geometry comparisons, including all
  clip directions, behind-eye/zero-depth crossings, depth occlusion, multiple
  instances, resizing, rotation and reset/republication.
- An independent affine texture oracle checks 1,299 interior pixels against
  analytical screen barycentrics (maximum one 8-bit level error). Patterned
  cloud tests cover 28 wind/atlas/camera/seam/pole states with the existing
  two-level transcendental/texture rounding allowance, not a geometry waiver.
- Separate fixtures check shadow depth acceptance over the entire covered
  surface, alpha holes/no shadow depth writes, copied input lifetimes,
  invalid last-sample atomicity, both sample budgets, handle exhaustion,
  buffer growth/shrink and zero warm camera/wind geometry uploads.
- Every Deck probe uses an isolated executable, existing bundled SDL/Wayland,
  a 20-second timeout and RAM/GPU-allocation guards. The installed game is not
  replaced. D3D12 blobs are generated/checked, but no D3D12 runtime is available
  here; compilation is not a substitute for that backend's runtime oracle.

Full-game comparisons against the previous checkpoint: all 12 Palace captures
are byte-identical. Of 16 navigation captures (2160x1344, movement then hold),
12 are identical and four differ in exactly one pixel's single channel by one
8-bit level. GF500: blue 100→99 at (63,446); GF1700: green 217→216 at (633,671);
GF1800/GF1900: red 118→117 at (1598,413). Both final WRAM dumps match their
controls. The strict navigation comparison correctly reports failure; the
separate pixel audit records this explicit affine-rounding change, not an
exact-parity claim or a raised comparison threshold. Before the affine shader
correction, all 28 captures were byte-identical with the surface prototype and
unused-upload cleanup alone.

The final guarded Deck GPU fixture completes in 0.61 seconds, with over
11.7 GiB RAM available and about 67 MiB peak incremental GPU allocation. Its
process exits normally and Wayland remains healthy. This is a compatibility
probe, not an FPS benchmark. Binary hashes:

- Mac game: `e127d69295bc351fd6d26cda66bb7a6111cd92b4ee719eec7639cd62e5a4a055`.
- Linux GPU fixture: `96b6bc3efa2b9c3f945e2a57c4902198450568e85db0a86a23a35de0bc693fe5`.

Evidence: `/private/tmp/actraiser-surface-prototype.lxXoiY/`; Deck probe directory
`/home/deck/argame/surface-adapter-20260912.Cu4EKv/`. The baseline and diagnostic
failure logs are preserved alongside the successful explicit-affine probe.
No new in-game FPS gain is claimed for this boundary prototype.

## Live integration and ownership

The presentation owner publishes at most 128x128 source quads, excluding
registered cliff replacement cells. Directions, heights, edge opacity,
UVs and source-space lighting normals depend on geography, cliff revision,
chart radius and relief scale, not camera position or wind. Source construction
still uses the existing bounded fork/join worker pool. Temporary CPU source
arrays are freed after the adapter copies them. Per-frame light, camera,
shadow and overlay inputs are copied uniforms.

The grid replaces the existing world-surface opaque handle, not an extra
fifth handle. Its partial integration bypasses the old held ocean/mountain
retention: those surfaces must be staged again. Rejection queues no grid
layers, latches the prototype off until resource reset, and uses the complete
ordinary world. Existing core failure handling remains unchanged. The renderer
receives a generic UV clear rectangle/feather/color, not town/settings policy.
The source API remains project-private; no runner ABI, render vtable, native
handle exposure, GPU readback or compute-queue contract is added.

One clock sample supplies the same shadow parameters to GPU land and the
remaining CPU ocean/cliff receivers. GPU land is excluded from the CPU receiver
cache. Blur/haze use the same GPU geometry/depth as land; remaining cliff
overlays retain the ordinary path. All graphics toggles are preserved.

Live validation adds eight overlay/material oracles, mixed ordinary/source
overlay ordering, copied overlay lifetime, invalid-final-overlay atomicity and
combined shadow/overlay budget tests. A dedicated real-Metal world fixture
compares exact cold/warm images through 13 states (light, relief amount,
detailed ground, orbit, Palace/navigation, shadow/haze toggles, geography
replacement/restoration and an Advent-scale camera). It checks GPU reuse and
zero rejection. A declining-adapter unit fixture verifies complete land/ocean
shadows on fallback and only one failed attempt per resource lifetime.

The final full local build and all 165 app tests pass. The expanded standalone
GPU fixture also passes on Deck/Vulkan (bundled SDL, isolated executable):
0.61 seconds, at least 11.7 GiB available RAM, about 149 MiB peak incremental
GPU allocation, no guard activation. This checks the new overlay uniforms,
mixed ordering and budgets on the real backend, **not in-game Deck throughput**.
Fixture SHA-256: `3544a6feb0d19c785d5b9961b156148f721d92785bc79ce6daa9b6b26392cd63`.
Evidence: `deck-grid-probe.log` in the live evidence directory below. D3D12
shader blobs are generated, but D3D12 runtime behavior remains untested here.

Whole-game frozen-weather comparisons have identical final WRAM in both
views and no unexpected view fallback. They are **not pixel-identical**:
Palace differs in about 300 of 2,903,040 pixels, mostly one 8-bit level, with
one persistent horizon pixel reaching 14 levels. Navigation differs in
1,342–3,378 pixels; only 1–9 pixels per capture exceed two levels, but isolated
coverage-edge changes reach 104 levels. The strict comparator still fails;
its tolerance is not raised. Full-frame inspection shows no broad lighting,
haze or shadow seams. These edge differences remain an acceptance gate,
not a claim of exact parity with the CPU transform.

## Live benchmark results — local Metal, 2026-09-12

32 real-game runs: four cohorts of eight serial ABBAABBA runs, 2160x1344,
isolated save/settings, unchanged quality/effects and pinned binaries. Palace
uses 1800 ticks; navigation uses 2000 ticks, movement followed by a hold.
Each result is the frame-weighted last five settled reporting windows, then
the median across four runs per variant. All runs completed their expected
tick/present schedule with zero re-presents and matching final WRAM. Builds,
captures and GPU tests were kept out of timing intervals.

Render CPU **wall** milliseconds, median [min–max]:

| View / helpers | Current default | GPU grid | Median reduction |
| --- | --- | --- | --- |
| Palace / 3 | 3.196 [3.144–3.273] | 3.075 [2.931–5.800] | 3.8% |
| Navigation / 3 | 2.906 [2.489–2.953] | 2.638 [2.525–3.997] | 9.2% |
| Palace / 0 | 3.767 [3.151–4.026] | 3.259 [2.785–3.410] | 13.5% |
| Navigation / 0 | 3.150 [3.000–3.198] | 2.816 [2.748–2.823] | 10.6% |

**Precision warning:** media analysis/Spotlight indexing and separate local
build activity were observed during these cohorts. One candidate run in each
three-helper cohort had millisecond-scale join waits (Palace mean 3.003 ms
versus control median 0.047 ms). These runs are retained, not selectively
deleted. Background load is a plausible contributor, not proof that every
spike is external. The zero-helper comparison removes helper scheduling from
both variants; navigation has disjoint ranges there. Other ranges overlap,
so the headline percentages are provisional and not a quiet-machine estimate.
Do not compare the separate worker cohorts as evidence for removing multicore
support. The worker pool and fallback remain intact.

The more stable architectural evidence is upload/draw traffic. Three-helper
cohort medians, per presentation:

| View | Depth geometry upload (MiB) | Draws | Submitted vertices |
| --- | --- | --- | --- |
| Palace | 4.026 → 2.035 (−49.5%) | 16 → 29 | 662,188 → 935,581 |
| Navigation route | 6.599 → 4.393 (−33.4%) | 15 → 28 | 611,630 → 1,004,008 |

Texture upload stays approximately unchanged. The entire source chart now
reaches the vertex shader, and splitting land from ordinary surfaces adds
13 draws: opaque/overlay boundaries plus separate land/ocean-cliff shadow
draws. This is a real GPU cost, not free offload. Some held ocean/mountain
CPU staging returns because the prototype occupies their old retention slot.
Moving-camera grid projection and per-frame land haze staging are removed;
ocean/cliff projection and cloud-body preparation are not.

Presentation CPU medians fall 1.809→1.532 ms in Palace and 2.316→2.004 ms in
navigation with three helpers. Present/wait rises 4.556→4.608 and 4.782→5.116 ms
respectively. Median cadence remains about 8.34 ms (~120 Hz); GPU timestamps
are unavailable. The zero-helper navigation cadence is likewise essentially
unchanged (8.557→8.570 ms). **These are CPU/traffic gains, not equivalent FPS
gains, GPU timings, or Steam Deck performance measurements.**

Evidence directory: `/private/tmp/actraiser-live-surface.91uC2O/`.
`timing-grid-{palace,navigation}{,-zero}/results.json` records all run values,
input hashes, exact environment and run directories; `work-summary.json`
records traffic from the same settled windows. `verify-grid-*` records the
strict capture failures and quantified `pixel-diff.json` instead of hiding
the differences. Local benchmark binary SHA-256:

- Control (`7f393741`): `e127d69295bc351fd6d26cda66bb7a6111cd92b4ee719eec7639cd62e5a4a055`.
- Candidate: `0bdd4e7b9952b47b461d740c1d7ad972e5d7fd59deea0bad4416d51dc1337dd6`.

Reproduction uses `tools/compare_pipeline_performance.py`, the manifest
`/private/tmp/actraiser-world-surfaces.NDSzGz/checkpoints.json` and config
`/private/tmp/actraiser-deck-profile.2Y9hIv/benchmark-defaults.ini`:

```sh
python3 tools/compare_pipeline_performance.py \
  --control /private/tmp/actraiser-live-surface.91uC2O/control \
  --candidate /private/tmp/actraiser-live-surface.91uC2O/candidate \
  --manifest /private/tmp/actraiser-world-surfaces.NDSzGz/checkpoints.json \
  --config /private/tmp/actraiser-deck-profile.2Y9hIv/benchmark-defaults.ini \
  --checkpoint navigation --scene 'World 3D' --quit-frames 2000 \
  --set AR_SIM3D_WORLD_GPU_GRID=1
```

For Palace use `--checkpoint palace --scene 'Sky Palace' --quit-frames 1800
--set AR_REPLAY_NOSTOP=1`. Zero-helper cohorts add `--control-workers 0
--candidate-workers 0`. The old control ignores the new grid flag. No saves,
installed Deck executable, or saved graphics settings are changed.

## Remaining integration gates

The [cliff integration](gpu-cliff-source.md) now shares GPU ground, blur, haze
and shadow placement, removing three split-material draws. It is now default
after the user accepted its performance/memory tradeoff; the documented isolated
image differences remain. Extend the
source to the camera-oriented ocean, then handle mountain cutouts without losing
their held-view retention. Preserve
their exact receiver/depth association. The [coarse culling experiment](gpu-grid-culling.md)
now removes 18–28% of submitted vertices with exact images and no extra draws,
but has not demonstrated a frame-throughput gain. It is now enabled by default
within this prototype at the user's request; `AR_SIM3D_WORLD_GPU_GRID_CULL=0`
retains the full source for comparisons. The extra split-material draws remain unresolved.
Retain multicore source construction/compatibility paths.

The [CPU stream cache](gpu-world-stream-cache.md) now removes repeated held-view
ocean/mountain clipping within this prototype. It is bounded and exact, but
still stages/uploads through the existing depth contract. It does not restore
their former GPU residency or resolve the moving-camera integration gate.

Investigate isolated CPU/GPU coverage differences, repeat quiet local cohorts,
and measure GPU/present cost and cold source construction on Deck as the
remaining surfaces are integrated. This is not async compute or GPU-driven visibility.
