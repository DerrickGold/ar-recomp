# All-effects Steam Deck benchmark — 2026-09-12

The current enhanced paths were measured with effects enabled, including the
connected SIM globe and curved cloud cover. The clearest follow-up targets are
SIM's camera-dependent CPU model projection/uploads and Sky Palace's cloud
shadows plus volumetric foreground clouds. No runtime code or defaults were
changed by this investigation.

## All-effects results

Three independent runs per row; median presentation throughput, with the full
run-to-run range. These are **uncapped rendered presents**, including interpolated
re-presents, not simulation speed or the panel's physical refresh rate.

| Workload | Median FPS | Run range | Render CPU wall time/present |
| --- | ---: | ---: | ---: |
| Aitos SIM, fixed orbit + scrolling/events | 165.85 | 161.03–166.61 | 4.323 ms |
| Aitos SIM, shipping dynamic camera + scrolling/events | 121.38 | 119.02–121.51 | 7.181 ms |
| Aitos SIM, low-angle/zoomed-out free camera | 192.24 | 189.20–194.28 | 3.862 ms |
| World navigation | 282.27 | 282.24–282.73 | 1.268 ms |
| Sky Palace | 278.71 | 277.81–279.18 | 1.674 ms |

The camera rows are **different realistic workloads, not a perfectly isolated
camera-angle A/B**: fixed orbit uses distance 350 / pitch −575 mrad; the low-angle
case uses 450 / −1350; dynamic mode uses its shipping baseline pose and reactive
motion. The free-orbit replay still scrolls and animates the town. These averages
include later scene re-entry work, not just an inexpensive idle tail.

## Hotspots and interpretation

### 1. Moving SIM camera: retained town geometry misses its projection cache

Median model-project CPU wall time rises from **0.287 to 1.623 ms/present** between
fixed-orbit and dynamic-camera workloads. Total measured render preparation rises
from 4.323 to 7.181 ms. Dynamic depth uploads are approximately **2.48 MiB/present**,
versus 0.47 MiB in the fixed-orbit workload. Mountains also rise from about
0.055 to 0.461 ms and the SIM cloud scope from 0.106 to 0.397 ms.

Code confirms the mechanism: `ProjectionKeyMatches` in
`src/sim/sim_background_voxel_renderer.c` includes the entire camera matrix and
camera position; `PrepareRetainedPass` invalidates on a changed key. The ordinary
path then executes `CollectDepthGeometry` / `DrawModel`, projecting model-face
corners on the CPU. `ApplySimDynamicCamera` in `src/present_sim3d.c` changes the
camera during lean/damping, including interpolated re-presents. This is an active
enhanced-renderer path, **not a failure-induced switch to authentic graphics**.

Best next prototype: retain model-space town meshes and move camera projection to
the GPU, while preserving the SIM facade, foundation/terrain contact, actor depth
ordering, and layer contracts. Profile mountain/cloud rebuilds separately after
that. Three render helper threads were enabled here; this experiment does not
compare worker counts or justify removing multicore support. Coarse GPU busy
averages around 70% during the dynamic SIM measurement interval, consistent with
remaining CPU-side feeding work but not sufficient to prove a single bottleneck.

### 2. Sky Palace: substantial cost in the cloud stack

A separate **on/off/off/on** sequence changed only cloud density 35 → 0 → 0 → 35.
All other settings stayed enabled. Full clouds produced **277.59–278.10 FPS**;
density zero produced **554.25–555.29 FPS**. The whole-stack difference is about
**1.80 ms/present**. This disables globe clouds, their shadows, and foreground
Palace clouds together; it is diagnostic, not a quality-preserving optimization.

Full clouds submit about 28 draws / 978k vertices, compared with 6 draws / 288k
vertices at density zero. Submitted vertices include repeated receiver/body
draws, not just unique cloud geometry. The `SIM clouds` CPU scope itself is only
about 0.11 ms, so optimizing just the foreground quad-projection loop would not
address most of the measured difference.

