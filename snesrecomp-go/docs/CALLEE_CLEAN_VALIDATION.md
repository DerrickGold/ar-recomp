# Native callee-clean return ownership — 2026-09-09

## Result

Mario Paint's recorded title click reaches the editor, and a short mouse-drag
replay draws visible red marks. The compiler/runtime repair is generic; the
project still has zero authored functions, dispatches, width overrides or
HLEs. Its adapter now uses cooperative polling and explicitly brackets reset
as an entry without an incoming hardware return frame. ROM, `recomp/` and
the analysis database are unchanged. Generated C and both project binaries
have been updated; the deployed headless replay matches the isolated result.

This is bounded progress, not full-game correctness, cycle-exact scheduling,
audio fidelity, or complete static reachability. No production 65816
interpreter, host-written game flag, continuation `func`, or game-specific
stack offset is used.

## Defect and execution contract

The [polling-only candidate](POLL_WAIT_VALIDATION.md) reaches completed frame
266. A native routine moves its two-byte return word while removing six
argument bytes and eight local bytes, then executes RTS. The resulting
`$01:912F -> $01:8FB5` transfer belongs to the suspended caller. Treating it
as a new registry entry loses that continuation; merely accepting the return
would also be insufficient because the caller used to restore its pre-call
S unconditionally, undoing the native cleanup.

Known-source direct JSR/JSL envelopes now register a stack-local
`CpuReturnScope` after pushing their real hardware frame. Normal equal-stack
returns retain their existing fast path. A moved native RTS/RTL may return
normally to the immediate owner only when all of these agree:

- CPU identity, original callee-entry S, and two-/three-byte frame kind;
- actual popped continuation PC and bank;
- positive, nonwrapping stack adjustment within bank-zero WRAM;
- final S does not consume the suspended caller's own return frame.

The accepted return marks its owner, so the caller keeps the real final S.
Ownership is removed before non-local return propagation as well as normal
completion. It is not a search through ancestors for a convenient matching
PC; recursion with identical return PCs must respect its immediate caller's
boundary. Direct calls/gotos remain compiled C paths; no heap allocation,
target lookup on an ordinary return, or fixed-capacity ownership array is
added. Metadata is execution-thread host state, not serialized CPU state.

The next reached cleanup at frame 752 (`$00:A096 -> $00:85F9`) exposed reset's
different entry semantics: it establishes its native stack with TCS and has
no hardware caller. `cpu_reset_scope_begin` explicitly marks that root and
its same-activation tail driver. Its permitted WRAM boundary is not inherited
by a nested interrupt activation. This removes the stale initial S boundary
without identifying reset by a title-specific address or symbol. See the
integration example in [API_REFERENCE.md](../runtime/docs/API_REFERENCE.md).

Ordinary HLE returns retain their existing stack-neutral contract. Unknown,
unpaired, ancestor, emulation-mode, wrapping and non-WRAM returns retain their
existing paths. This first implementation does not register every authored
computed-call envelope or replace all legacy equal-stack rewritten-return
rules. The earlier complete native return-table helper recognizer still
handles that separately proven shape. The new ownership check is a runtime
control-flow contract, not a newly proven static callee summary.

## Redistributable contracts and test results

`internal/emitter/callee_clean_native_test.go` generates synthetic ROM bytes,
emits C, links the real runner, and executes all four entry M/X combinations:
short RTS cleanup, long RTL cleanup including the bank byte, recursive
same-site calls with a separately generated cleanup tail, a real compiled
DEC/RTI interrupt during allocated locals, legacy HLE return, and reset stack
initialization. Tests check native S/CPU/status, exactly-once continuations,
balanced activation depth, and an empty ownership chain after completion.

Runtime tests reject wrong CPU/PC/bank/frame kind, ancestor-frame consumption,
wrapping or non-WRAM stacks, emulation mode, missing owners and reset-boundary
leakage into interrupts. Watchdog invalidation cannot resurrect old scopes.
Emitter tests also cover live PB for JSR, PC wrap, JSL bank preservation,
unknown source PCs, and scope teardown before every return-status path.

```sh
go test -p 1 -count=1 ./...
go test ./internal/emitter -run TestCalleeCleanNativeExecution -count=1 -v
cmake -S runtime -B /path/to/isolated-runtime-build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-Werror -DCMAKE_CXX_FLAGS=-Werror
cmake --build /path/to/isolated-runtime-build -j 8
ctest --test-dir /path/to/isolated-runtime-build --output-on-failure
```

Full uncached Go suite: pass. Native cleanup cases: 6/6, each with four M/X
states. Strict-warning runtime: 35/35 C/C++ tests pass. Mario Paint's deployed
mouse-script parser test passes. One earlier concurrent Go run hit an
existing browser UI test's 15-second timeout while large builds competed;
the isolated full suite passed. Logs retain both runs.

## Mario Paint replay evidence

