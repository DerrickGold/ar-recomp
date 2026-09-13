# Capture fast-path audit

Date: 2026-09-12. Baseline: `7f3f218b` (finite scrolling skybox fix).

## Scope

Layer capture does not bypass the entire optimized PPU renderer. The packed
capture renderer already resolves each source once, exports its requested
planes, and composites the surviving sources. The unnecessary exclusion found
here is inside its virtual Mode-1 tile resolver: requesting a band array prevents
the eight-pixel winner kernel, including the existing NEON/SSE2 implementation.

Ordinary captures used to request that array even without a custom band
classifier. The row was already initialized to `0xff` (use hardware priority),
and the resolver only wrote that same value back. They now pass no band
destination when the binding has no classifier. The initialized metadata remains
available to capture export, including hardware high-priority splits. Classified
layers still request metadata and retain their existing behavior.

A second eligibility correction ignores dormant large-tile virtual bindings
when the layer is disabled on both screens. Such a binding previously rejected
the whole optimized scanline, even though it cannot contribute any output.
Visible large-tile bindings keep their fallback.

Both decisions use private runtime state, not room IDs or host renderer details.
No public headers, ABI records, services, capture flags, texture formats, quality
settings, or presentation behavior change.

## Other exclusions inspected

| Gate / path | Assessment |
| --- | --- |
| Custom band callback + whole-tile virtual resolver | Next kernel candidate. Band labels must be written for nontransparent texels, independently of main/sub priority replacement. Simply removing the null-band guard loses authored classification. |
| Partial layer windows + whole-tile virtual/native resolver | Further opportunity: classify each tile against window-run boundaries, leaving only crossing tiles on the per-pixel path. Current gate is line-wide, even for unaffected tiles. Uniform whole-line windows are already optimized. |
| VRAM-backed margin span | Uses tile/provider batching but still applies pixels individually; no equivalent explicit whole-tile SIMD branch there. Investigate separately, preserving its store/ownership semantics and mirror seams. |
| Active large-tile virtual binding | Keep fallback: the virtual resolver assumes 8×8 entries, whereas the reference fetch accounts for large-tile subtiles and flips. |
| Tiled-mode mosaic | Already localized to a BG kernel on authentic rows. Policy-remapped margins still use the reference sampler for correct display-space mosaic phase. |
| Mode-7 mosaic / scaled host override capture | Keep fallback pending dedicated affine-mosaic / subpixel-ownership kernels. Not relevant to ordinary Mode-1 action rows. |
| Owning-screen winner / combined main-winner policies | Keep fallback: these need deferred winner semantics not implemented in the packed capture path. Plain main-winner and full-add export already have fast paths. |
| Full OBJ range capture | Already canonicalized to the ordinary OBJ cache; removing all 128 slots is already an empty-source shortcut. |
| Separate authentic output | Already shares packed source results when sampling matches. Camera/OBJ-offset differences still require an independent resolve. Hidden-source camera mismatches are a possible further narrowing, not changed here. |
| Explicit reference-renderer flag | Intentional independent diagnostic oracle (`AR_PPU_REFERENCE`), not enabled by ordinary layer capture or image screenshots. |
| Finite skybox background-view restrictions | Keep existing opt-in contract. Classified, remapped, windowed or mosaic sources cannot be blindly reused as an independently positioned live world view. |

Duplicate overlay clear/fill traffic is a separate memory-write experiment, not
a fast-path eligibility issue. Preserve primary/band aliasing, unbind/rebind and
content-mask behavior. The previously rejected line-local export trial is not
part of this change; see [its measurements](scanout-export-experiment.md).

## Verification design

The virtual capture fixture now compares independently sampled reference pixels
against the optimized renderer with and without a classification callback. It
covers full and partial tiles, horizontal/vertical flips, transparent texels and
provider gaps, raw/repeat/clamp/mirror margins, normal-scroll mirror seams,
vertical rows, main-only and subscreen-only ownership, full/half addition,
fixed-color subtraction, main-winner masks, live palette/brightness/scroll
changes, and a dormant large-tile BG2 binding. Main output, three capture planes,
per-row content masks and final winner scratch are checked. Mosaic cases retain
their separate source path. Provider span-use assertions distinguish an optimized
capture from accidentally testing the reference renderer twice.

Correctness and timings are separate. Replay timing uses serial ABBAABBA trials,
identical binaries/settings/ROM/replay/seed hashes, isolated writable saves and
settings, completion checks and same-mode final WRAM equality. Image capture,
profilers and builds do not run alongside timing trials. See
[the scanout plan](steam-deck-action-scanout-plan.md) for the starting evidence
and hardware qualifications.

