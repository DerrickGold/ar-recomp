# Camera-guided background motion bound

Prototype, 2026-10-19. Opt in with `AR_GPU_CAMERA_MOTION_BOUND=1`;
unset or other values retain the original bound candidates. The preceding
actor-stationary experiment remains separate and disabled in these comparisons.
Base commit: `f7fb6436` plus the uncommitted actor prototype.

## Mechanism and exactness

The global search scores all 225 legal displacements on a coarse sample grid,
pruning a candidate only when its nonnegative partial cost exceeds a known
complete cost. Previously that upper bound came from nine displacements around
zero. Scrolling farther than one pixel can make all nine poor matches.

The capture owner already has immutable BG1/BG2 camera coordinates. It retains
the previous capture's coordinates and supplies the opposite camera delta as
an optional image-motion hint. BG1 low/high/far planes use BG1; BG2 low/high/far
and nonperiodic skybox use BG2. The backdrop keeps the original search. Camera
metadata and hint selection stay outside the generic motion kernels.

The bound pass scores one additional displacement, clamped to the existing
`[-7, 7]` search radius. Backward analysis uses its inverse. The original nine
bound candidates remain, so a wrong hint cannot weaken pruning. A hint already
inside the original 3x3 grid adds no work. Room changes/invalid pairs clear hints;
inactive bands never read their optional cost slot.

The hint is not accepted as motion. All 225 candidates, deterministic tie rules,
fine refinement, confidence checks and inverse validation remain in place.
Pruning uses strictly `>` rather than `>=`, retaining every possible winner and
equal-cost tie. The chosen coarse candidate and final result are therefore
unchanged even if animation, raster scrolling or skybox policy disagrees with
the camera hint.

There is no additional pass, pipeline, readback, fence or per-frame allocation.
The bound buffer grows by 64 bytes. Hints occupy the unused analysis uniform
component; synthesis retains its existing uniforms. The native worker receives
an immutable copy of the hint metadata with the rest of its job.

## Validation

- 1,152 exact comparisons with the independent CPU global-motion oracle:
  three geometries, unequal plane widths, static/scrolling/animated/periodic/
  transparent/empty/sparse inputs, correct/reversed/random/extreme hints,
  inactive bands and hint-on-to-off transitions.
- 134/134 exact whole-game frame pairs: Marahna `05:01` at phase 0.5 (48),
  Fillmore `01:01` at phases 0.25 and 0.75 (43 each). Same binary, settings,
  assets, input and final WRAM within each comparison. Camera travel includes
  horizontal scrolling and vertical drops. Native decode, direct layer output
  and worker preparation exercised; Fillmore uses resident effect projection.
  All 134 also match the saved pre-change binary's captures, independently of
  toggling the new mode within the prototype binary.
- All 16 focused renderer/motion tests and four backend/private-header checks
  pass. The worker integration fixture supplies the camera delta matching its
  moving image.
- MSL, SPIR-V and DXIL regenerated and checked. Module and new test compile
  under strict C11 warnings for ARM64/x86-64 macOS, x86-64 Windows and Linux.
  Runtime validation here is **Metal only**, not a Deck or Windows result.
- No increase in authored C style violations; the new test has none.

## Isolated search measurement

Mac M2, Metal. Eight 640x352 planes, eight analyses per submission, 64 timed
batches after eight warmup batches; reference/hint/hint/reference order.
Uploads, downloads and compilation are outside timing. Values are mean
encoding-through-completion time normalized per analysis, not per-frame latency.

| Input | Original shader control | Hint off | Correct hint | Reversed hint |
|---|---:|---:|---:|---:|
| Dense stationary | 1.649 ms | 1.648 ms | 1.651 ms | 1.638 ms |
| Scrolling | 5.243 ms | 5.228 ms | 2.085 ms | 5.208 ms |
| Scrolling with changed pixels | 5.482 ms | 5.503 ms | 2.424 ms | 5.536 ms |
| Empty | 0.428 ms | 0.413 ms | 0.435 ms | 0.434 ms |

Correct hints reduce scrolling search by 56–60% in these synthetic fixtures.
Wrong hints still perform the full original work plus the small bound check.
This table is not a whole-frame speedup. The original control was compiled from
the saved pre-change module/shaders using the same benchmark harness.

