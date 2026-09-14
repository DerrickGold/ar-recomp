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

## Follow-up: restore the sprite-window visibility cue

The geographic focus above left the entire active town clear. It did not
replace the earlier ground dimming at the smaller, panning sprite-emission
window. That cue is now restored on the detailed curved ground and coastal
water, using captured margins, rounded corners, lead distance and bottom-edge
lift compensation. Existing cull-haze, out-of-range ground fade and darkening
settings control it; disabling the stage leaves the accepted visuals unchanged.
This is a ground visibility cue, not a change to gameplay exploration, sprite
culling, model lighting, HUD or clouds. Flat SIM remains unchanged.

The existing surface material evaluates the mask from independent chart
coordinates. Panning changes uniforms, not source meshes or model-cache keys;
no extra draw, texture, shader variant or runner ABI is introduced. The portable
surface contract adds bounded corner/inset distances, using previously unused
uniform components. Terrain shadow sampling continues to use its original UVs.

Follow-up evidence is in `runs/sim-globe-visibility-20260913/`: 174/174 desktop
tests pass, including 157,464 samples against the native cull predicate and
GPU checks for mask motion, off/restore, copied parameters and unchanged draw/
upload counts. Six-town fixtures retain 205 unaffected captures byte for byte;
only the six focus captures intentionally change. These are correctness and
resource-count checks, not a new throughput benchmark.
Three strict live Aitos replays also complete with identical simulation state:
the six fog-off composite captures match the preceding build exactly, and the
fog-on captures show the restored cue alongside actors, clouds and HUD.

## Fog performance acceptance — September 14

Deck-first follow-up: 16 timed runs, four per build in each of two camera
setups (ABBA ABBA), following two excluded warmups. Both builds use frozen
`c2b82449` sources and identical generated code; only the candidate includes
the fog-restoration patch. Concurrent recompiler/settings/event changes were
excluded. Both keep the haze stage enabled, so this measures the restored
sprite-window cue rather than disabling the existing geographic focus too.

Real visible Wayland/Vulkan at 1280×800, existing 15 W limit and automatic GPU
policy, three helpers, Unlimited presentation, the populated Aitos replay,
3,000 emulation ticks per trial and all effects enabled. Overview uses pitch
−0.75/distance 4.5/High detail; close-up uses pitch −1.35/yaw 0.35/distance 2,
Ultra requested, full cloud opacity and rim strength. Adaptive LOD remains on.

| Median | Before restoration | Restored fog | Change |
| --- | ---: | ---: | ---: |
| Overview, host presentations/s | 228.85 | 230.59 | +0.76% |
| Close-up, host presentations/s | 231.86 | 233.33 | +0.63% |
| Overview, presentation CPU ms/present | 1.0199 | 1.0188 | −0.11% |
| Close-up, presentation CPU ms/present | 1.0129 | 1.0121 | −0.08% |

No meaningful end-to-end slowdown was observed; the sub-1% throughput changes
are not a claimed optimization win. Overview trial ranges were 228.52–230.84
before and 230.40–231.35 after; close-up ranges were 231.66–232.09 before and
232.54–233.81 after. Median per-window p95 cadence was 11.551→11.569 ms for
overview and 11.583→11.506 ms for close-up (not pooled frame p95s).

All 16 final WRAM hashes match, with no settled SIM fallback or failed-draw
windows. Overview submitted vertex counts and draw counts were effectively
unchanged, with no added depth-copy traffic. Peak temperature was 61°C and
available RAM stayed above 11.31 GiB. Peak device-wide VRAM+GTT differences
were small and changed direction between workloads: 884→893 MiB overview,
921→914 MiB close-up; these are not process-exclusive allocation counts.

SIM re-presents again recorded alpha zero despite interpolation being enabled.
These rates measure host presentation throughput, not faster simulation or
230 distinct interpolated game frames per second. Gameplay remains near
60 ticks/s. CPU scopes are wall time, not isolated GPU shader execution time.
This follow-up measures these two SIM workloads, not every mode/device. Mac
performance follow-up was waived by the user based on this Deck acceptance;
Mac timing has not been repeated for the fog patch.

Evidence, full logs, hashes, resource guards, scripts and build provenance:
`runs/sim-globe-fog-deck-20260914/`. All benchmark processes exited; the installed
Deck game and its power policy were left unchanged. No commit is implied.

## Shipping fog defaults — September 14

The shared factory defaults now match the approved values in the developer's
current settings: 20% world haze, 48px cloud edge overlap, 0% sprite-window ground
fade, 30% out-of-range darkening, 0px corner rounding and 0% world defocus. The
unchanged values already match: cloud shroud/local-area haze/lift inset enabled,
35% cloud opacity, 96px cloud falloff, 48px cull lead, 16px ground ramp, 72px cloud
altitude and 100% drift. Square corners retain the soft edge ramp; zero ground
fade does not disable darkening.

These are compiled defaults for missing preferences and explicit reset actions,
not a migration of saved player choices. The installer does not ship the
developer's live `settings.ini`, and its stock config template does not override
these values. Settings tests cover save/reload, explicit older preferences,
environment precedence and reset; the pure focus test checks that the default
zero-haze rectangular mask still darkens the ground and preserves alpha. All
four focused checks (settings, settings overlay, render metadata and globe
focus) pass after the default changes.

The Deck batch above used the earlier explicit benchmark settings (35%
darkening, 10% ground fade, 96px corners); it is not a measurement of this exact
new factory preset. This follow-up changes only constants and introduces no
new rendering paths, resource contracts or draws.
