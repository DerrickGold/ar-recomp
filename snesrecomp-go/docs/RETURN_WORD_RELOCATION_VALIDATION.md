# Native return-word relocation validation

2026-09-10. Control: `e9f03eaf`. This behavior-affecting follow-up to the
[conditional frame-lifetime audit](FRAME_LIFETIME_VALIDATION.md) fixes a
return-ownership failure without new roots, authored overrides, or an
interpreter fallback. The conditional audit remains report-only; its findings
do not authorize generated code changes.

## Reproduction and cause

The supplied Mario Paint `crash.csv` reproduces the reported
`$00:A096 -> $00:F005` diagnostic at frame 822 in both the deployed binary and
an independently rebuilt control. The ROM SHA-256 is
`e842cac1a4301be196f1e137fbd1a16866d5c913f24dbca313f4dd8bd7472f45`.
The recording contains 822 input frames; extending it retains released mouse
buttons, not newly explored interactions.
Its SHA-256 is
`7ff922ace07cea0891db7de504934ce31bf2f9d28f0190d492fbd09ad2bc49d6`.

One path through the caller consumes its own incoming frame with PLA, saves P,
pushes arguments, and calls `$00:9FC4` at `$00:F002`. The callee moves its own
return word while removing arguments. Its RTS really does target `$00:F005`,
which is already an internal generated continuation. The existing PC-only
callee-clean guard rejects the actual final stack position because it crosses
the enclosing caller's stale entry boundary. Registering `$00:F005` as a new
normal function would not repair the ownership error.

The rebuilt control stops with WRAM `B57F4D56`, SRAM `011FFCA6`, pixels
`6B1EE3D9`, and semantic edges `249728:8A43D81E234B5725`; CPU state includes
`A=1FFC Y=F004 S=1FF8 PB=00 M0X0`, matching the failed return.

## Compiler/runtime contract

The emitter recognizes a deliberately narrow local def-use proof in a single
basic block: `PLY; TCS; PHY; RTS`, `PLX; TCS; PHX; RTS`, or
`PLA; TXS; PHA; RTS`. Up to four direct/absolute STZ instructions may appear
between the matching push and RTS. Register widths must decode as words.
No branch, call, replacement HLE, unmodeled store, or register clobber may
bypass the proof. Conditional HLE hooks remain intact, including their ROM
fallback. Long return/bank shuttles are not covered.

Runtime checks witness that the pull read this immediate call's physical
incoming frame, with the exact expected continuation and live bank/width.
Every allowed store must access ordinary WRAM disjoint from the relocated
word. MMIO, unknown mappings, stack wrapping, emulation mode, different owner
scopes, and equal-valued ancestor words cannot establish this witness.

Only this stronger evidence permits returning directly to the captured host
continuation while retaining the actual native S. Existing equal-stack and
bounded callee-clean fast paths, ancestor-return handling, and exit-M/X
diagnostics remain. No caller limits are rebased. No guest instruction is
skipped, and no extra guest memory read, frame push, or stack adjustment is
introduced. This is not a whole-function cleanup-size or exit-M/X summary.

The contract is game-agnostic and implemented in the emitter and portable C
runtime. Commercial ROM bytes are absent from its reusable synthetic tests.
Adoption requires regenerating C and rebuilding with the matching runtime.

## Generation isolation

All control/candidate inputs and outputs were frozen under the ignored local
directory `build/frame-retirement.fhq6aV`. Before final Mario Paint deployment,
no generation targeted an authored game's output directory. Configurations,
HLE declarations, and analysis database inputs were identical within each pair.

| Game | Variants, both | C units, both | Unresolved, both | Generated source comparison |
| --- | ---: | ---: | ---: | --- |
| Mario Paint | 1,803 | 64 | 37 | One C unit changes; two return-width variants gain witnesses |
| ActRaiser | 4,647 | 83 | 68 | Byte-identical |
| Battletoads | 913 | 48 | 17 | Byte-identical |
| Mighty Morphin Power Rangers | 8,248 | 138 | 47 | Byte-identical |

Counts are generation diagnostics, not claims of executed unresolved edges or
complete gameplay coverage. Mario Paint gains no dispatch entries or variants.
Power Rangers control and candidate runtime builds both encounter the same
pre-existing frontend/SDK mismatch (removed `host/audio_trace.h` and older
determinism-digest API names). Generation is compared, but runtime equivalence
and performance are not claimed for that game.

## Synthetic validation

- Full uncached `go test -p 1 -count=1 ./...` passes.
- Strict-warning Release runtime build and all 35 C/C++ CTest targets pass.
- Emitted-source tests cover three register shuttles, live guarded stores,
  width mismatches, clobbers, branches, calls, unsupported stores/RTL,
  HLE/exclusion barriers, conditional HLE preservation, and exit-M/X checks.
- Generated-C execution tests cover a caller with a consumed incoming frame,
  argument cleanup, and recursive same-PC calls, at all four initial M/X
  states. They check exactly-once continuations, native S and CPU state, and
  complete owner-chain unwinding alongside the existing callee-clean cases.
- Runtime tests reject wrong physical origins, same-valued ancestor words,
  wrong owner/CPU/bank/width/target, stale watchdog-invalidated witnesses,
  emulation/wrap, aliasing writes, MMIO, and mapper-dependent memory.

## Replay validation method