## Correctness results

- All 36 runtime tests pass, including the independent PPU oracle, public ABI,
  installed consumer and finite-background-view coverage.
- The expanded PPU oracle also passes in optimized portable/SIMD-off and x86_64
  SSE2 builds. The primary runtime test uses ARM/NEON. SSE2 was executed via
  Rosetta on the Mac and subsequently natively on the Steam Deck. The Deck
  test links the exact candidate build's PPU, color-LUT and saveload objects
  with the existing focused-device event stub; it does not substitute a
  differently compiled PPU implementation.
- All three application runner/render-backend boundary checks pass.
- Aitos: six byte-identical final composites, game frames 1400–2400.
- Aitos at the maximum 64-row-per-side vertical extent: another six identical
  final composites over the same game-frame interval.
- Marahna (`saves/subscreen-access.rec`, warp 05/01): four byte-identical final
  composites, game frames 600–1200, including subscreen-additive layers.
- Fillmore (warp 01/01): three byte-identical final composites, game frames
  500–700, including the clamped skybox, 32 extra vertical rows and CRT.
- Each completed A/B image comparison also has identical final WRAM.

Fillmore's original dynamic-camera settings did **not** produce byte-identical
composites, including in an unchanged-control/unchanged-control comparison.
Disabling CRT alone did not fix the variation. `present.c` intentionally damps
dynamic-camera lean and kicks with wall-clock time, so differing presentation
timings yield slightly different projected pixels. Setting reactive strength to
zero in **private test settings only** makes the A/B captures exact with CRT and
all the original effects enabled. No camera or image-quality workaround was
added to production. The first Marahna capture interval was also rejected because
its game clock had not reached the requested final shot by the host tick limit;
the completed interval above is the accepted comparison.

Evidence is under `/private/tmp/actraiser-capture-fast-path.rpepqO/`, including
the copied control/candidate binaries, a fixed copy of the replay harness,
isolated reproduction settings, per-run logs, input hashes and image hashes.
The baseline binary SHA-256 is
`85c111732afca0bab73efc4485f891ef8871c1ad3c29082e34cba86a2109c8cc`;
the candidate is
`e3949fe24953abcc498da12ca804f3484034b485ea467b7a7c5a402678d06b8d`.

## Timing

The initial `aitos-no-classifier` batch was rejected: the shared benchmark script
changed during a run, and another task was concurrently benchmarking the game.
Its partial numbers are **not** performance evidence. A second batch
(`aitos-clean`) overlapped a separate graphics-test run and was stopped after two
trials; it is also rejected. A final attempt (`aitos-guarded`) used before/after
process checks and refused to start because another game benchmark was active.
No speedup percentage or performance-regression conclusion is supported by
these partial/confounded Mac samples. The separate Deck comparison below is
the accepted timing evidence.

### Native Steam Deck comparison

Eight serial ABBAABBA trials completed on the user's OLED Deck (AMD Custom APU
0932, SteamOS 3.8.16), followed by two separate image-validation trials. Each
run completed exactly 3,000 tick-presents with no re-presents. The pinned Aitos
replay uses scene `Action 3D`, room `04/04`, 16:10 square pixels, a hidden
1080×672 Wayland desktop window, interpolation off and zero vertical extension.
Enhanced action effects remain enabled. Three helpers were requested equally;
the settled action path reports no dispatched helper jobs.

Both binaries were freshly built through the hermetic Linux shipping build
path, Zig 0.16.0, `x86_64-linux-gnu`, `-O2`, SIMD enabled (SSE2), deep
instrumentation and recorder excluded. Control is the isolated `7f3f218b`
snapshot; candidate changes only the two PPU production decisions above. The
candidate reused 391 of 392 translation units and rebuilt only `ppu.c`.
Both use the same generated sources, assets, libraries, ROM and fixture inputs.
The old installed Deck executable was neither used as control nor replaced.

| CPU wall scope | Control median [min–max], ms | Candidate median [min–max], ms | Median reduction |
| --- | --- | --- | --- |
| PPU scanout (nested) | 3.975 [3.798–4.050] | 3.543 [3.445–3.670] | 10.9% / 0.432 ms |
| Action scanout (same operation, nested) | 3.975 [3.798–4.050] | 3.542 [3.444–3.669] | 10.9% / 0.432 ms |
| PPU + capture | 4.042 [3.859–4.122] | 3.609 [3.505–3.742] | 10.7% / 0.433 ms |
| Upload | 1.140 [0.966–1.223] | 1.096 [1.050–1.283] | Inconclusive |
| Render CPU (tool-defined sum) | 5.484 [5.097–5.675] | 5.018 [4.851–5.380] | 8.5% / 0.466 ms |

