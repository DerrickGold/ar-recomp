# DKC3 compiler/runner handover

Snapshot: 2026-09-14. Continue in the existing ActRaiser workspace; do not start a fresh bring-up or replace the compiler's dirty files.

## 1. Outcome and immediate objective

The compiler/runner checkpoint is committed as **dd7a6f1c5ca0c9095e9ac44b803f3232c4a3daf8**:
“feat(recomp): close rooted static discovery and native continuation gaps”.
It includes 144 files, including all previously untracked compiler implementation/tests and the runner OAM correction.

DKC3 boots, reaches the title, and progresses through attract scenes into a cabin. Full attract/gameplay acceptance is **not complete**: the 6000-frame idle request stops at frame 4908 on **$34:8E0D -> $B4:8F58**, a missing compiled callback.

The apparent corrupted sprites were a separate, now-fixed runner OAM-address reload defect. Do not reopen that diagnosis based on old screenshots.

After the checkpoint, we added tested finite arithmetic/local indirect-read analysis and a cyclic-worklist fix. This follow-on work is **uncommitted** and does not yet recover the DKC3 callback. Candidate23b regenerates exactly the same C as candidate22.

Next objective: establish the callback-table initializer's bank/input and scalar-slot lifetime relationships, then reuse the new local indirect-read query to recover handlers automatically. Do not insert the observed missing target as a root or config workaround.

## 2. Scope and non-negotiables

- Keep shared compiler/runtime improvements game-agnostic, portable, deterministic, AOT, and fast on direct compiled edges.
- No production interpreter fallback; no importing reference-generated code or hand-authored reference configuration.
- No renderer-backend abstraction or widescreen implementation in this work.
- Preserve authored HLE, width/control-flow overrides, data ownership, and unrelated user changes.
- Work in isolated build directories. Do not regenerate adjacent projects' src/gen or edit their authored config.
- Use runtime observations to diagnose missing static relationships, not to seed additional roots. The current DKC3 experiment retains exactly 19 previously admitted ROM-hashed observed roots; it is **not a static-only one-shot recompile**.
- Cold inventory is conditional, not a closed target-set, native M/X/DB proof, or reachability claim. Retain actual native reads, live-state dispatch, stack/continuation behavior, and hard misses.
- The worklist already feeds newly discovered entries through all strategies. Do not solve this by increasing outer iterations, scanning every decodable ROM word, or assuming DB survives an unknown helper.
- No background jobs or running regeneration sessions need resuming at handover.

## 3. Workspace and artifact roots

All paths below are on the current machine. Build artifacts are ignored/local: they do **not** travel with a Git checkout. Preserve/copy the needed artifact trees explicitly if moving hosts. ROMs are not redistributable.

| Name | Absolute path | Purpose |
| --- | --- | --- |
| ROOT | /Users/derrick/Documents/Programming/ActRaiserRecomp | Actual Git repository |
| C | /Users/derrick/Documents/Programming/ActRaiserRecomp/snesrecomp-go | Compiler and shared runner; not a separate submodule |
| W | /Users/derrick/Documents/Programming/ActRaiserRecomp/build/dkc3-hirom.V6Z4S4 | Original isolated DKC3 bring-up, roots, runtime tests |
| R | /Users/derrick/Documents/Programming/ActRaiserRecomp/build/dkc3-hirom.V6Z4S4/attract-progress.B2G2at | Active candidate project, generated C, headless build, candidates01–23 |
| B | /Users/derrick/Documents/Programming/ActRaiserRecomp/build/dkc3-hirom.V6Z4S4/boot-completion.kGi3Uy | Earlier boot baseline and ten retained observed-root inputs |
| O | /Users/derrick/Documents/Programming/ActRaiserRecomp/build/dkc3-oracle.kAV0s8 | Reference checkout/build, OAM evidence, latest cross-game validation, value-query probe |
| ROM | /Users/derrick/Documents/Programming/DonkeyKongCountry3Recomp/dkc3.sfc | User's ROM, read-only |

Read these first:

