# Globe cloud-shadow sample batching

Implemented after committing the preceding cloud-slice bounds optimization as
`815454b5`. Batching is **enabled by default**; `AR_SIM3D_SHADOW_BATCH=0` is a
diagnostic reference. No graphics settings, cloud density, atlas resolution,
sample count, town detail, or worker count were reduced.

## Repeated all-effects results

Same-binary pairs use the complete [all-effects profile](../all-effects-2026-09-12/profile.json),
moving clouds, normal visible presentation, interpolation, three helpers,
Unlimited refresh, and zero audio volume on both sides. Pair order alternates.
Three 2,000-tick Palace pairs and two 2,400-tick navigation pairs per host;
the first scene-entry window is excluded, retaining all subsequent 27 Palace
or 33 navigation windows. These are median **presentation-cadence** FPS, not
emulated tick rates or isolated GPU-pass timings.

| Host / scene / output | Individual draws | Batched draws | Throughput gain |
| --- | ---: | ---: | ---: |
| Deck Vulkan / Sky Palace / 1280×800 | 333.96 FPS | 409.34 FPS | +22.6% |
| Deck Vulkan / navigation / 1280×800 | 282.97 FPS | 380.07 FPS | +34.3% |
| Mac Metal / Sky Palace / 1440×896 | 236.38 FPS | 359.25 FPS | +52.0% |

Deck Palace ranges: 333.47–334.04 versus 408.10–409.53 FPS; navigation:
282.69–283.25 versus 379.89–380.24 FPS. Mac Palace: 236.21–236.47 versus
359.23–361.35 FPS. Deck cadence falls 2.994→2.443 ms for Palace and
3.534→2.631 ms for navigation. Aggregate render CPU falls 1.354→1.074 ms
and 1.270→0.966 ms respectively; nested CPU scopes are not additive and may
include driver blocking or changes in the tick/re-present mix.

Palace submits **28→16 draws** and about **977k→555k indexed vertices** per
presentation on the Deck. Navigation submits **27→15 draws** and about
**1,006k→574k vertices**. These are actual submission counters, not logical
shadow sample counts. Buffer allocations, source uploads and logical budgets
are unchanged by batching.

### Noisy controls: retained, not used for gain claims

Two cross-mode Mac pairs drifted substantially: navigation's individual-draw
runs were 247.39/192.34 FPS and batched runs 414.18/289.87 FPS. Background
indexing/media processes were recorded. No causal Mac navigation speedup is
claimed from those runs.

Two dynamic-camera SIM pairs on each host were also noisy. Deck individual
runs were 138.41/135.37 FPS and batch runs 122.43/138.71 FPS; Mac runs were
338.12/258.50 and 244.24/255.09 respectively. The first Deck batch run is
an outlier, not evidence of a persistent regression. These controls do not
establish a performance gain or a tight regression bound.

`PresentSimGlobeUnderlay` submits ocean, terrain and mountain surface batches
with **zero shadow samples**, so this optimization does not coalesce SIM draws.
SIM's town/cloud-shell work remains on its existing paths. Its three image
pairs are byte-identical. No SIM speedup is claimed here; earlier SIM GPU
model-projection improvements remain enabled.

All 28 timed runs completed without memory/timeout guard aborts or sampled
enhanced-path failure, fallback, rejection, opt-out or limit counters. Final
emulated WRAM matches per scene across hosts/variants. Legitimate mode-entry
bridges are not classified as failures. No installed game files, settings,
power governors, or GPU clock policy were changed.

## Implementation and contract audit

The SDL depth adapter coalesces at most three **consecutive compatible black
surface shadow samples** at submission. It requires the same retained source,
selection range, transform, material, shadow basis, rotation, texture extent,
and atlas rectangle. Only offset and opacity may vary. Different colors,
receivers, charts or intervening queue entries retain the normal GPU path;
there is no new CPU fallback.

The vertex shader shares radial placement and spherical-coordinate helpers
with the opaque surface shader. Exact ordered FMA placement is important:
independently compiled receiver and surface pipelines must agree on depth.
Each tap independently preserves per-corner wrap, seam unwrap and pole clamp,
then screen-linear interpolation through hardware clipping. The fragment
shader applies the existing alpha cutoff to each tap before combining black
transmittance as `1 - product(1 - alpha)`, **not** the sum of sample alphas.

