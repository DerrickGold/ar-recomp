# Mario Paint first-pass bring-up — 2026-09-09

## Latest: title click and basic drawing pass without authored entries

The recorded title click completes 1,200 frames and reaches the painting
editor. A 1,500-frame click-and-draw replay produces visible red marks on its
canvas. Both repeat with identical CPU/state/pixel/audio-count/semantic-edge
reports, zero hard diagnostics, and one balanced native NMI per frame.

The generic polling seam services the original NMI counter wait. Generated
direct-call return ownership now accepts native callee-clean returns to the
already-active caller and preserves their actual final stack pointer. An
explicit reset-entry scope handles reset's initial stack relocation without
relaxing ordinary caller or nested interrupt boundaries. No game-specific
stack offset, PC rule, configuration entry, or HLE was added to the compiler.

Mario Paint's adapter and generated code are installed and both interactive
and headless binaries rebuilt. Run `./build/MarioPaint mp.sfc` from its project
directory. `tests/title-click.csv` and `tests/title-click-and-draw.csv` retain
the regression workloads. Its ROM, `recomp/`, and analysis database are
unchanged: 1,803 variants, zero authored functions/dispatches/widths/HLEs.

Full Go tests and 35/35 runtime tests pass. ActRaiser and Battletoads were
regenerated and replay-tested against frozen controls with matching state
and semantic dispatch. No material runtime slowdown was measured; linked
binaries grew 6.5–15.3%, an optimization opportunity rather than a size-neutral
result. Full game/tool coverage and audio fidelity remain unverified.

See [callee-clean validation](CALLEE_CLEAN_VALIDATION.md) for the contract,
hashes, limitations and reproducible checks. Current artifacts are under
`build/callee-clean.NCVuAh/` in the enclosing ActRaiser checkout. Older sections
below are historical; their temporary artifact directories may no longer
exist and their failure/installation status is superseded by this section.

## Historical: recorded title click and cooperative polling

The native return-table helper fix now completes 300 idle frames and a
900-frame scripted cursor run, still with zero authored functions/dispatches/
width overrides/HLEs. It recognizes a complete native helper contract,
executes that helper, and uses its actual target instead of returning into
inline table data. Discovery recovers eight sites and 97 open-prefix table
references, not closed selector bounds or proven reachability. There are
1,803 generated variants. That candidate is installed in the Mario Paint
project; ActRaiser and Battletoads regeneration remained byte-identical in
that phase.

The user's actual title-click recording then exposed a separate NMI counter
wait at `$01:D38E`, outside the adapter's original scheduling PC. The new
generic polling seam resolves that watchdog **in an isolated candidate**:
real compiled NMIs drain the counter over sixteen frames, without a config
entry or host-written flag. The recording advances through 265 frames before
a callee-clean return defect blocks editor entry at frame 266. Thus the
full click replay is still failing; no first-shot/playable claim is made.

See [poll-wait validation](POLL_WAIT_VALIDATION.md) for the exact contract,
reproduction, cross-game results, performance checks and next blocker.
The production Mario adapter/generated files are not replaced by that
polling candidate. The sections below are historical results, not the current
failure location.

## Historical: seventh-NMI defect fixed without configuration additions

See [paired-tail validation](PAIRED_TAIL_VALIDATION.md) for the implementation
and synthetic generated-C conformance tests. The original paired-tail
continuation failure is fixed; watchdog frame starts inside checkpoints now
also preserve the active execution tracking. Local gotos and direct calls
remain fast paths, and same-activation paired tail chains use one driver.

Mario Paint regenerates to the same 1,411 variants with its original zero
authored entries/HLEs and unchanged analysis database. Generated semantic
hash: `63647f3d7873ab5443e19bec2f5b91c27d1efb83b74fb05aaae1df94153d16f9`.
It completes 23 NMIs with 23 successful context checks. The 23-frame
recording/replay matches WRAM `39571DD0`, SRAM `011FFCA6`, pixels `E5BDB074`,
CPU, and semantic edges `46376:CF1DD326AB20A6E3`. The original six-frame prefix
remains unchanged.