1. [R/STATUS.md](/Users/derrick/Documents/Programming/ActRaiserRecomp/build/dkc3-hirom.V6Z4S4/attract-progress.B2G2at/STATUS.md): chronological candidate01–23 record. Read the top and candidate23 first; older “next” statements are historical.
2. [O/OAM-FINDING.md](/Users/derrick/Documents/Programming/ActRaiserRecomp/build/dkc3-oracle.kAV0s8/OAM-FINDING.md): confirmed hardware defect, corrected capture, regression evidence.
3. [C/docs/EXPERIMENTAL_STORED_TARGETS.md](/Users/derrick/Documents/Programming/ActRaiserRecomp/snesrecomp-go/docs/EXPERIMENTAL_STORED_TARGETS.md): implemented contracts/conditions, including uncommitted arithmetic and local indirect reads.
4. [O/ORACLE.md](/Users/derrick/Documents/Programming/ActRaiserRecomp/build/dkc3-oracle.kAV0s8/ORACLE.md): external reference setup and limitations. Its final “sprite cause still unproven” paragraph predates and is superseded by OAM-FINDING.md.

### ROM identity

- 4,194,304 bytes, FastHiROM.
- CRC32: 448EEC19.
- SHA-256: 2277a2d8dddb01fe5cb0ae9a0fa225d42b3a11adccaeafa18e3c339b3794a32b.
- Existing isolated authored config: W/project/recomp/bank00.cfg contains only “bank = 00” and “auto_vectors”.

### Frozen/current executables

Use **R/DKC3-oam-before-indirect-read** as the next A/B reference. Do not overwrite it.

At handover it is byte-identical to R/build/DKC3Headless:

```text
e4eba5dd806b1556f58209e0f1913d7cfd4ec362de0c623686121d0709fff172
```

R/build/libdkc3_generated.a SHA-256:

```text
94c718c2fda06d8778703316759ef363406556c906073b92ba7f136e368d770b
```

R/CMakeLists.txt points at C/runtime and R/gen. R/src/main.c is the current isolated headless integration; R/recomp/funcs.h is its generated function header. Trace, watchdog, and semantic-dispatch instrumentation are enabled; IPO is disabled. This is not a trace-free release performance baseline.

O/v2regen is the checkpoint tool; O/v2regen-next includes candidate23b production analysis changes. Build a fresh tool from the working tree for further work. There is no R/tool executable.

## 4. Exact working-tree state and code map

Before this handover document was added, git status contained exactly:

```text
 M snesrecomp-go/docs/EXPERIMENTAL_STORED_TARGETS.md
 M snesrecomp-go/internal/tooling/value_provenance.go
?? snesrecomp-go/internal/regen/indirect_values_test.go
?? snesrecomp-go/internal/tooling/value_indirect.go
?? snesrecomp-go/internal/tooling/value_indirect_test.go
```

Do not discard these untracked files. This handover is an additional documentation-only change.

### Follow-on implementation

- internal/tooling/value_provenance.go: shared query engine; new indirect target category, arithmetic/indirect-read integration, sticky cardinality overflow, and prevention of mask self-seeding through indirect reads.
- internal/tooling/value_indirect.go: finite word ADC/carry, same-slot selector correlation, local scratch-pointer reaching definitions, mapper-aware indirect ROM reads, JMP(abs) consumers.
- internal/tooling/value_indirect_test.go: nested records/state tables; carry; pointer reuse; aliases; HLE/data; unknown bank/D/decimal; growing/unseeded cycles; HiROM low-address targets and bank-crossing rejection.
- internal/regen/indirect_values_test.go: one regeneration discovers handlers and their direct callee without observations; default/opt-in/HLE cases; jobs1/4 determinism and authored-config immutability.

Important semantics:

- Unknown carry stops arithmetic. Known decimal mode rejects it; unknown decimal mode remains an explicit cold binary-arithmetic condition.
- Known nonzero D is unsupported by this local-pointer consumer. Unknown D carries an explicit zero-D condition, not a generated CPU mutation.
- Partial/indirect/indexed/RMW pointer overwrites, calls, hardware writes and unknown effects stop local pointer recovery.
- The load before storing a handler back into its scratch slot must see the earlier pointer definition, not a global union including its own result.
- On >256 word/provenance alternatives, a query stays unknown for that solve. Previously it could empty, reseed, and repeatedly exhaust the work budget.
- “Converged” means the bounded analysis settled, not that code coverage is complete.

### Next packages to inspect

