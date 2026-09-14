# Continuous SIM globe: performance acceptance and cutover

September 13, 2026. The six-town visuals were approved before this comparison.
The shipping choices are now **flat SIM** (globe setting off) and **continuous
globe SIM** (on), using the approved 3× navigation radius and 40% factory
landscape height. Explicit saved settings are preserved.

The prototype compilation switch, plateau compositor, flat-footprint/collar
mapping, whole-image focus treatment, and obsolete retained-image cache have
been removed, including their dedicated tests. CMake and the hermetic release
manifest compile the same continuous implementation normally. The surviving
test-only globe adapters exercise raw/facing/detailed spherical sources; none
renders a plateau. Model, mesh, atlas and terrain residency remain intact.

## Repeated measurements

Fresh control/candidate builds included the final approved water transition,
grounded billboards and directional rim lighting. Each pair differed only in
the then-private continuous-composition switch. Steam Deck testing started
first; Mac testing then ran concurrently on the separate device. Only one game
was launched per device; our local builds completed before timed Mac batches.
The Deck harness also rejected competing game/build processes.

| Test | Previous connected plateau | Continuous globe | Result |
| --- | ---: | ---: | --- |
| Deck, overview, host presentations/s | 187.51 | 229.30 | +22.3% |
| Deck, close/low-angle, host presentations/s | 192.62 | 231.50 | +20.2% |
| Mac, overview, render CPU ms/tick | 2.8079 | 2.7693 | −1.4%; overlapping ranges |
| Mac, close/low-angle, render CPU ms/tick | 2.7255 | 2.7306 | +0.2%; negligible |

Deck overview: four runs per variant, ABBA ABBA. The old/new throughput ranges
were 180.64–188.84 and 227.98–229.96 presentations/s respectively. The median
per-window p95 cadence improved from 17.12 to 11.55 ms (not a pooled frame p95).
CPU composition fell from 1.638 to 1.031 ms/present; upload processing fell
from 3.497 to 2.296 ms/call. The Deck close-up check used ABBA, two runs each,
with non-overlapping throughput ranges (190.93–194.31 vs 230.78–232.22).

Each Mac comparison used four runs per variant, ABBA ABBA. Both variants hit
the approximately 120 Hz compositor ceiling, so these runs do **not** establish
a displayed-FPS gain. Overview composition CPU improved from 0.875 to 0.736
ms/present even though total render CPU was close. An initially higher close-up
sample did not persist across the alternating repeats.

## Workload and measurement limits

- Deck: real visible Wayland/Vulkan fullscreen at **1280×800**, Unlimited
  presentation, with the normal fixed-timestep host loop. Mac: real hidden
  Cocoa/Metal presentation at **1440×896**, one emulation tick per present.
  Earlier hidden Deck results at 720×448/1080×672 are not final evidence.
- Both use three helper workers, the same isolated Aitos eruption replay and
  fixture SRAM/settings, 3,000 emulation ticks per run, and restored inputs.
  Final WRAM hashes match within each comparison. No measured settled SIM
  window reported a fallback or failed draw.
- Overview: pitch −0.75, yaw 0, distance 4.5, High models with Adaptive LOD,
  materials/AO, shadows/soft shadows, particles, effect lighting, rim light,
  clouds, haze/dimming and backdrop enabled. Stress: pitch −1.35, yaw 0.35,
  distance 2, Ultra detail requested, cloud opacity and rim strength 100%.
- The Deck interpolation setting was requested on, but SIM's recorded
  re-presents used alpha zero. The quoted rates are **host presentations**, not
  faster game simulation or proof of newly generated intermediate SIM frames.
  Gameplay remains approximately 60 ticks/s.
- CPU scopes measure wall time, not GPU execution time. GPU timestamps were
  unavailable. The Deck remained at its existing automatic power policy;
  observed peak temperatures were 60–62°C, with at least 11.3 GiB available RAM.
- Peak device-wide driver-reported VRAM+GTT sum decreased from about 1,334 to
  884 MiB in the overview batch (not a process-exclusive allocation measure).
  Mac overview dynamic depth traffic decreased from about 0.802 to 0.599
  MiB/present while more resident geometry was submitted. Multicore support
  was not reduced to obtain these results.
- This is a populated Aitos performance gate, not a benchmark of every town,
  every miracle, or every device. The approved Northwall visual fixture remains
  sparsely populated. Windows was not executed in this pass.

## Cutover verification

The cleaned-up default compiles on Mac and through the Linux hermetic builder.
Eight live close-up composite captures match the approved candidate **byte for
byte**, including globe Off at GF1300 and On at GF1600; final WRAM also matches.
The final desktop/Metal suite passes **173/173**, with no skips (one obsolete
image-cache test was deleted, not disabled). Both final Deck checks complete
3,000 ticks at native resolution with reference-identical WRAM, including the
flat/globe toggle. All **211/211** final six-town globe captures match the
approved images byte for byte, including motion and camera-limit views. Their
hashes and fixture logs are retained alongside the evidence. No release build
or commit is implied.

Raw results, full per-run logs, worker settings, input/binary hashes, Deck
resource guards and analysis are retained under
`runs/sim-globe-performance-20260913/`. Mac reports distinguish render CPU from
present/wait; Deck reports distinguish tick work from re-presentation work.
The earlier gallery remains under `runs/sim-globe-visual-acceptance/` as immutable
historical evidence, without keeping the old renderer to regenerate it.
