# Connected SIM world: default-on release integration

2026-09-12. Promotes the accepted hybrid prototype into the normal game build.
This is a bounded surrounding-world background, not a replacement SIM renderer.

## Shipped behavior

- **Connected world underlay** (`sim3d_globe_underlay`) defaults On. It runs when
  Simulation town 3D, ground projection, world underlay and town models are
  enabled. Authentic users are not switched into 3D, and enabling this feature
  does not enable inter-town navigation or the Sky Palace backdrop.
- The active town retains its authored facade, terrain, models, actors,
  effects, picker and priority/depth composition. Neighbour models use Low
  detail; detailed neighbour models/mountains are limited to a 24-cell reach.
- The surrounding surface meets the active 32-cell town before transitioning
  to the navigation globe over eight cells. Disabling globe relief still
  preserves the active town's edge elevation. Relief-disabled builds retain
  their flat SIM edge instead.
- Camera limits are applied to a presentation-only copy, before every SIM
  layer receives the same matrix: distance at most 4.5, yaw within +/-0.35.
  The camera-range follow-up restores standard SIM pitch down to -1.35 radians
  (about 13 degrees above the ground); the original release probe below used
  the earlier -0.85 limit. Native movement and saved poses are unchanged.
- Existing SIM haze/dimming highlights the whole active town; cloud shroud
  and backdrop remain SIM effects. World terrain/model/lighting controls are
  available without turning on navigation. Navigation-only cloud shadows do
  not expose irrelevant SIM shadow controls.
- The connected cloud shroud samples a shell with the globe's centre and
  radius, preserving the configured altitude above the town. A cached 64x48
  screen grid reaches the horizon without the flat underlay's rectangular
  cutoff, feathers the shell tangent and rejects the planet's hidden far-side
  clouds. Clear triangles are omitted. The existing single-pass cloud shader,
  optional portable three-bank effect, settings and flat-underlay mode remain.
- Accepted smaller/rounded Aitos globe cap and compact rear slope are included.
  The selective 30% rear fit applies only to isolated, interior mountain
  stamps; original front art, height, cleanup footprints, overlapping ranges
  and town-boundary stamps remain protected.

## Ownership, default path and fallbacks

The enable flag is a normal settings descriptor, folded into the existing
game-owned `SimRenderFeatureMask`. The producer captures owned neighbour data
only after resolving an eligible frame. It does not consult a prototype
environment variable, and the presenter does not read live WRAM or settings.
No runner ABI, renderer interface or backend-native handle was added.

`present_sim_globe.h` is the private background-rendering contract. Its
implementation deliberately shares `present_world_nav.c`'s resource owner:
there is one atlas/terrain/model cache policy, not a second upload pipeline.
It receives the final SIM matrix and does not invoke the navigation camera,
UI, input or scene composition. Embedded model cache keys own their mapping
values, and navigation projection memoization is invalidated on SIM use.

Geometry is published through the existing retained GPU contracts. Source
preparation retains the existing helper-thread dispatcher; live town work
continues each frame. The bounded opaque mesh pool grows from four to eight
handles so the globe cannot crowd out the retained SIM town passes. Storage
is allocated only when a mesh is published; weather budgets are unchanged.

Unchanged background images are automatically reused. Continuously changing
views render directly without making a copy that will not be reused. There
is no shipping cache opt-in/opt-out branch; the direct-image oracle is compiled
only into the GPU test. The public setting's environment alias remains a
normal supported settings override, not a second source of presenter state.

The flat underlay is intentionally retained as a low-end option and a GPU
resource-failure fallback. Cache-copy allocation failure keeps the complete
direct image; loss of the caller's render target aborts the frame. These are
required recovery/quality paths, not obsolete optimization experiments.

## Verification

Camera-range follow-up: the GPU test now covers eight poses/relief combinations
for each of six native town backgrounds, including the lower pitch with both
yaw bounds and minimum/maximum zoom. Direct/cached pixels and return to navigation
remain exact. Full-game Aitos before/after captures at the same requested pitch
confirm the less overhead framing, with identical final WRAM. The default pose,
neighbour draw bounds, LOD policy, world-navigation camera and Sky Palace are
unchanged; no new performance claim is made for the lower viewpoint.

Curved-cloud follow-up: cloud tests cover normal/low pitches, both yaw bounds,
three altitudes (including an eye below the shell), atlas-to-sphere reprojection,
curvature invalidation, retained meshes, cloud-off and flat-underlay switching.
Placement values are published on both direct and cached globe frames through
the private presentation contract; no renderer ABI or shader format changed.