- internal/tooling/value_inputs.go: invocation inputs and contextBank; currently caller contexts originate from direct calls/tails.
- internal/tooling/cold_bank_query.go: bounded bank/abstract-stack queries; cold-only saved-stack non-aliasing condition must not become a published DB fact.
- internal/tooling/banksummary.go: exact helper return/bank/MX summaries and reasons for refusal.
- internal/tooling/tableinitializer.go: register/index expression and ROM-table evidence.
- internal/tooling/commandvalues.go, commandfieldvalues.go, commandresumes.go, decodedcommands.go: already-rooted command inputs, rebased field relationships, resumable stream worklist.
- internal/regen/regen.go: discovery closure; consumes shared cold targets after cheaper direct/stored passes settle, then reruns normal discovery.
- internal/rom/mapper.go: mapper-aware ROM ownership; never use a universal “below $8000 means invalid” rule.
- internal/emitter and runtime/src/core: already-tested owned continuations/parked return behavior. No evidence currently requires another continuation fix for this miss.

## 5. Remaining failure: evidence and precise gaps

Authoritative corrected-run evidence:

- O/ours-fixed-attract.log and .err.
- O/ours-fixed-attract.ppm and .ppm.wram.
- R/idle-22-dispatch.jsonl: pre-OAM-correction diagnostic observations; same callback failure, not root input.

The 6000-frame request actually stops at4908:

```text
trapped: 348E0D -> B48F58
frames=4908 nmi=4908 irq=0 context_checks=0 failures=2 watchdog=0
wram=54FE7AC1 sram=B3FFFEF0 pixels=A79F3B12
edges=792063:06CB8B6039BB6156 pcm_nonzero=4943614 peak=16049
```

Use the trap's live DB/PB evidence, not the later restored adapter summary alone.

### Dispatcher

```text
34:8DEC  LDA $1C3B
34:8DEF  BMI $8E10
34:8DF1  ASL A
34:8DF2  CLC
34:8DF3  ADC $1C3B
34:8DF6  ADC $1C7F
34:8DF9  STA $42
34:8DFB  LDY #$0002
34:8DFE  LDA ($42),Y
34:8E00  AND #$00FF
34:8E03  STA $1C3B
34:8E06  LDA ($42)
34:8E08  STA $42
34:8E0A  PEA $8E0F
34:8E0D  JMP ($0042)
```

This is a three-byte state record: handler word plus next-state byte. At the failure, the table base in1C7F isF80A, ROM B4:F81C holds8F58, and the native target isB4:8F58. These observed values diagnose the chain; they are not newly authorized roots.

### Initializer and reused RAM slot

The relevant initializer is34:AD89, also internally entered at34:ADC0:

- Derive a selector from the high byte of1C5F, mask it, shift left4, addF6AA.
- Store that record address in1C61 at34:ADEE.
- Read record field+11 at34:AE24; publish the callback-table pointer in1C7F at34:AE27.
- Read field+3 at34:AE2A, mask7F, initialize state1C3B at34:AE30.
- RecordF6EA +11 = F6F5 containsF80A, matching the observed base.

**The F638 pointer table is an earlier, different lifetime of1C61.** Do not combine it indiscriminately with the F6AA strided-record selector. Global unions here create unrelated/garbage alternatives and can saturate the query.

### What still prevents recovery

1. The existing indexed JSR at34:80DC independently recoversAD89 among its targets, but these indexed edges are not yet invocation contexts in the shared value engine.
2. This is not fixed by caller contexts alone: cold bank summaries stop before both sides:
   - query34:80DC stops at34:80B4's JSL BB:8567;
   - queries34:ADEE/AE27 stop at34:ADC0's JSL BC:F888.
3. Both are call_bank_contract_unresolved. This is **not evidence that the helpers change DB**; their contracts have not been established.
4. BB:8567 is a JMP thunk toBB:8CF6. BC:F888 calls80:807E, B8:806F, B8:805A, and80:8081.
5. Even after bank resolution, the reused slot needs producer/lifetime separation. Do not manufacture a canonical bank, treat all writes as the same record family, or merely raise query budgets.

Suggested next independently testable milestone: a synthetic ROM with an indexed initializer call, helper bank/stack contracts, a reused scalar slot, and nested state-table publication; prove the useful input relationship without closing an uncertain edge. Reuse the candidate23 local indirect consumer and existing discovery closure.