Every candidate's scanout average is below every control's. Total render CPU
has overlapping ranges; its median benefit is less precise. Scanout is nested
inside PPU + capture, not an additional cost. These are CPU wall scopes on the
actual Deck, **not GPU timestamps or a visible Game Mode FPS gain**. The
reducer discards the first matching window and frame-weights the last five:
720–826 measured presents per trial. Wall-time reporting yields different
window boundaries, so this is not an exact frame-by-frame microbenchmark.

Upload's nominal median drop is only 0.044 ms (3.9%), with strongly overlapping
ranges; this patch does not establish an upload improvement. The user's older
7.11 ms PPU + capture, 7.03 ms action scanout and 4.74 ms upload values have not
been matched to a source binary, sampling cadence and exact settings. The user
subsequently identified Fillmore as their source; see the Fillmore follow-up.
Current
values are lower, but an all-optimizations speedup percentage cannot be inferred
from that unpaired comparison. In particular, normal visible rendering may
present retained/interpolated frames between PPU updates: report CPU time per
scanout call separately from average cost per host presentation.

The Deck remained on AC, with its existing 15 W cap, `powersave` CPU governor
and automatic GPU clocks. Steam and the desktop compositor remained active;
clocks were not locked. No other game/test/build was observed by the process
guards. No capture, profiler or build ran during the timing batch. Peak sampled
APU temperature was 55°C, minimum available RAM 11.596 GiB and peak combined
GPU VRAM/GTT allocation 545.137 MiB. Allocation returned to 390.816 MiB after
each run. No 6 GiB available-RAM / 2 GiB GPU-growth guard, timeout or logged
graphics failure occurred.

The independent PPU oracle passed natively on Deck. Separate strict final
composite captures at game frames 1400, 1600, 1800, 2000, 2200 and 2400 were
byte-identical; frame 2000 was also visually inspected. All ten replay runs
have final WRAM SHA-256
`c97d01974a22892ec1c5d208eb0dcba636f5567eb8d5d770ab31a5f5bf275187`.

This route exercises unclassified world-backed capture (`hle=03`, zero band
cache builds/hits). It supports keeping the no-classifier optimization, not a
separate measured gain from the dormant-large-tile correction or an estimate
for custom-classified layers, every room, interpolation or different zooms.
Keep broader kernels as separately measured experiments.

The [archived evidence and reproduction commands](
evidence/capture-fast-path-deck-2026-09-12/README.md) include all trial logs,
input hashes, guard records, result JSON, build logs and the frozen diagnostic
harness. Full binaries and run bundles remain under
`/private/tmp/actraiser-deck-capture.ViJHxd/` locally and
`/home/deck/argame/capture-fast-path-20260912.XW1iYR/` on Deck.

### Visible fullscreen FPS follow-up

At the user's request, a separate four-run ABBA comparison used **normal
non-headless gameplay**, borderless fullscreen at the panel's 1280×800, with
interpolation enabled and Refresh rate set to Unlimited. No image readback or
profiler ran. The user confirmed the rendering was visible. Read-only session
checks during the batch reported the Wayland seat active, lock/screensaver off,
panel DPMS On and the desktop not being shown over application windows. This
remained the KDE desktop session, not Game Mode.

| Visible, interpolated, unlimited | Control | Candidate | Change |
| --- | ---: | ---: | ---: |
| Host FPS, median [run range] | 386.55 [373.54–399.55] | 432.90 [431.52–434.28] | +12.0% |
| PPU + capture, ms/call | 5.745 | 5.091 | −11.4% |
| PPU scanout, ms/call | 5.658 | 5.007 | −11.5% |
| Action scanout, ms/call | 5.657 | 5.007 | −11.5% |
| Upload, ms/call | 3.184 | 3.172 | Essentially unchanged |

Each trial completed 3,000 emulation ticks in approximately 50.5 seconds, and
all four final WRAM hashes matched the hidden runs. A run's FPS is the interval-
weighted rate from its last ten settled 04/04 reporting windows, after dropping
entry warmup; the table takes the median of the two runs per build. This is a
small targeted comparison, not a confidence interval or a whole-game claim.