Control and candidate binaries use semantic-dispatch tracing. Mario Paint
comparisons include full WRAM/SRAM/framebuffer dumps and complete reported
CPU/state/semantic-edge fingerprints. Battletoads compares the complete
headless report on idle and stress input. ActRaiser uses `snesbuild
replay-bench` with WRAM, SRAM, CPU-state and dispatch artifacts on all five
representative workloads. Performance runs are serialized, warmed up, and
paired; state comparisons must pass before timing differences are meaningful.

One exploratory candidate run tripped the five-second watchdog during an SPC
upload at frame 362 while several builds were active, before the reported
return. Its log is retained as `mario-crash1200.log`. Subsequent standalone
900- and 1,200-frame runs passed. This event is not silently discarded or
treated as proof of a host-load cause; clean repeated runs are required.

### Matched unaffected workloads

One warmup pair and three alternating-order measured pairs per workload;
full outputs match in every run. Mario Paint's warmup dumps additionally
match byte-for-byte. Times are median wall seconds, including process setup.

| Workload | Frames | Control | Candidate | Change |
| --- | ---: | ---: | ---: | ---: |
| Mario Paint supplied recording, before failure | 821 | 15.512 | 15.511 | -0.00% |
| Mario Paint title click | 1,200 | 9.572 | 9.568 | -0.04% |
| Mario Paint title click and draw | 1,500 | 9.730 | 9.729 | -0.01% |
| Battletoads idle | 1,800 | 1.056 | 1.062 | +0.57% |
| Battletoads stress input | 3,600 | 2.072 | 2.084 | +0.57% |

The last healthy recorded frame (821) has WRAM `E907AA9A`, SRAM `011FFCA6`,
pixels `58712332`, and semantic edges `249694:0823592E0B6C1DFC` on both sides.
Beyond the failure, equivalence to a crashing control is not asserted: the
candidate must complete the intended frame count and repeat deterministically.

### Supplied crash recording

The final candidate completes three runs each to 1,200 and 6,000 frames with
zero failures, runtime diagnostics, or watchdog trips. Full WRAM/SRAM/pixel
dumps and CPU/semantic-edge reports match across repeats at each endpoint.
The frame-1,200 capture shows the paint editor. These tests cover the recorded
interaction and its idle aftermath, not all tools or future mouse sequences.
The earlier upload watchdog did not recur in any serialized validation run;
host contention remains a plausible explanation, not a demonstrated cause.

| Endpoint | WRAM CRC32 | SRAM CRC32 | Pixel CRC32 | Semantic edges: count / hash |
| --- | --- | --- | --- | --- |
| 1,200 | `52BE6600` | `011FFCA6` | `FE3BF072` | `264017 / 8BF9611F976F557E` |
| 6,000 | `D0375C0C` | `011FFCA6` | `5E34F806` | `460134 / AE31003E9405680C` |

At 6,000 frames, all runs report
`A=5501 X=0020 Y=0006 S=1FFB D=0000 DB=00 PB=01 P=30 M1X1 E0`,
`nmi=6000 irq=0 context_checks=6000`, and nonzero PCM count `5636144`
with peak `28672`. Total wall time is 18.43–18.45 seconds. No comparison to
control performance is meaningful beyond its frame-822 failure.

Machine-readable local evidence is in `replay-all.jsonl`, with per-run logs,
binary dumps and captures alongside it in the isolated validation directory.

### ActRaiser regression gate

All five representative replays pass one warmup and three adjacent measured
A/B pairs. WRAM, SRAM, CPU state and semantic-dispatch artifacts match exactly;
expected frame counts and workload markers are present, with no hard runtime
diagnostics. The suite performance gate passes at -0.20% regression.

| Workload | Frames | Candidate median seconds | Paired median change |
| --- | ---: | ---: | ---: |
| Mode 7 / world map | 6,000 | 4.4012 | +0.57% |
| Wide Sky Palace | 1,200 | 0.7495 | -0.66% |
| Simulation actions | 6,000 | 3.6665 | -0.20% |
| Wide Aitos | 4,000 | 3.6011 | +0.10% |
| Wide Death Heim | 4,000 | 2.9081 | -0.80% |

The report is `actraiser-ab.json`, with its exact suite, ROM, input and binary
identities. This validation ran on macOS arm64; the implementation does not
introduce architecture-specific code, but these runs are not cross-platform
execution evidence.

## Local adoption

After those gates passed, Mario Paint's actual `gen/` was regenerated: 64
files, one changed, semantic source SHA-256
`abe6b7309d186f6afb7d56154d7b814c01ef4a5d0afff5cc4a82bc6780305cc7`.
The original generated files remain in the isolated snapshot. Both
`build/MarioPaint` and `build/MarioPaintHeadless` were rebuilt, and the
project's mouse-script CTest passes. Its `recomp/` remains byte-identical to
the pre-change snapshot: no functions, dispatches, width overrides, or HLEs
were added or removed. Other games' working directories were not modified.

The deployed headless executable completed a further 6,000-frame run of the
original recording. Its full report and WRAM/SRAM/pixel dumps match the
isolated candidate exactly (`mario-deployed-crash6000.*`). Interactive SDL
launch itself was not exercised in this validation.

From the Mario Paint project directory:

```sh
./build/MarioPaint mp.sfc
./build/MarioPaintHeadless mp.sfc --frames 6000 --input crash.csv
```

The shared tree retains the preceding report-only audit changes alongside
this fix. All tracked and new source files pass whitespace checks. No commit
was made as part of this validation.