During the settled Palace interval, coarse GPU busy averages about **90%** with
full clouds. The GPU is meaningfully occupied in this current scene; the much
older 14% utilization observation should not be assumed to describe this build.
There are no per-pass GPU timestamps here: CPU scopes include driver blocking,
and the tick/re-present mix changes as FPS rises. Neither this on/off delta nor
`present/wait` can be labeled pure cloud GPU time.

The follow-up `cloud_components.py` sequence separately tests cloud shadows off
and single-layer foreground clouds, with full-cloud controls at both ends:
full / no-shadows / flat-foreground / flat-foreground / no-shadows / full.

| Palace variant | Median FPS (two runs) | Cadence | Draws / submitted vertices |
| --- | ---: | ---: | ---: |
| All effects | 278.66 | 3.589 ms | 28 / 978k |
| Only cloud shadows off | 374.91 | 2.667 ms | 10 / 345k |
| Only foreground volumetric option off | 372.60 | 2.684 ms | 28 / 977k |

Both effects contribute approximately **0.9 ms of end-to-end frame cost** in this
scene. Turning off shadows removes 18 draws / approximately 633k submitted
vertices. The flat foreground variant removes only about 1.5k submitted vertices
and leaves draw count unchanged, yet is substantially faster. That makes
foreground translucent coverage/overdraw a stronger hypothesis than raw vertex
count or draw-call count alone. It is still an inference, not a GPU subpass trace;
the individual ablation deltas should not be assumed strictly additive. The
reverse-order repeats agree within about 0.4% for each diagnostic variant.

Likely optimization directions to validate without losing the look: reduce
redundant shadow receiver work and foreground translucent overdraw; investigate
tighter cloud coverage bounds or a separate cloud render target with a visually
validated reconstruction. The existing world shadow sampling and foreground
slice paths are in `src/present_world_nav.c` and `src/present_world_nav_sky.c`.
The diagnostic switches are not proposed shipping defaults.

### 3. SIM re-entry/reframing spikes: preparation and uploads

The fixed-orbit Aitos runs each have a short reporting window at **55–64 FPS**
after town re-entry around game frame 906/907. Those windows average approximately
3.64–3.85 ms of model projection, 5.33–5.64 ms of total SIM depth-pass CPU wall time,
2.08–2.37 ms of outer upload work, and 2.74–2.92 ms of PPU/capture work per present.
These nested scopes must not be added together. Dynamic and low-angle cases also
retain short re-entry spikes; their good overall FPS does not mean every frame
meets a 60 Hz budget.

Prioritize persistent geometry and avoiding redundant scene re-publication,
then examine the remaining entry upload burst. Loading under the existing fade
may help visible pacing, but these measurements do not establish that every spike
occurs while fully hidden.

Particles are not a first-priority CPU target in this fixture: the SIM effects
scope averages **0.026–0.035 ms/present** with effects visibly active. This does not
measure the marginal GPU cost of every particle type, nor prove particles are
always cheap in every town/miracle. Haze and CRT similarly cannot be cleared of
GPU cost solely from their small CPU scopes.

## Coverage and safeguards

- Explicit settings are in `profile.json`; each run's final environment is in
  `raw-evidence.zip`. SIM ground, billboards, heights, shadows/softness, rim/effect
  lighting, particles, connected underlay, curved clouds, haze/dimming, defocus,
  backdrop, and lift compensation are enabled. Globe terrain/mountains, live
  ground, towns, lighting, clouds/shadows, atmosphere, Palace volumetric clouds,
  and CRT with all its nonzero sub-effects are enabled.
- Uses Quality / High / Adaptive LOD / Materials + AO / Varied models /
  Per-model lean / Pixel-clean. Effect strengths are ordinary shipped values,
  not every quality slider maxed or Ultra/2× supersampling. Cloud drift stays on.
  The unimplemented picker-ease option remains off; diagnostic layer mask is zero.
- Aitos has an active eruption/particles and scrolling. Separate untimed Mac
  metadata/capture preflights verified all SIM effective flags `$7eff`, visible
  effects, and changing scroll coordinates. Fillmore miracle/scroll coverage was
  also checked on Mac, but is **not** a Deck timing claim in this report.
- Target enhanced scene samples report zero compatibility fallback, rejected,
  failed, opt-out, and limit counters. All replays produce matching final WRAM
  hashes within each fixture across repeats and effect/camera variants.