These are **completed host presentations**, including interpolated/repeated
presents between roughly 60.1 Hz authentic game updates. The 90 Hz panel cannot
display 433 distinct frames each second. The high FPS is not uncapped emulation
and was not calculated as the reciprocal of PPU time. One control trial had
2,998 tick-presents for 3,000 ticks because catch-up can consume multiple ticks
before presenting; the visible harness validates the final emulation ordinal
and permits this normal behavior, unlike the headless harness.

CPU stage totals are divided by each stage's actual call count here. Dividing
instead by every presentation would show only 0.707 ms PPU + capture and
0.441 ms upload for candidate because most presents reuse an existing capture.
Those are average costs per presentation, **not** 0.707 ms full PPU work or a
corresponding upload optimization. The hidden one-tick-per-present run and
visible unlimited/interpolated run also impose different CPU/GPU load and clock
conditions. Compare the matched columns within each operating point, not their
absolute costs against one another or against unpinned historical readings.

Visible evidence and the separate cadence/call-count reducer are archived under
`visible-unlimited/` and `visible_deck_benchmark.py` in the linked evidence
directory. The reducer's synthetic checks cover cadence weighting, per-call
normalization and rejection of reported failed presents. Production FPS, pacing,
quality settings and renderer code were not modified for this measurement.

A subsequent visible Vsync pair completed at **90.00 FPS control / 90.01 FPS
candidate**, matching the panel's 90 Hz target. Both used interpolation and
3,000 authentic ticks; final WRAM remained identical. PPU scanout was 5.493 →
4.960 ms/call, PPU + capture 5.579 → 5.048 ms/call, and upload 3.083 →
2.976 ms/call. This is one trial per build, so the small upload difference is
not evidence of an upload optimization. The benefit here is CPU headroom,
not an increase above the display cap. The screen-lock query returned false
before and after the pair; no rendering error, timeout or memory guard fired.
Raw evidence is `visible-vsync/` in the archive. Both benchmark processes exited
normally; the installed executable hash remained unchanged afterward.

### Fillmore: the level behind the user's original readings

The user identified Fillmore as the source of the old ~7 ms scanout readings.
The prior skybox reproduction was recovered: `saves/fillmore-act.rec`, private
seed SHA-256 `d02d7e3fad4bba785c31bb257e9dcf7f3c439e6b9b1d755fc1a9e8b1ec0461f2`,
warp 0101 at game frame 400, with the original `skybox-settings.ini`. This
uses 32 extra vertical rows per side, Dynamic Cam (reactive strength 40),
Plane + skybox, CRT, enhanced action effects, 16:10 square pixels and enhanced
localization. Interpolation is **off** in this recovered preset. Dynamic camera
behavior was preserved for timing, not frozen as in the earlier pixel oracle.
Fullscreen output remains 1280×800, normal gameplay rather than headless.

Four new visible ABBA trials completed 2,000 emulation ticks each, in ~34.1 s,
with the last ten settled `Action 3D`, `01/01` reporting windows reduced using
the same interval-weighted FPS and per-stage-call normalization as Aitos.

| Fillmore, interpolation off, Unlimited | Control median | Candidate median | Change |
| --- | ---: | ---: | ---: |
| Host FPS [run range] | 381.68 [380.25–383.11] | 414.33 [409.96–418.70] | +8.6% |
| PPU + capture, ms/call | 7.484 | 6.745 | −9.9% |
| PPU scanout, ms/call | 7.385 | 6.644 | −10.0% |
| Action scanout, ms/call | 7.384 | 6.644 | −10.0% |
| Upload, ms/call | 0.935 | 0.940 | Unchanged |

This reproduces a control scanout cost much closer to the user's reading and
demonstrates a ~0.74 ms PPU reduction from the two PPU edits alone. These FPS
values include retained-frame redraws between authentic updates; interpolation
is off, and the 90 Hz panel still cannot display 414 distinct images/s.

The recovered preset's current upload cost is far below the old 4.74 ms value,
but control and candidate are effectively identical. Thus this patch establishes
no upload improvement. Matching the historical interpolation setting remains
necessary before attributing that older difference: visible interpolated Aitos,
for example, measured ~3.17 ms/upload call while this non-interpolated Fillmore
fixture measures ~0.94 ms. They are different workloads, not an upload A/B.