This is private backend submission work. SDK ABI and public structure layouts,
atomic queue admission, copied-input ownership, geometry/effect budgets,
selection lifetime, reset behavior, atlas publication, depth/no-depth-write
policy and sampling remain unchanged. A draw-local 352-byte uniform extends
the existing 304-byte prefix; static assertions guard both sizes. Retained
sample structures and buffers do not grow. Physical draw/vertex metrics
account for coalescing. Existing helper-thread behavior is untouched.

The new shaders and pipeline are prepared through the existing video
boot/reset capability path, not lazily during gameplay. MSL, SPIR-V and DXIL
blobs are generated ahead of time. The ordinary transparent surface pipeline
remains required for other effects and incompatible samples, not merely as
an obsolete implementation branch.

## Correctness and portability

- Fifteen untimed image pairs: Metal Palace/navigation/SIM at 1440×896,
  Vulkan Palace/navigation at 720×448. Clouds were frozen only for image
  comparisons, not timing. Maximum per-channel error is **3/255 Palace,
  4/255 navigation, 0 SIM**; maximum mean error is 0.01451/255 Palace and
  0.28692/255 navigation. Fewer intermediate 8-bit blends produce widespread
  small rounding differences over shadowed ocean; the result is not bit-exact
  to individual draws. No pixel differed by more than 8/255.
- The final default-enabled Mac replay, with no batching override, gives nine
  byte-identical screenshots and matching final WRAM versus the tested opt-in
  path across all three scenes. The shared surface helper refactor also
  matched three preceding cloud-bounds default Palace screenshots byte-for-byte
  with batching disabled.
- The complete depth GPU suite passes on Metal and Vulkan with both the new
  default and diagnostic opt-out. New coverage includes 39 reference comparisons:
  one/two/three/nine samples, incompatible colors/rotations/atlases, clipping at
  side/near/eye planes, longitude seams/poles, nonidentity bases, overlapping
  faces, reset/viewport reuse, copied inputs and queued-update rejection.
  The below-cutoff test requires exact pixel identity, not an error tolerance.
- Existing capacity/ownership tests still pass. The 64 opaque plus 64 effect
  admission limit remains intact; eligible submissions now need 86 physical
  draws instead of 128. Over-budget requests retain atomic rejection.
- All three private/render backend boundary tests pass. Shader-blob loading
  passes on Metal, and generated MSL/SPIR-V/DXIL headers pass regeneration
  checks. Vulkan executes the final depth suite on the Deck. Windows/D3D12
  runtime execution was **not** tested in this batch.

## Reproduction and evidence

[Results](results.json), [runner](probe.py), [image comparator](compare.py), and
[validator/archive generator](summarize.py) are versioned. The generated
[raw archive](raw-evidence.zip) remains local under the repository ZIP ignore
policy. It contains logs, environment/guard/telemetry reports, image statistics,
and hashes, not ROMs, saves, executables, or installed settings.

Builds use the preceding isolated model-projection source plus the scoped
cloud bounds and batching changes, excluding unrelated dirty packaging and
localization work. Mac: Release, enhanced text off. Deck: hermetic x86_64 Linux
`-O2`. Deck fixtures were copied and hash-verified entirely on the Deck into
the private test directory. The same binary is used on both sides of every
pair; final promotion changes only the default diagnostic choice and tightens
the tiny-alpha test. Binary and scoped source hashes are in `results.json`.

With a private pinned input directory prepared by the preceding all-effects
probe, use `probe.py ROOT --binary BINARY --inputs PINNED_INPUTS` for the three
Palace pairs, and `--cases navigation sim-held --repeats 2 --cohort modes` for
controls. Add `--deck` with the Deck-local base probe for Wayland runs. Use
`--visual --cohort images` for image pairs and a separate root with `--default`
for default-on verification. Never reuse/overwrite a timed input cohort.

This closes the three-tap globe shadow batching stage. It does not optimize
SIM's separate shadow pass or prove that other rendering hotspots are exhausted.