### Read-only isolated query probe

O/value_probe_test.go runs via O/value-overlay.json, without adding game-specific source to the compiler. It re-decodes existing bank34 generated entries and prints node values/bank blockers. O/value-probe.log contains the latest output.

It is a diagnostic subset, **not** the full generation pipeline or reachability/equivalence evidence. It imports no missing target as a root. Do not promote its broad or garbage alternatives.

## 6. Artifact map and validation already completed

### Candidate23 analysis-only follow-on

Under R:

- regen-23.log: first attempt; work budget exhausted, deliberately stopped before emission.
- regen-23b.log: completed corrected run.
- go-value-indirect-final.log: final full Go suite passes.
- runtime-value-indirect.log: all38 runtime C/C++ CTests pass.
- test-indirect-regen.log and test-value-saturation.log: intermediate focused tests; final Go log is authoritative.
- gen/: candidate23b output, byte-identical to candidate22.
- STATUS.md: detailed history, including rejected hypotheses and earlier candidates.

Candidate23b: 25282 variants,4027 internal wrappers,286 files,0 changed,111 decode-budget stubs; regeneration2m40.482s. Final shared pass41777 nodes /57646 evaluations, converged,7 indexed /55 saved-pointer /0 new indirect targets.

Generated semantic source SHA-256:

```text
3a97cdcd11aa330b783356b9e019a69cabe4dcdd70d9da15e6f53b845d6f7e12
```

ActRaiser: O/ar-gen-value-indirect is byte-identical to O/ar-gen; log O/ar-regen-value-indirect.log. 5636 variants, semantic SHA-256:

```text
7627d13e22cf03b49567339493ffc41027b9981ccc7ea0403cd09d366691ba52
```

There was no changed runtime/generated game behavior in candidate23b, so no new replay/performance improvement is claimed. Other games were not rerun for that follow-on phase.

### Committed OAM correction and cross-game acceptance

The existing ppu_handleVblank address-reload handler had no caller. The committed fix invokes it once at the shared beam's visible-to-VBlank edge, respecting forced blank; repeated positioning within VBlank does not restart a transfer. No per-game workaround.

At the image after4825 executed frames, all544 captured OAM bytes now match the actual $2104 DMA write stream. Before the fix they all differed because the cursor drifted. WRAM may change after DMA, so compare the bus stream, not a later sprite buffer.

Under O:

- OAM-FINDING.md: full evidence, tests, interpretation.
- ours-fixed-4825.png: corrected intact character; ours-fixed-4825.* includes image, memory, bus and scanout-state captures.
- ours-4825.* and ours-bus-4825.*: old fragmented-image/bus controls.
- capture_ours.c, DKC3Capture, DKC3Capture-fixed: read-only ABI capture wrapper and old/fixed executables.
- ours-final.edges: corrected full782597-event semantic sequence, byte-identical to R/idle-prefix-command-values-after.edges.
- ours-final.log/.err/.ppm/.ppm.wram: ordinary corrected harness prefix.
- go-test.log, ctest-final.log: full Go and38 runtime tests.
- ar-bench.json/.log: all five ActRaiser workloads, three measured adjacent pairs after warmup; deterministic artifacts match, suite delta-0.20%, each median within0.4%.
- bt-idle.log, bt-stress.log: frozen Battletoads1800-idle/3600-stress complete logs match R/bt-idle-22.log and R/bt-stress-22.log. The old cached fixture already has silent audio; no audio fidelity claim.
- mp-before.* and mp-fixed.*: Mario Paint6500-frame log/image/WRAM/SRAM equality using frozen frontend/generated objects and an isolated runtime archive replacement.
- dkc3-perf.json/.log: four measured DKC3 adjacent pairs after warmup;4825-idle +0.14%,2700-Start +0.11%, no material regression.
- libsnesrecomp_runtime-before.a, ActRaiser-before, Battletoads-before, MarioPaint-before: preserved controls for that phase.

The Mario Paint input at /Users/derrick/Documents/Programming/MarioPaint/crash.csv now ends at frame822; SHA-256 7ff922ace07cea0891db7de504934ce31bf2f9d28f0190d492fbd09ad2bc49d6. Do **not** call this the original frame5380 crash recording. MMPR was not rerun for the OAM phase.

