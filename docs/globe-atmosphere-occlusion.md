# Covered globe atmosphere — 2026-09-12

The default GPU globe path omits whole atmosphere bands proven to be hidden
behind its opaque ocean mesh. This affects navigation and Sky Palace, not SIM
town rendering. It changes neither the visible halo's vertices/triangles nor
its opacity, colors, compositing order, resolution or effect settings.

## Bounds and ownership

`WorldNavigationOccludedShellRings` is a private, pure presentation-geometry
helper. It bounds the actual faceted ocean from inside, accounting for both
angular tessellation steps and floating-point placement error. An ideal smooth
sphere would not be a safe occluder for this mesh. The apparent covered cone is
convex, so an omitted triangle's interior is covered as well as its vertices.
The first potentially visible band retains both original vertex rings and its
original diagonal/interpolation.

The camera must match the perspective matrix's projective origin. Conservative
near/far ray-distance checks ensure that clipping cannot expose the discarded
background, including close/Advent views. The near bound uses all four viewport
corner rays; the far bound uses perpendicular distance. Invalid, inconsistent,
orthographic or uncertain inputs retain the complete shell. Radial navigation
does not CPU-clip its triangles, but still has a valid perspective matrix and
an opaque GPU ocean, so it is eligible too.

The presenter skips covered projection/clipping work. Its direct navigation
draw submits only the retained vertex suffix with rebased indices. The fixed
index array adds 109,440 bytes, with no per-frame allocation or new resource
handle. Existing clipped-geometry caching still handles stable Palace views.
The cache key includes whether occlusion is enabled, so an unavailable GPU
surface path cannot reuse a previously shortened range. The existing complete
`AR_SIM3D_WORLD_GPU_GRID=0` compatibility opt-out retains full atmosphere.

No runner ABI, render-device vtable, shader, game-state interpretation, helper
pool or layer ownership change. Atmosphere remains a background draw before
the opaque globe; it is not moved into a differently blended depth target.

## Validation and measurements

An isolated source snapshot of `136ac86a` prevents concurrent unrelated action
work from entering the control/candidate binaries. RelWithDebInfo game builds
use the same settings, compiler and disabled watchdog. Debug fixture builds
keep assertions enabled: this suite uses assertions with setup side effects
and is not a valid `NDEBUG` test harness.

All 165 isolated Debug CTests pass. The geometry test exercises 100 scaled/
altitude cases and over 10,000 independent interior ray samples, plus horizon,
near/far, invalid-camera and invalid-matrix cases. The final additional guard
assertions also pass. The real-GPU suite includes atmosphere profiles, manual
orbit, high relief, tall models, Advent and compatibility paths.

Final full-game comparisons are byte-identical: 16 navigation, 2 Palace and
9 SIM-town composites, each with matching final WRAM. Four additional Vulkan
composites (two per globe view) also match byte-for-byte with matching WRAM.

Local timing uses serial ABBAABBA, four runs per variant, Quality and three
helpers, without our own overlapping builds/tests/captures. Settled windows
are frame-weighted, then summarized across runs. Navigation render CPU is
1.818 [1.768–1.847] → 1.754 [1.460–1.782] ms: a **3.5% lower median**, with
overlapping ranges and unrelated PPU variation. Atmosphere itself is much
more consistent: .238 [.231–.243] → .103 [.087–.105] ms, **56.6% lower**.
Hidden-window cadence remains approximately 8.34 ms; this is not an FPS gain.

The first candidate only activated the optimization for CPU-clipped Palace
views. Its separate Palace ABBAABBA measured atmosphere .0913 → .0553 ms,
39.5% lower, but total render CPU 2.398 → 2.418 ms with overlapping ranges
and higher PPU time. No aggregate local Palace improvement is claimed. That
cohort and the ineffective first navigation cohort are retained. The final
candidate extends the same bound to radial navigation; final Palace images
and the Deck results below use that completed candidate.

### Steam Deck directional check

Both binaries warmed before short ABBA cohorts, two runs per variant, 2,000
ticks, Quality, three helpers, Vulkan/Wayland, hidden 1080x672 output. All
time/memory guards and replay/WRAM checks pass. Installed game/settings/saves
are untouched.

| Scene | Render CPU ms, control [range] | Candidate [range] | Reduction | Atmosphere ms | Cadence ms |
| --- | --- | --- | --- | --- | --- |
| Navigation | 2.362 [2.348–2.376] | 2.165 [2.155–2.176] | 8.3% | .1672 → .0721 | 3.569 → 3.367 |
| Palace | 3.305 [3.288–3.323] | 3.247 [3.240–3.255] | 1.7% | .0769 → .0451 | 4.497 → 4.434 |

These are directional CPU/cadence results, not GPU timestamps, visible Game
Mode FPS or battery measurements. Moving-navigation time windows cover
slightly different trajectory segments at different throughput; unrelated
geometry byte/count differences are not attributed to atmosphere culling.
Available RAM remains above 11.48 GiB, and maximum observed GPU allocation
growth is under 411 MiB, similar to the existing atlas-cache baseline.

## Evidence and remaining work

Local evidence: `/private/tmp/actraiser-atmosphere-occlusion.h4mKM3/`.
Final local navigation timing is `navigation-v2/`; `palace/` is the first
candidate cohort described above. `verify-*-v2/` and `verify-town/` hold final
local comparisons. Deck logs/guards/results are separately under `deck/`.
Remote evidence and raw Deck captures:
`/home/deck/argame/atmosphere-probe-20260912.nibDrF/`.

| Binary | SHA-256 |
| --- | --- |
| Mac control | `9d3b2ee15cd4ba3879f96329e07e59cc653063bad2b0b819cfdc1db1d42da243` |
| Mac final candidate | `75bb0217f146ba9e208b77cab06c3068fdf78718ea0d03caa08044f1e266ee58` |
| Linux control | `1543433a92fd086f130ace6041d5f22f07b79ad3d165c110d4b8ff2a9ec9091b` |
| Linux candidate | `2a5a3313c5d8ab1003eb4230a34a8713f7c9c446a267d2597c1477aa542d262f` |

This clears the 1% directional cutoff on Deck; it does not establish that
every remaining opportunity is exhausted. The independent native lava atlas
still has a small per-tick transfer (.067–.090 ms in these Deck scopes), and
Palace PPU/capture remains about 1.91 ms. SIM receives the shared upload,
parallel terrain, GPU cloud-composition and stable-view geometry-retention
work, but moving-town model projection is still CPU-owned. The globe's newer
camera-independent GPU projection and animation snapshots are not yet a town
renderer implementation.