The capture now shows the title screen with a hand cursor. Full uncached Go
tests and 35/35 strict runtime tests pass. ActRaiser and Battletoads were both
regenerated and replay-validated against frozen controls, preserving authored
config/HLEs. Final warmed A/B results: ActRaiser suite +0.61%, Battletoads idle
−0.27% / stress −0.40%; no material regression or hard diagnostics. Mario
Paint's `gen/` is updated to this candidate, with the original failed control
preserved separately; its ROM, `recomp/` and analysis database are unchanged.

The 300-frame acceptance run still fails after those 23 frames: an inline
table dispatcher at `$01:E393` rewrites its stacked return PC, but generated
RTL treats stack-depth equality alone as an ordinary host return. The caller
resumes into table data at `$01:9734`, eventually trapping at `$01:973C` with
a garbage target `$48:0857`. This requires generic return-address ownership
and inline-table recovery, not more authored functions or a game HLE. No
playable/first-shot-pass claim is made. Current artifacts are under
`/private/tmp/snesrecomp-paired-tail.Lzx8P2`; earlier controls remain intact.

## Frame/NMI adapter follow-up — first-shot runtime FAIL

The adapter is now wired in `/Users/derrick/Documents/Programming/MarioPaint`.
Its original **1,411 variants and zero authored roots/dispatches/width overrides/
HLEs are unchanged**. It has a headless validator and SDL3 native presentation,
relative mouse/buttons, queued audio and deterministic mouse record/replay.

Compiled reset/native SPC upload and six NMI/frame transactions complete.
The seventh NMI fails on `$01:E746 -> $00:80FF`: a branch to a separately
emitted body loses its paired caller-return context. `tailCallStatement` in
`internal/emitter/function.go` inherits `_entry_s/_hrv` only on its unpaired
path; the paired path starts registry dispatch without that inheritance.
The callee's RTL then tries to dispatch the already-active NMI continuation
as a new function. `dispatchReturnTransfer` in `internal/emitter/dispatch.go`
contains the same split and merits the same synthetic regression coverage.
No compiler workaround or `func 0080FF` was added. This result is not a
discovery miss, not a passing first-shot recompile, and not playable yet.

Scheduling uses the new optional `RtlGameExecutionApi.execution_checkpoint`
at the recovered wait block `$01:E2EB`. It runs the actual NMI and returns to
the same active C call, allowing the ISR's real `STZ $016A` to release the
wait. It never clears that flag in the host, restarts reset, or longjmps at
normal frame boundaries. Native CPU register/status/stack/context and saved
hardware return-address checks guard interrupt completion. The shared hook
contains no Mario Paint addresses or scheduling policy. V2 table extents
remain valid; existing games leave the callback null.

Validation directory: `/private/tmp/mariopaint-frame-adapter.CQWM57`.
See the Mario Paint project's `README.md` and `BRINGUP.md` for build commands,
the recovered schedule, precise failing path and limitations.

- Strict runtime 35/35 C/C++ tests and full `go test ./...`: pass.
- New synthetic hook contract: 10,000 resumptions, no recursive scheduling
  or activation growth, exactly-once memory effects, observable nested ISR
  blocks, restored CPU context, unchanged V2 prefix and null-hook behavior.
- SDL/headless build and mouse-script parsing tests pass. The SDL window,
  relative capture and audio device initialize and complete six frames;
  all printed state/dispatch results equal headless for the same input.
- Six-frame idle recording/replay: WRAM `B60AE0F4`, SRAM `011FFCA6`, pixels
  `AA8E0D21`, semantic edges `45806:D9BB3E3B7BAD8ED6`; CPU snapshots match.
- Scripted mouse movement and both buttons change game WRAM to `3113850F`;
  its recording/replay matches exactly. Interactive drawing is not verified.
- The 300-frame acceptance run fails deterministically during NMI 7 after
  six completed frames: WRAM `46619593`, edges `45879:44ED3723AF3326F8`.
  One dispatch miss plus the adapter's incomplete-interrupt error produce
  exit 1. No watchdog is used to pace or advance frames.
- Scanout is still black and PCM still zero in the validated prefix; neither
  successful rendering calls nor successful audio device initialization
  proves playable visual/audio content.