- Nine focused portable suites pass: settings, metadata, SIM camera, pure
  mapping, retained-image policy, three world-presenter configurations and
  mountain materials. [Log](portable-tests.log).
- 45,738 mapping round trips span all six towns and seven height levels.
  Tests also cover independent world/town relief and invalid inputs. Mapping
  and retained-image tests pass ASan/UBSan, with checks retained under NDEBUG.
- GPU cache parity spans 23 changing states, five repeats and three atlas/cache
  configurations. Continuous-change draw traffic matches direct rendering.
  Six native town backgrounds are checked at normal/bounded cameras and with
  relief off; direct/cached pixels and the return-to-navigation image are exact.
  [GPU matrix](gpu-tests.log), [depth tests](depth-gpu-tests.log).
- The integrated Mac release build matches the accepted prototype in all
  21 full-composite Aitos captures and final WRAM. [Results](prototype-parity.json).
- Default enablement, navigation disabled, and SIM clouds/backdrop/haze/effects
  enabled: zero versus three helper threads produce 21 identical composites
  and identical final WRAM. Cloud drift is frozen for deterministic comparison.
  [Results](default-effects.json). An earlier attempt stopped at the replay end
  before the requested final tick; it was discarded and rerun with no-stop.
- The CMake Mac release build and hermetic Linux build both succeed. The normal
  workspace executable is also rebuilt. The last follow-up changes only menu
  availability for navigation-only cloud-shadow controls; it does not change
  rendering. A final Deck correctness rerun covers that executable too.
  [Final executable and verification](deck-final-verification.json).

The all-town GPU captures isolate the background, intentionally omitting the
active town. They are not six full gameplay playthroughs. Full-game replay
coverage here is Aitos. Campaign-wide/manual release QA remains separate.
The bounded background composition is intentionally not a shared town/world
depth buffer; unrestricted camera travel is not supported.

## Steam Deck probe

Visible Wayland, 1280x800, normal presentation cadence, three helpers, held wide
Aitos camera, High/adaptive active-town models, focus haze; clouds/weather off.
Two runs per variant in `flat, default, default, flat` order:

| Background | Rendered FPS range | Mean of two runs |
| --- | ---: | ---: |
| Flat | 210.67–210.90 | 210.79 |
| Default connected world | 201.27–207.03 | 204.15 |

This focused confirmation is about **3.15% below flat throughput**. It is
consistent with the earlier larger prototype cache benchmark (~204 FPS
cached versus ~158 uncached); it is not a claim of a new measured improvement
over that prototype. Two runs are a probe, not a hardware-wide performance
guarantee. These are rendered frames, not panel refresh or emulation ticks.
No Mac performance timings were used during the concurrent action testing.

Both variants complete 2,400 ticks with identical final WRAM, no view fallback
or failed frame in settled windows, and no resource/timeout guard firing.
GPU allocation growth is a whole-system reading, not a cache-only footprint;
GPU execution timestamps are unavailable. [Metrics/platform/guards](deck-timing.json).

Separate Deck replays compare explicit enablement to the **default with no
enable or cache environment variable**. All five 1080x672 composite images and
final WRAM match. [Verification](deck-verification.json).

All runs use private fixture saves/settings and temporary game binaries under
`/home/deck/argame/sim-connected-release.4jFBq6`. The installed Deck executable,
live saves, Steam, governors and power settings are untouched. Binary/input
hashes and exact environment are included in the result files.

## Reproduce the focused Deck probe

Build Linux through `snesbuild build --hermetic` with `--target x86_64-linux-gnu`,
the project's Zig toolchain and the Deck's bundled SDL headers/libraries.
The [build log](build-deck.log) records the tested paths and 394 translation units.

Prepare a new bundle on the Mac using the included [probe](probe.py) and
[environment](environment.json):

```sh
python3 docs/evidence/sim-connected-world-release/probe.py \
  --prepare /absolute/path/to/linux/ActRaiserRecomp --output /new/bundle
```

Copy that bundle to a new Deck directory and run `python3 probe.py --output
verification --verify`, then `python3 probe.py --output timing`. Output paths
must be new. The probe refuses concurrent game processes and guards only its
own child against timeout or memory exhaustion. Its ROM path is read-only.