ROM SHA-256:
`e842cac1a4301be196f1e137fbd1a16866d5c913f24dbca313f4dd8bd7472f45`.
From its project directory, with your own verified dump:

```sh
./build/MarioPaintHeadless mp.sfc --frames 1200 --input tests/title-click.csv
./build/MarioPaintHeadless mp.sfc --frames 1500 --input tests/title-click-and-draw.csv
./build/MarioPaint mp.sfc
```

| Replay | WRAM CRC32 | SRAM CRC32 | Pixel CRC32 | Semantic edges: count/hash |
| --- | --- | --- | --- | --- |
| Recorded title click, 1,200 frames | `6E5BBB02` | `011FFCA6` | `64C5FA2E` | `204469:B22A64A40DEFA762` |
| Click and draw, 1,500 frames | `6786EBA1` | `011FFCA6` | `9D9F0CD3` | `216651:066AB56AF19225A9` |

Each replay repeats byte-identically in its full printed CPU/state/pixel/
audio-count/semantic-edge report, with failures/watchdog zero and exactly
1,200/1,500 balanced NMI context checks. PCM counts/peaks are respectively
`845498/28672` and `1152824/28672`; these are not audio fidelity measurements.
Captures confirm the editor, palette, toolbar, pencil cursor and drawn marks.
Other tools, save persistence and extended play remain untested.

## Cross-game equivalence, timing and size

Both A/B candidates use identical ROM/config/database/frontend/build settings;
the before emitter omits this turn's ownership envelopes, retaining the
earlier polling and return-table fixes. Controls were rebuilt from frozen
copies after external cleanup removed the old temporary directories. This
isolates the ownership generation change; prior polling results are recorded
separately. ActRaiser and Battletoads production sources/configs/generated
directories were not replaced.

All five ActRaiser representative replay workloads match WRAM/SRAM/CPU and
semantic-dispatch artifacts. Battletoads 1,800-frame idle and 3,600-frame
stress runs have byte-identical full reports, including semantic edges.
Mario Paint's common 265-frame prefix matches. All complete without hard
diagnostics. Comparisons use semantic-dispatch instrumentation, not just
registry-edge counts. One warmup and three alternating adjacent pairs, with
no simultaneous builds, give:

| Workload | Median wall-time change |
| --- | ---: |
| ActRaiser five-workload suite | -0.38% |
| Battletoads idle | -0.10% |
| Battletoads stress | +0.00% |
| Mario Paint common 265-frame prefix | +0.13% |

No material runtime regression is evident on this macOS ARM64 host. This
does not establish timings on other architectures or post-click equivalence
against the broken control.

A final rebuild/recheck using the same current Mario adapter on both sides
again matches every report. Three fresh warmed pairs give Battletoads idle
-5.04%, stress -0.10%, and Mario's common prefix +0.96%. The idle difference
did not appear in the earlier batch and is not claimed as a speedup. These
additional measurements are retained in `replay-bench-final.jsonl`.

The inline ownership bookkeeping has a measurable linked-file size cost in
these matched builds; this is **not** a size-neutral change:

| Binary | Before bytes | After bytes | Change |
| --- | ---: | ---: | ---: |
| Mario Paint headless | 7,609,856 | 8,303,520 | +9.12% |
| ActRaiser | 23,490,256 | 27,090,016 | +15.32% |
| Battletoads headless | 3,815,024 | 4,062,832 | +6.50% |

Outlining or statically limiting ownership metadata is a future optimization;
it must retain these contracts and re-run replay/size/performance gates.

Generated semantic source identities (not ROM hashes):

| Game | Variants / C files | SHA-256 |
| --- | --- | --- |
| Mario Paint | 1,803 / 64 | `0e287f202c6bdf06ad061780ac0ddd0e16b093112c96b299088cad046aedbd09` |
| ActRaiser | 4,647 / 83 | `36ff5fddc8db300af9de90b536f84cb1b89b26eddc8342718fe11cb48ec10ab6` |
| Battletoads | 913 / 48 | `5e76a195f6f7c42af8166a4813f7ae8dd9a65f034eedc84d51324f304354f04b` |

Raw unresolved emission counts remain 37 / 68 / 17 respectively. None of
those counts proves the edge executes; no hard diagnostic executes in the
passing workloads above. Authored HLEs/configuration remain intact.

Local, ignored artifacts are in the enclosing ActRaiser checkout at
`build/callee-clean.NCVuAh/`: frozen before/after generation and binaries,
`actraiser-ab.{log,json}`, `replay-bench.{js,jsonl}`, `native-clean-tests.log`,
`go-isolated.log`, `runtime-latest-tests.log`, Mario replay logs and
`mario-click1200.png` / `mario-draw1500.png`. The original deployed Mario
generated control is retained under `mario/gen/` there. No ROM or derived
generated game code is added to the shared compiler's tracked files.