## Gameplay timing

Twelve sequential runs: two per mode per scene, reversing order within each
scene. Same release binary and replay/settings/final WRAM per scene; 960x540,
16:9, 64 extra rows, Metal VSync, native worker, direct output and resident
effects. Actor-stationary rejection and early submission are both off.
Pacing ticks 1600–2499 exclude boot, warp and first diorama frames.

The table pools about 1,800 **packing + global/actor motion GPU command
intervals** per mode. These are command-buffer start/end times, including GPU
dependencies; they are not whole-frame times or hardware busy percentages.

| Scene | Mean off → on | Mean change | p99 off → on |
|---|---:|---:|---:|
| Stationary Bloodpool `02:01` | 0.676 → 0.698 ms | +3.4% | 1.032 → 1.156 ms |
| Marahna `05:01` replay | 2.706 → 2.498 ms | −7.7% | 5.636 → 4.148 ms |
| Fillmore `01:01` replay | 2.096 → 1.992 ms | −4.9% | 5.220 → 5.292 ms |

Marahna's improvement repeats in both orders: mean 2.727 → 2.493 ms and
2.685 → 2.502 ms; p99 5.470 → 4.167 ms and 5.751 → 4.085 ms.
Fillmore's average improves, but its tail does not show a reliable gain.
Stationary Bloodpool shows no benefit; pooled medians are nearly identical
(0.6708 → 0.6716 ms), with a slightly worse mean/tail in the enabled runs.
Keep this regression control in subsequent hardware comparisons.

All twelve traces were foreground/unlocked, completed their GPU commands and
reported every presentation with a unique display timestamp. Resident effects
downloaded zero motion metadata. Display-interval p99 remained 16.667 ms in
every run on this 60 Hz Mac. There were isolated 33.333 ms intervals in one
Marahna run of each mode, and a 50 ms interval in one enabled Fillmore run.
The cause of those isolated stalls is not established by this experiment.
The known launch-dependent extra refresh also remains: API-return-to-display
averaged about 32 ms in most runs, 48 ms in one Bloodpool reference run, and
39 ms in one Fillmore enabled run. These measurements are not input latency.

**Decision:** retain as an opt-in prototype. It produces identical pixels and
repeatable savings during useful scrolling cases, but does not establish a
whole-frame pacing win, resolve compositor stalls, or prove a Deck 90 Hz budget.
Check actual Windows Vulkan/D3D12 and Deck runtimes before default promotion.

### Steam Deck follow-up, October 20

The [Deck validation](steam-deck-prototype-validation.md) now covers Vulkan:
all 1,152 CPU-oracle checks and 134 full-game frames match exactly, including
combined batching/worker/actor prototypes. Isolated scrolling search improves
39–47%; camera-on and camera-off game runs both retain 11.111 ms display p99
with no missed refreshes in the four presentation-feedback windows.

This supports default promotion on the tested Metal and Deck Vulkan paths.
It does not establish a whole-game pacing improvement when VSync already
holds 90 Hz, nor cover D3D12.

**Promoted October 20, 2026.** The bound is on by default for Metal and
Vulkan through `gpu_prototype_policy.h`; `AR_GPU_CAMERA_MOTION_BOUND=0`
restores the reference search and D3D12 keeps it until a Windows runtime
pixel comparison exists. The promotion A/B is recorded in
`runs/frame-pacing-claude-20261020/mac-promotion-ab/`.

## Reproduction and evidence

Ignored local evidence: `runs/gpu-camera-bound-20261019/`. It contains baseline
sources/binary, original-shader control, benchmark logs, capture comparisons,
timing scripts and reports, validation logs and source/binary hashes.

```sh
cmake -S . -B build-tests
cmake --build build-tests --target actraiser_gpu_global_motion_test
SDL_GPU_DRIVER=metal build-tests/actraiser_gpu_global_motion_test
SDL_GPU_DRIVER=metal build-tests/actraiser_gpu_global_motion_test --benchmark
```

The release-built standalone benchmark in the evidence directory avoids debug
encoding overhead. `tools/compare_action_gpu_frames.py` accepts
`--camera-motion-bound 0|1`, records the selection, and rejects an enabled
capture without the initialization log confirming activation.