Corrected DKC3 prefix after4825 frames:

```text
failures=0 watchdog=0 wram=5D5C4FE8 sram=B3FFFEF0 pixels=B9C96C0E
edges=782597:B3272951FC2488EC pcm_nonzero=4854989 peak=16049
```

The older image CRC wasC152F71E; that deliberate image change is already explained and committed. For future compiler-only comparisons against the corrected baseline, pixels should match too.

### External reference project

O/reference is a separate checkout of https://github.com/elliotttate/DKC3Recomp:

- root3a033f19801a2bd3abf784d4b29c4462495d19de;
- runtime3b24b9daae623cd66a14e1ecf8483f0b0d2be91b;
- UI ad2f3e293c6641c93ee69963dd669661f3e40290.

O/headless/dkc3_snesrecomp_headless is its reference executable. Build/generation logs, reference-idle*, reference-repeat*, reference-scalar*, and sampled screenshots are all under O; reproduction commands are in ORACLE.md.

Keep its interpreter confined to that executable. Top-level MIT does not cover everything: runtime PolyForm Noncommercial; disassembly reference GPL-3. No code/configuration has been copied into ours.

Three6000-frame reference runs completed with matching final dumps, **but all emitted interp_cap/unresolved-abandon at808CAD frame58, explicitly skipping side effects**. It is not an infallible hardware oracle or a run satisfying our acceptance rules. Host-frame timing also differs from our parked-wait scheduler. No runner-performance comparison against it has been established.

## 7. Reproduction commands

These commands assume Bash or Zsh. They leave adjacent game projects untouched. Create a new artifact directory for each attempt; never overwrite frozen binaries or old evidence.

### Set up and inspect

```sh
DKC3_ROOT=/Users/derrick/Documents/Programming/ActRaiserRecomp
DKC3_COMPILER="$DKC3_ROOT/snesrecomp-go"
DKC3_WORK="$DKC3_ROOT/build/dkc3-hirom.V6Z4S4"
DKC3_ATTRACT="$DKC3_WORK/attract-progress.B2G2at"
DKC3_BOOT="$DKC3_WORK/boot-completion.kGi3Uy"
DKC3_ORACLE="$DKC3_ROOT/build/dkc3-oracle.kAV0s8"
DKC3_ROM=/Users/derrick/Documents/Programming/DonkeyKongCountry3Recomp/dkc3.sfc
DKC3_RUN="$(mktemp -d "$DKC3_ATTRACT/handoff.XXXXXX")"
DKC3_TOOL="$DKC3_RUN/v2regen"

git -C "$DKC3_ROOT" status --short
git -C "$DKC3_ROOT" diff --stat
git -C "$DKC3_ROOT" diff --check
shasum -a 256 "$DKC3_ROM"

cd "$DKC3_COMPILER"
GOCACHE="$DKC3_ROOT/build/go-cache" go build -o "$DKC3_TOOL" ./cmd/v2regen
```

### Exact frozen19-root regeneration

Do not replace this list with a glob or newer dispatch-census files.

```sh
DKC3_OBS_INPUTS=(
  "$DKC3_WORK/dkc3-observed-19.json"
  "$DKC3_WORK/dkc3-observed-20.json"
  "$DKC3_WORK/dkc3-observed-21.json"
  "$DKC3_WORK/dkc3-observed-22.json"
  "$DKC3_WORK/dkc3-observed-23.json"
  "$DKC3_WORK/dkc3-observed-24.json"
  "$DKC3_WORK/dkc3-observed-25.json"
  "$DKC3_WORK/dkc3-observed-27.json"
  "$DKC3_WORK/dkc3-observed-28.json"
  "$DKC3_BOOT/observed-01.json"
  "$DKC3_BOOT/observed-02.json"
  "$DKC3_BOOT/observed-04.json"
  "$DKC3_BOOT/observed-07.json"
  "$DKC3_BOOT/observed-08.json"
  "$DKC3_BOOT/observed-09.json"
  "$DKC3_BOOT/observed-10.json"
  "$DKC3_BOOT/observed-11.json"
  "$DKC3_BOOT/observed-12.json"
  "$DKC3_BOOT/observed-13.json"
)
DKC3_OBS_ARGS=()
for DKC3_OBS_FILE in "${DKC3_OBS_INPUTS[@]}"; do
  DKC3_OBS_ARGS+=(--observed-dispatches "$DKC3_OBS_FILE")
done
shasum -a 256 "${DKC3_OBS_INPUTS[@]}" > "$DKC3_RUN/observed-inputs.sha256"

"$DKC3_TOOL" regen --rom "$DKC3_ROM" \
  --cfg-dir "$DKC3_WORK/project/recomp" \
  --out-dir "$DKC3_ATTRACT/gen" \
  --funcs-out "$DKC3_ATTRACT/recomp/funcs.h" --jobs 6 \
  --experimental-stored-targets --experimental-parked-waits \
  --experimental-internal-tails --allow-stubs \
  "${DKC3_OBS_ARGS[@]}" > "$DKC3_RUN/regen.log" 2>&1
```

