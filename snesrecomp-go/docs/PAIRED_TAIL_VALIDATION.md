# Paired split-tail return context — 2026-09-09

## Contract and implementation

A same-bank branch into a separately generated body is a tail transfer of
the **same routine activation**, not a new callable routine. Preserve its
original hardware entry stack and paired-host-return flag across registry
lookup; the target must select its variant using the live M/X flags.

`internal/emitter/function.go:tailCallStatement` previously preserved this
context only for unpaired trampoline entry. Paired entry dropped it before
calling `cpu_dispatch_pc_from`, whose ordinary contract correctly clears the
host-return flag. The split body then dispatched its caller's continuation
as a new function. Mario Paint exposed this at `$01:E746 -> $00:80FF` inside
the seventh NMI.

The paired path now calls `cpu_dispatch_paired_tail_from` after popping the
current generated activation. This shared helper:

- supplies the original entry S/host-return context to the next prologue;
- owns one driver for that CPU, original entry S and generated stack depth;
- yields subsequent tails at that same boundary to its existing loop;
- gives actual nested JSR/JSL calls their own return boundary;
- clears unused one-shot context when a miss or an HLE returns without a
  generated prologue, preventing leakage into later unrelated calls.

It allocates no heap memory and adds no interpreter or game-specific policy.
Local gotos, direct C calls and the existing unpaired trampoline fast path
are unchanged. `dispatchReturnTransfer` now shares this tail emitter instead
of duplicating its own paired/unpaired logic. Newly generated output needs
the matching updated runner archive/header that exports the helper.

Frame watchdog resets also preserve generated-stack/return tracking when
called inside a synchronous execution checkpoint. The host activation is
still live there. Ordinary non-checkpoint frame starts retain the existing
tracking-reset behavior, including resetting the paired-driver state.

## Redistributable regression coverage

`internal/emitter/tail_context_test.go` builds original synthetic ROM bytes
for a conditionally split routine and checks every M/X variant, the shared
dispatch-return path and the unchanged local-goto path.

`internal/emitter/tail_native_test.go` emits another original fixture,
compiles it against the actual runner with CMake, and executes all M/X
states, taken/untaken branches, paired/unpaired entry, 10,000-iteration tail
chains and real nested JSR calls. A paired C caller's continuation must
never enter the registry; an unpaired entry's hardware continuation must
execute exactly once. Register widths, final X, hardware S and generated
activation depth are checked. This native tier requires CMake and a C/C++
toolchain and is skipped with `go test -short` or without CMake; pure-Go
emission tests remain available independently.

The runtime infrastructure suite separately tests 10,001 split transfers,
live width changes, inheritance when current S differs from entry S, a
genuine nested child, bounded driver depth, cleanup after a missing target,
and watchdog frame starts during an active execution checkpoint.

## Mario Paint: original blocker fixed, another compiler gap exposed

No authored functions, dispatches, width overrides or HLEs were added. The
candidate remains 1,411 variants / 64 C files / 38 raw unresolved indirect
emissions / 75 markers. Generated semantic SHA-256:

`63647f3d7873ab5443e19bec2f5b91c27d1efb83b74fb05aaae1df94153d16f9`

The original six-frame prefix is identical to its control. A 23-frame idle
recording and replay both exit 0 with:

```text
frames=23 nmi=23 irq=0 context_checks=23 failures=0 watchdog=0
WRAM=39571DD0 SRAM=011FFCA6 pixels=E5BDB074
semantic edges=46376:CF1DD326AB20A6E3 PCM nonzero=0 peak=0
A=0001 X=00FF Y=0016 S=1FF1 D=0000 DB=7F PB=01 P=30 M=1 X=1 E=0
```

The native capture at this point shows the Mario Paint title screen and hand
cursor. Interactive drawing and audible output are not validated.

The longer 300-frame acceptance run still fails after those 23 frames. This
is **not** a playable or clean one-shot runtime result:

- `$01:9730` JSLs a return-address-based inline-table dispatcher at `$01:E393`.
- That helper saves status/X/Y/D, sets D from the hardware stack pointer,
  loads an inline table entry through the stacked return PC, overwrites the
  stacked return address, decrements it for RTL, and restores the registers.
- Generated RTL at `$01:E3AB` treats unchanged stack depth alone as proof of
  an ordinary host return. It ignores that the return address was changed.
- The compiled caller consequently resumes at `$01:9734`, which is table
  data, not its real continuation. Garbage decoding eventually emits an
  unresolved-indirect diagnostic at `$01:973C` with target `$48:0857`.

Do not register that garbage target or the table as code. The next generic
work is stack-return-address ownership and inline-table dispatcher recovery,
including real handler discovery and correct unwinding of the replaced
caller continuation. Avoid a game-specific HLE or hand-authored function
list as a substitute. This patch does not attempt that separate change.

Local validation artifacts (no ROM redistribution):
`/private/tmp/snesrecomp-paired-tail.Lzx8P2`.

The helper is a separate runtime archive member (`src/core/paired_tail.c`),
so device-only SDK clients do not acquire a generated dispatch-table link
dependency. The source manifest includes it for packaged archives. Runtime
unit tests and the installed-consumer test cover this layout.

## Validation

- `go test -count=1 ./...`: pass, including the native emitted-C fixture.
- Strict standalone runner C/C++ tests: 35/35 pass, including SDK consumers.
- ActRaiser: regenerated 4,647 variants (same counts), semantic source hash
  `69248358d47bd4ceaf0db8483fb5735596762f8801c99655254ec390c21a252d`.
  All five representative replays match WRAM/SRAM/CPU/semantic dispatch,
  with no hard runtime diagnostic. Three adjacent A/B pairs after warmup:
  suite time +0.61%, individual medians −0.12% to +1.18%.
- Battletoads: regenerated 913 variants (same counts), semantic source hash
  `18d1cfc1ced39390f49a7a7a370e5eec19a444b29fe98ca6f33bbd73a46192c1`.
  Existing 1,800-frame idle and 3,600-frame stress workloads match their
  control output byte-for-byte, including CPU, WRAM, framebuffer and audio
  observations. Three warmed adjacent pairs: −0.27% idle / −0.40% stress,
  no material regression; see `battletoads-ab-final.log`.
- Generated behavior changed intentionally at paired split-tail sites; this
  is not a report-only change. No authored config or HLE was removed/added.