- View-transition logs still contain brief one-frame authentic bridges at scene
  changes, including unsupported capture at SIM entry. Zero geometry fallback
  counters do **not** mean zero view transitions anywhere. No sustained unexpected
  drop to authentic was found in the measured enhanced scenes.
- No memory guards or runtime failure checks tripped in **25 timed runs**. They kept
  at least 11.3 GiB available RAM, with less than 0.9 GiB additional reported GPU
  memory. This is GPU growth relative to each run's start, not total allocation.
- Action rendering, busy interactive settings menus, extra vertical-tile
  configurations, replacement packs, and all town/miracle combinations are not
  benchmarked here. Audio runs at zero volume; its normal pipeline still runs.
  The isolated fixture has no Unicode font pack; logs explicitly retain the
  native interface font. These omissions should not be mistaken for a complete
  release-wide effects/asset stress test.

## Method and reproducibility

- Steam Deck desktop Wayland, borderless **1280×800**, Vulkan / RADV VANGOGH
  (`25.99.99` reported driver), Unlimited, VSync 0, cap 0, interpolation on, three
  render workers. No power/governor settings were changed; the recorded CPU
  governor is `powersave`, GPU power mode `auto`. No TDP inference is made.
- Renderer/source baseline: clean tracked-source export of **`e1699c9e`**, plus
  the required ignored generated `src/gen` / `recomp/funcs.h`, isolated Linux
  x86-64 hermetic build with `-O2`. Exact executable SHA-256:
  `012e457c98a4b1063f128dc065414ad27062b7009119e7122a86c01ddd00762a`.
  Concurrent uncommitted installer, UTF-8, distribution, and documentation work
  was not included or modified. This is not a comparison against an older binary.
- Private Deck folder: `/home/deck/argame/all-effects.TtkGxe`. Existing Deck ROM,
  saves, and replays were hash-verified and copied within the Deck; no ROM/save
  was uploaded. Installed game and user settings were not overwritten. Build,
  harness, and profile transfer/testing were explicitly authorized.
- `probe.py --visible` uses normal ticking plus re-presents; default headless
  mode is only a tick-work/preflight mechanism and must not be compared as FPS.
  Baseline order was reversed in round two. Each Aitos/navigation replay runs
  2400 emulation ticks; Palace runs 2000. Only the first matching scene reporting
  window is omitted; all later target-scene windows are retained.
- Per-run throughput is total reported cadence intervals divided by their total
  duration. CPU means are frame-weighted; table entries are medians of independent
  runs. `render CPU` sums non-overlapping outer scopes (PPU/capture, world build,
  metadata, town canvas, frame snapshot, upload, presentation), not their nested
  child scopes or helper-thread time. It excludes emulation and present/wait.
- Optional telemetry samples the driver's GPU busy counter at 4 Hz. Reported
  scene-only means use wall seconds 17–39 for SIM and 8–32 for Palace, approximate
  scene intervals rather than frame-synchronized GPU timings. Raw samples,
  thermal/governor snapshots, memory guards, logs, and environments are archived.
- Mac captures were untimed correctness checks while another task was building.
  No competing Mac performance timings are presented.

Existing private Deck setup can reproduce additional runs with a unique cohort:

```sh
ssh steamdeck 'python3 /home/deck/argame/all-effects.TtkGxe/probe_followup.py \
  /home/deck/argame/all-effects.TtkGxe --deck --visible --telemetry \
  --cohort repeat-all-effects --cases sim-held navigation palace --repeats 3'
```

Use `--set AR_SIM3D_CAMERA_MODE=1 --cases sim-held` for dynamic SIM; `--cases
sim-low` for the low-angle case. `followup.py` and `cloud_components.py` record the
serial diagnostic orders. They expect `probe_followup.py` beside them on Deck.

`aggregate.py` re-parses raw logs, checks reported aggregates and failure counters,
validates final WRAM hashes across variants, and produces `results.json` plus
`raw-evidence.zip`. The archive contains logs/metrics only, **no ROM, save, or
executable**. Input hashes are included; full private run directories remain on
Deck for further diagnosis.