- ActRaiser regeneration is byte-identical (4,647 variants; hash below).
  All five representative replays match WRAM/SRAM/CPU/semantic dispatch.
  Three adjacent A/B pairs after warmup: suite time **−0.61%**; per-workload
  median deltas −1.37% to +0.08%, no material regression or hard diagnostics.
- Battletoads 1,800-frame idle / 3,600-frame stress outputs are byte-identical;
  three warmed A/B pairs give **+0.43% / +0.20%**, no hard diagnostics.

These changes and observations are uncommitted. The original ROM/generated
files and unrelated working-tree changes have been preserved. The next step
is a generic, fixture-tested split-tail return-context fix, then regenerate
an isolated Mario Paint candidate and rerun cross-game validation. Do not
register active continuations as normal functions to sidestep the failure.

## Previous boot-only milestone — result and scope

**Generation and compilation succeed; this is not yet a running game.**
The first vector-only boot stopped at the shared AABB-only SPC bootstrap.
After adding an original native upload protocol, the same generated code
passes that upload and waits for an NMI-updated flag. The deliberately minimal
headless probe has no frame/interrupt adapter. No graphics, audible audio,
interactive mouse gameplay, or subsequent gameplay reachability is claimed.

This is the next useful integration target, not evidence that all 38 emitted
unresolved indirect paths execute or that zero authored declarations will be
sufficient for the whole title. No trap is suppressed to advance the probe.

The shared runner changes add serial pad/mouse input and native SPC upload;
no title-specific PC, RAM policy, HLE, renderer, authored config, or generated
game code was edited in the shared repository. Existing materialization and
return-bit analysis edits were preserved. The boot-probe sources, automatic
config, generated C, database, README, and this report were copied into the
MarioPaint project without changing its original ROM. Validation work is under:

`/private/tmp/snesrecomp-mariopaint.TjtL2P`

## Local cartridge identity

Supply your own dump; no ROM or derived game code is committed.

- Local file: `/Users/derrick/Documents/Programming/MarioPaint/mp.sfc`
- Title: `MARIOPAINT`; Japan, revision 0, LoROM, 1 MiB, 32 KiB SRAM.
- SHA-256: `e842cac1a4301be196f1e137fbd1a16866d5c913f24dbca313f4dd8bd7472f45`
- CRC32: `38C9626C`; header checksum/complement `4B9E/B461`, verified.
- Reset `$00:8000`, native NMI `$00:80D4`, native IRQ `$00:80AF`.

The staged `recomp/` has one `bank` directive per bank and `auto_vectors`
in bank 00. There are **zero authored functions, dispatches, width overrides,
or HLE definitions**. Native interrupts receive live-width variants.

## Reproduction

From the compiler checkout (substitute your own isolated output paths):

```sh
go build -o /tmp/v2regen ./cmd/v2regen
/tmp/v2regen analyze --rom /path/to/mp.sfc --cfg-dir /path/to/recomp \
  --jobs 8 --format json --out-analysis /path/to/analysis-db.json
/tmp/v2regen regen --rom /path/to/mp.sfc --cfg-dir /path/to/recomp \
  --analysis-db /path/to/analysis-db.json --out-dir /path/to/gen \
  --funcs-out /path/to/recomp/funcs.h --allow-stubs --jobs 8
```

The staged `CMakeLists.txt` and `src/main.c` build `MarioPaintHeadless`, a
**bounded boot probe**, not an interactive frontend. It attaches a mouse on
port 1 through the public API, invokes compiled reset, and reports the first
watchdog stop. Its nonzero exit is expected until the frame adapter exists:

```sh
cmake -S /private/tmp/snesrecomp-mariopaint.TjtL2P \
  -B /private/tmp/snesrecomp-mariopaint.TjtL2P/build-after \
  -DCMAKE_BUILD_TYPE=Release -DSNESRECOMP_ENABLE_IPO=OFF
cmake --build /private/tmp/snesrecomp-mariopaint.TjtL2P/build-after -j 8
/private/tmp/snesrecomp-mariopaint.TjtL2P/build-after/MarioPaintHeadless \
  /Users/derrick/Documents/Programming/MarioPaint/mp.sfc
```

## Evidence