Wait for exit0, not just a printed semantic hash, before compiling. On failure, inspect the log; do not build partial output. After successful regeneration:

```sh
cmake --build "$DKC3_ATTRACT/build" -j6 > "$DKC3_RUN/build.log" 2>&1
```

This updates only the isolated active candidate. Preserve R/DKC3-oam-before-indirect-read for comparison.

### Prefix equivalence and progression

The headless arguments are ROM, frame count, output PPM, input mode. Mode0=idle; mode3 pulses Start at frames2100–2101. Mode1 pulses Start from800 periodically; mode2 holds right from800.

```sh
DKC3_EDGE_LOG="$DKC3_RUN/before.edges" \
  "$DKC3_ATTRACT/DKC3-oam-before-indirect-read" \
  "$DKC3_ROM" 4825 "$DKC3_RUN/before.ppm" 0 \
  > "$DKC3_RUN/before.log" 2> "$DKC3_RUN/before.err"

DKC3_EDGE_LOG="$DKC3_RUN/after.edges" \
  "$DKC3_ATTRACT/build/DKC3Headless" \
  "$DKC3_ROM" 4825 "$DKC3_RUN/after.ppm" 0 \
  > "$DKC3_RUN/after.log" 2> "$DKC3_RUN/after.err"

cmp "$DKC3_RUN/before.edges" "$DKC3_RUN/after.edges"
cmp "$DKC3_RUN/before.ppm.wram" "$DKC3_RUN/after.ppm.wram"
cmp "$DKC3_RUN/before.ppm" "$DKC3_RUN/after.ppm"
diff -u "$DKC3_RUN/before.log" "$DKC3_RUN/after.log"
diff -u "$DKC3_RUN/before.err" "$DKC3_RUN/after.err"

"$DKC3_ATTRACT/build/DKC3Headless" \
  "$DKC3_ROM" 6000 "$DKC3_RUN/attract.ppm" 0 \
  > "$DKC3_RUN/attract.log" 2> "$DKC3_RUN/attract.err"
```

Repeat the paired prefix check for2700 frames with mode3 and distinct filenames. The current6000 attempt fails at4908; do not treat that expected reproduction as success. Run longer/input workloads only after fixing and explaining the current miss.

Read stderr, complete CPU/SRAM/WRAM/audio summaries and full edge sequences. context_checks=0 in this harness is not evidence of an interrupt-context conformance test. Zero watchdogs alone is insufficient. Do not ignore bounds warnings because a legacy summary reports zero errors.

### Go and runtime contracts

```sh
cd "$DKC3_COMPILER"
GOCACHE="$DKC3_ROOT/build/go-cache" \
ZIG_GLOBAL_CACHE_DIR="$DKC3_WORK/deferred-zig-cache" \
  go test ./... > "$DKC3_RUN/go-test.log" 2>&1

cmake --build "$DKC3_WORK/runtime-tests" -j6
ctest --test-dir "$DKC3_WORK/runtime-tests" --output-on-failure -j2 \
  > "$DKC3_RUN/ctest.log" 2>&1
```

### ActRaiser regeneration and replay regression

```sh
"$DKC3_TOOL" regen --rom "$DKC3_ROOT/ar.sfc" \
  --cfg-dir "$DKC3_ROOT/recomp" --out-dir "$DKC3_RUN/ar-gen" \
  --jobs 2 --experimental-internal-tails --allow-stubs \
  > "$DKC3_RUN/ar-regen.log" 2>&1

diff -qr "$DKC3_ORACLE/ar-gen" "$DKC3_RUN/ar-gen"
```