All four runs have final WRAM SHA-256
`e9077187b73d23a81e9da30cd0e4cf895e5c4391facfcd9e826c50cbbb08ba54`;
no memory guard, timeout or logged graphics failure occurred. Scene logs confirm
`hle=03`, zero band-cache builds/hits and a successfully initialized CRT shader.
The previous independent PPU tests and frozen-camera pixel comparisons remain
the visual correctness evidence; no screenshot readback accompanied these
dynamic-camera timing runs.

Evidence: `fillmore-unlimited/`, `visible_fillmore_benchmark.py` and
`fillmore-skybox-checkpoint.json` in the archive. The full original settings and
seed remain in the isolated local/Deck fixture directories named by the hashes
and manifest; they are not shipped game defaults or changes to user settings.

The same Fillmore preset then completed a separate visible 90 Hz Vsync pair:
89.95 FPS control, 89.99 FPS candidate. PPU + capture was 7.182 → 6.548 ms/call;
action scanout 7.082 → 6.451 ms/call (PPU scanout 7.083 → 6.451). This operating
point closely reproduces the user's ~7.1/~7.0 ms starting PPU readings while
showing ~0.63 ms of CPU headroom from the patch. Upload was 0.764 → 0.800 ms/call,
again not an improvement. This pair contains only one run per build, so the
four-run Unlimited comparison remains the repeated timing evidence. Both final
WRAM hashes match the other Fillmore runs; no guard or rendering failure was
reported. The installed executable hash remained unchanged and the desktop
was unlocked after testing. Evidence is archived in `fillmore-vsync/`.

### Fillmore with interpolation enabled

At the user's request, the same visible Fillmore runs were repeated with only
`AR_INTERP_ENABLE=1` changed (apart from private per-run output/save/settings
paths). Binaries, replay, seed, config, layer INI, fixture settings and diagnostic
harness hashes match the preceding interpolation-off batch. CRT, dynamic
camera, skybox, 32-row vertical extension and 1280×800 fullscreen are unchanged.

| Fillmore, interpolation on, Unlimited | Control median [run range] | Candidate median [run range] |
| --- | ---: | ---: |
| Host FPS | 349.50 [348.26–350.74] | 378.46 [378.37–378.55] |
| PPU + capture, ms/call | 7.513 [7.492–7.534] | 6.743 [6.723–6.762] |
| PPU scanout, ms/call | 7.413 [7.391–7.434] | 6.643 [6.625–6.661] |
| Action scanout, ms/call | 7.412 | 6.642 |
| Upload, ms/call | 1.305 [1.300–1.309] | 1.320 [1.319–1.322] |

Four serial ABBA trials establish the same direction: 10.4% lower scanout time
(0.770 ms/call), 10.3% lower PPU + capture and 8.3% higher uncapped host FPS.
Upload is not improved. The FPS includes interpolated/retained presentations,
not faster simulation or 378 distinct images displayed on the 90 Hz panel.

A separate Vsync pair completed at 89.99 FPS control / 90.01 FPS candidate.
PPU + capture was 7.174 → 6.455 ms/call; PPU scanout 7.072 → 6.358 ms/call;
action scanout 7.071 → 6.357 ms/call; upload 1.097 → 1.117 ms/call. As before,
this pair is a panel-target check, not four repeated measurements per build.

All six runs completed exactly 2,000 emulation ticks, passed the graphics and
memory guards, and ended with the same Fillmore WRAM hash as the earlier runs.
The desktop was unlocked before/after testing and the installed executable's
hash remained unchanged. No profiler, screenshot capture or build ran beside
the timing trials. Archives: `fillmore-interp-unlimited/` and
`fillmore-interp-vsync/` in the evidence directory.

Interpolation increases candidate upload from the prior 0.940 to 1.320 ms/call
in Unlimited mode. The current nested `action analysis` scope is 0.382 ms/call,
including CPU endpoint copies, GPU endpoint-copy recording and motion analysis;
ordinary `action upload` is 0.573 ms/call. Neither is additive to their parent
upload scope, and neither is an exclusive GPU timing. The historical 4.74 ms
upload value is **not reproduced with interpolation either on or off** in this
fixture. Its remaining binary/settings/sample differences are unresolved;
do not claim a cumulative upload speedup percentage from that older value.

Source review and the current breakdown prioritize PPU work over interpolation
analysis for this scene. Proposed overlay color-table reuse, finite-skybox
initialization/edge batching, bounded scratch/export changes and exact motion
scoring improvements are described in the [updated optimization plan](
steam-deck-action-scanout-plan.md#updated-priorities-after-visible-fillmore-interpolation-testing).
These are hypotheses for subsequent isolated experiments, not additional
production changes or promised gains from this benchmark turn.