Shadow analysis discovers 494 variants in seven passes and 13 automatic open
facts. The persisted database contains zero proven facts; supplying it selects
the existing exact-state generation path, not a new behavior of this patch.
Regeneration emits 1,411 variants, 64 files, 38 raw unresolved indirect
emissions, and 75 stub markers. Semantic generated-source SHA-256:

`1e24dc0a62717a1607275498a31d0491afcca78750ef9c7a5a3744a38051a25a`

Before: compiled `$01:DF25` waits for `$CC` from the bootstrap after its
ready marker. WRAM CRC32 at timeout `7B518E8A`.

After: the SPC is executing uploaded code at `$050A`, and the CPU has advanced
to `$01:E2CE`, specifically the loop at `$01:E2EB` reading `$00:016A`.
This routine sets the flag and waits for interrupt-driven completion; the
probe supplies no interrupt. WRAM CRC32 `3DF3220F`, SRAM CRC32 `011FFCA6`.
The watchdog's wall-clock cycle count is not a deterministic checkpoint.

The next frame adapter must preserve active compiled continuations and real
interrupt stack/M/X semantics. Do not clear `$016A` from the host, add an
unconditional yield-and-return HLE, or treat the watchdog longjmp as a normal
frame boundary: those would skip the actual game logic.

## Shared contracts and regression checks

See `runtime/docs/INPUT_AND_BOOT.md` in the compiler checkout for the runner
input/boot integration contract.

- Full `go test ./...`: pass.
- Strict-warning standalone runtime: 35/35 C/C++ tests pass. The two new
  suites cover serial devices and original SPC protocol fixtures.
- Public input header compiles independently; ABI offsets are asserted.
- Native-layout v11 and portable v12/v13 snapshot paths are tested. Host
  legacy tail extent/alignment remains 50 bytes / 10-byte double offset,
  identical to the pre-change runtime. Mid-packet v13 state restores exactly.
- Semantic digest explicitly advances to schema 3 and includes pending mouse
  motion. Old boot-ROM snapshot PCs restart the new firmware; uploaded/hidden
  ARAM execution is not redirected. Existing pad replay streams are unchanged
  and do not gain mouse records implicitly.
- Fresh ActRaiser generation is byte-identical to the frozen control: 4,647
  variants, semantic source hash
  `a765bd3d6bead8677de2e114d9336d4d046d25a4b6f4bbeb891899a1670f2c20`.
- All five ActRaiser replay workloads match WRAM, SRAM, CPU, and semantic
  dispatch artifacts with no hard diagnostics. Both candidates use the same
  frontend, generated C, trace/watchdog flags, and ThinLTO settings.
- Battletoads 1,800-frame idle and 3,600-frame stress runs retain byte-identical
  output, including their CPU/WRAM/framebuffer/audio/dispatch checks. Three
  measured adjacent pairs after warmup in the final rebuild: median deltas
  −0.28% / −0.24%; one early, non-repeating timing outlier is retained in the log.
- ActRaiser final correctly configured three-pair benchmark: +0.90% suite,
  every workload below the existing regression threshold. Final rerun logs
  are `actraiser-final-ab.log` and `.json` under the staging root.
- `git diff --check`: pass. No production game regeneration directory changed.

The earlier failed ActRaiser comparison used mismatched instrumentation
flags, so its dispatch artifacts and timings were discarded. Mario Paint's
intentional watchdog stop is not included in the successful-game diagnostic
gate. No Mario Paint frame-performance or audio-fidelity result exists yet.

## Next work

1. Implement a continuation-safe game-frame/NMI adapter and scanout using the
   existing public runner contracts. Keep title-specific adapter decisions
   in the new game project, not the shared recompiler.
2. Add its SDL3 relative mouse adapter and deterministic tick-stamped mouse
   recording; validate actual game state/pixels with movement and clicks.
3. Independently continue the planned static return-stack analysis extension:
   memory INC/DEC and other read-modify-write footprints, without losing flag
   or return-token semantics. Mario Paint also contains stack-relative
   return-address manipulation around `$01:E393`; inspect it as evidence,
   not as permission to infer roots or rewrite live behavior.

Nothing in this milestone uses a production 65816 interpreter fallback.