If behavior changes, use existing isolated ActRaiser build R/ar-build (source ROOT/build/frame-retirement.fhq6aV/actraiser-current). Inspect its CMake paths before rebuilding so you know which generated directory it consumes. Freeze its current executable before replacing it; O/ActRaiser-before predates the OAM correction and is not automatically the next desired reference.

The reusable five-workload runner is ROOT/build/frame-retirement.fhq6aV/snesbuild:

```sh
"$DKC3_ROOT/build/frame-retirement.fhq6aV/snesbuild" replay-bench \
  --root "$DKC3_ROOT" --suite tools/runner-bench.json \
  --rom ar.sfc --config config.ini \
  --binary "$DKC3_ATTRACT/ar-build/ActRaiserRecomp" \
  --reference-binary /absolute/path/to/newly-frozen-ActRaiser-baseline \
  --workload mode7_worldmap --workload sky_palace_wide \
  --workload sim_actions --workload aitos_wide --workload death_heim_wide \
  --runs 3 --warmups 1 --output "$DKC3_RUN/ar-bench.json"
```

Run performance comparisons only after builds/replays stop competing for CPU. Warm both binaries and alternate multiple adjacent pairs. A noisy host was explicitly noted by the user.

O/bench-oam.cjs is a useful previous timing harness, but it permits a pixels-only difference for the already-fixed OAM defect. **Tighten that comparison for new compiler-only checks; do not inherit the image exemption.** It writes artifacts into its working directory. Snapshot I/O is included on both sides; it is not a runner-only throughput benchmark.

### Other games

- Battletoads ROM: /Users/derrick/Documents/Programming/BattleToadsRecomp/bt.sfc.
  Cached isolated binary: R/bt-build/BattletoadsHeadless. Use --frames 1800, or set BATTLETOADS_STRESS_INPUT=1 and use --frames 3600.
  Downstream source changed after this fixture was built; do not silently recompile it and call the result the same baseline.
- Mario Paint: /Users/derrick/Documents/Programming/MarioPaint, ROM mp.sfc.
  O/mp-before.* / mp-fixed.* and OAM-FINDING.md describe the frozen-object/runtime-only comparison and current recording caveat.
- MMPR: /Users/derrick/Documents/Programming/MightyMorphinPowerRangers, ROM mmpr.sfc.
  An older isolated build was incomplete/no executable; there is no fresh MMPR validation claim in this phase.

### Read-only value probe

```sh
cd "$DKC3_COMPILER"
PROBE_ROM="$DKC3_ROM" PROBE_GEN="$DKC3_ATTRACT/gen" \
GOCACHE="$DKC3_ROOT/build/go-cache" \
  go test -overlay "$DKC3_ORACLE/value-overlay.json" ./internal/tooling \
  -run TestIsolatedValueProbe -v > "$DKC3_RUN/value-probe.log" 2>&1
```

The overlay contains absolute local paths; update it only in a new isolated copy if relocating. It is not needed for normal generation or tests.

## 8. Recommended continuation order

1. Audit Git and read the uncommitted query/tests before editing. Verify the frozen baseline hash.
2. Reproduce/inspect the bank helper blockers and the two distinct1C61 lifetimes using existing evidence. Do not repeat the OAM diagnosis.
3. Add a small synthetic contract for the missing helper/context/lifetime relationship. Resolve only what static evidence establishes; keep conditional inventory separate from native bank/width facts.
4. Feed newly recoverable inputs/targets through the shared dependency worklist and existing outer discovery loop. Preserve direct fast paths, live-M/X routing and continuation ownership.
5. Regenerate with the exact same19 inputs; inspect added targets and source diff, then build only after regeneration succeeds.
6. Require unchanged corrected4825-idle and2700-Start prefixes, including complete semantic edges and pixels. Run6000 attract; investigate the next failure if any rather than adding its observed address to roots.
7. For behavior changes, run Go/runtime conformance, representative cross-game replays and repeated warmed A/B performance checks. Document intentional divergences instead of normalizing them away.
8. Once a further coherent milestone is validated, report it for commit. No additional commit or push was requested as part of creating this handover.
