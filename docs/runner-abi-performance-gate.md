# Runner ABI performance gate

Runner ABI work must retain correctness and portable performance across more
than one convenient scene.  `tools/benchmark_runner_replays.py` fixes the
inputs and measures four representative workload shapes:

| Workload | Frames | Coverage |
| --- | ---: | --- |
| `mode7_worldmap` | 6,000 | repeated authentic-width Mode 7/world-map transitions |
| `sim_actions` | 6,000 | simulation mode and the recording's pinned gameplay settings |
| `aitos_wide` | 4,000 | wide action mode, scrolling, and background HLE |
| `death_heim_wide` | 4,000 | late-game action, effects, and wide margins |

The primary gate is a release build with `SNESRECOMP_ENABLE_SIMD=OFF`.  This
keeps an ABI change honest on platforms without a specialized implementation.
A normal `ON` build is the secondary guardrail for the current host's shipping
path.  Neither build enables the watchdog, sanitizers, renderer diagnostics,
or the trace ring buffer.

## Capture a baseline

Use distinct build directories and do not rebuild them during the ABI series:

```sh
cmake -S . -B build-abi-baseline-portable -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAR_SIM3D_TERRAIN_ELEVATION=ON \
  -DAR_WATCHDOG=OFF -DAR_SANITIZE=OFF -DBUILD_TESTING=OFF \
  -DSNESRECOMP_ENABLE_SIMD=OFF
cmake --build build-abi-baseline-portable --target ActRaiserRecomp -j 8

python3 tools/benchmark_runner_replays.py \
  --binary build-abi-baseline-portable/ActRaiserRecomp \
  --label portable --runs 7 --warmups 1 \
  --output benchmarks/runner-abi-baseline-portable.json
```

Repeat with a separate build configured with
`SNESRECOMP_ENABLE_SIMD=ON` for the native-host result.  The script strips
ambient `AR_*`, `SNESRECOMP_*`, and `SNESREF_*` variables, uses dummy SDL
drivers, runs in disposable directories, and records the ROM, save, replay,
settings, config, binary, and final-state hashes.

## Pre-ABI checkpoint (2026-08-26)

Commit `6e0da75` was measured on Darwin arm64 with seven runs after one
full-suite warmup.  Lower time and higher emulated FPS are better:

| Workload | Portable median | Portable FPS | Native-SIMD median | Native FPS |
| --- | ---: | ---: | ---: | ---: |
| `mode7_worldmap` | 1.4271 s | 4,204.21 | 1.3237 s | 4,532.62 |
| `sim_actions` | 2.3538 s | 2,549.09 | 2.1126 s | 2,840.15 |
| `aitos_wide` | 2.4930 s | 1,604.52 | 2.4703 s | 1,619.21 |
| `death_heim_wide` | 1.6910 s | 2,365.44 | 1.6251 s | 2,461.35 |
| **Suite geometric mean** | — | **2,525.41** | — | **2,676.34** |

All seven repetitions within each workload produced identical final artifact
hashes.  Portable and native-SIMD hashes also match each other for all four
workloads.  The machine-readable records are
`benchmarks/runner-abi-baseline-portable.json` and
`benchmarks/runner-abi-baseline-native.json`.

## ABI v2 OBJ value checkpoint (2026-08-26)

The fixed-width `SrPpuObjPart` migration was measured as an adjacent A/B pair
against the frozen ABI-v2 PPU-query build. All replay artifact hashes matched.
The median candidate/reference deltas were:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | +0.94% | -1.04% |
| `sim_actions` | +0.15% | -0.34% |
| `aitos_wide` | -0.12% | -0.14% |
| `death_heim_wide` | -0.05% | -0.57% |
| **Suite geometric mean** | **+0.23%** | **-0.52%** |

Positive is slower. Both configurations pass the phase gate; the portable
suite change is within run-to-run noise and the native result is a small gain.
Machine-readable paired results were retained outside the source tree as
`runner-abi-concrete-obj-type-portable.json` and
`runner-abi-concrete-obj-type-native.json`.

The subsequent application-wide migration from internal
`kPpuOverlaySource_*` names to ABI-v2 `SR_PPU_OVERLAY_*` IDs also preserved
every replay artifact hash. Adjacent paired suite deltas were -0.02% portable
and +0.06% native-SIMD; the individual workload deltas stayed between -0.16%
and +0.21%. Machine-readable results are
`runner-abi-overlay-ids-v2-portable.json` and
`runner-abi-overlay-ids-v2-native.json` outside the source tree.

Rebaselining every public descriptor extent name from v1 to the sole v2
contract again preserved all replay artifact hashes. Adjacent suite deltas
were -0.02% portable and -0.14% native-SIMD; all individual workloads stayed
within -0.45% to +0.20%. The paired records are
`runner-abi-descriptor-v2-rebaseline-portable.json` and
`runner-abi-descriptor-v2-rebaseline-native.json` outside the source tree.

Migrating developer tools from a retained concrete PPU pointer to v2 snapshots
and borrowed OAM spans preserved every replay artifact hash. The developer
queries are absent from normal replay frames; adjacent suite deltas were
-0.03% portable and -0.14% native-SIMD, with every workload between -0.25%
and +0.04%. Paired records are `runner-abi-dev-tools-ppu-v2-portable.json`
and `runner-abi-dev-tools-ppu-v2-native.json` outside the source tree.

Removing the remaining concrete size/header dependencies from the frame-slot
and two host-policy modules preserved all replay artifact hashes. Adjacent
suite deltas were +0.00% portable and -0.06% native-SIMD; individual workloads
stayed between -0.14% and +0.10%. Paired records are
`runner-abi-surface-limits-v2-portable.json` and
`runner-abi-surface-limits-v2-native.json` outside the source tree.

Publishing Mode-7 state plus the resolved background-coordinate service, then
migrating the scene inspector and its contract test to ABI v2, preserved every
replay artifact hash. The new service is idle outside explicit developer
inspection. Existing PPU snapshots copy the additional fixed 16-byte matrix;
the measured adjacent suite deltas were +0.05% portable and -0.06%
native-SIMD. Individual workload deltas were -0.17% to +0.14% portable and
-0.68% to +0.65% native-SIMD. Paired records are
`runner-abi-ppu-inspection-v2-portable.json` and
`runner-abi-ppu-inspection-v2-native.json` outside the source tree.

Adding synchronous, capacity-checked PPU output/margin control and migrating
the HD replacement host preserved every replay artifact hash. The final
adjacent suite deltas were +0.25% portable and -0.08% native-SIMD; individual
workloads stayed between +0.01% and +0.65% portable and between -0.52% and
+0.15% native-SIMD. An initial attempt to update the surface generation from
the game's per-frame margin setters regressed both native wide-action samples
by roughly 6%. Moving that generation update to explicit ABI reconfiguration
restored parity while keeping ABI-created surface views coherent. Paired
records are `runner-abi-ppu-output-control-v2-portable.json` and
`runner-abi-ppu-output-control-v2-native.json` outside the source tree.

Adding atomic, synchronous overlay-capture and Mode-7-override claims, then
migrating the HD manifest policy from concrete PPU access, preserved every
replay artifact hash. The evaluator performs no PPU query when no replacement
art is loaded and otherwise takes one coherent snapshot per enabled frame.
The final adjacent suite deltas were +0.04% portable and +0.57% native-SIMD:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | +0.12% | +0.05% |
| `sim_actions` | -0.21% | -0.02% |
| `aitos_wide` | +0.07% | +1.55% |
| `death_heim_wide` | +0.20% | +0.69% |

The native wide-action shift reproduced across the paired run even though the
new ABI path is idle without loaded art; symbol inspection attributes it to
code-layout sensitivity rather than added frame work. It remains below the 2%
mandatory-rerun band and all phase limits. Paired records are
`runner-abi-ppu-capture-control-v2-portable.json` and
`runner-abi-ppu-capture-control-v2-native.json` outside the source tree.

Replacing the diorama renderer's two private PPU allocation-limit names with
the equivalent public ABI constants preserved every replay artifact hash.
Adjacent suite deltas were +0.01% portable and +0.03% native-SIMD; individual
workloads stayed between -0.03% and +0.04% portable and between -0.06% and
+0.18% native-SIMD. Paired records are
`runner-abi-diorama-surface-limits-v2-portable.json` and
`runner-abi-diorama-surface-limits-v2-native.json` outside the source tree.

Publishing the CPU arithmetic-unit state query/restore and migrating the
opt-in world-map comparison oracle preserved every replay artifact hash. The
new service is dormant in all normal replay frames. Adjacent suite deltas were
+0.20% portable and -0.52% native-SIMD; individual workloads were -0.66% to
+0.78% portable and -1.25% to +0.15% native-SIMD. The opposite action-scene
movement between configurations is consistent with code-layout sensitivity,
not service execution. Paired records are
`runner-abi-cpu-math-state-v2-portable.json` and
`runner-abi-cpu-math-state-v2-native.json` outside the source tree.

Removing concrete runner access from the host bootstrap and presentation
orchestrator preserved every replay artifact hash. Frames without an eligible
enhanced town canvas issue no new query; eligible frames take one compact PPU
snapshot and borrow VRAM/CGRAM zero-copy. Adjacent paired deltas were:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.36% | +0.15% |
| `sim_actions` | +0.24% | +0.09% |
| `aitos_wide` | +0.05% | -0.10% |
| `death_heim_wide` | +0.02% | -0.12% |
| **Suite geometric mean** | **-0.01%** | **+0.01%** |

An additional GPU-backed enhanced Aitos replay exercised the active town view,
retained identical WRAM, SRAM, and dispatch artifacts, and sustained 142-145
presentations per second in its settled town windows. Paired records are
`runner-abi-main-boundary-portable.json` and
`runner-abi-main-boundary-native.json` outside the source tree.

Migrating native-audio provenance tracing to the public observer retained one
unlikely disabled check at each pre-existing APU/SPC seam and performs no
payload construction unless a subscriber is active. Every replay artifact hash
matched. Adjacent paired deltas were:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.03% | -0.06% |
| `sim_actions` | -0.33% | +0.12% |
| `aitos_wide` | +0.40% | -0.12% |
| `death_heim_wide` | +0.02% | +0.10% |
| **Suite geometric mean** | **+0.02%** | **+0.01%** |

A separate 240-frame trace-enabled replay wrote all four CSV reports, retained
valid SPC-PC provenance, and aggregated 454 DSP writes. Paired records are
`runner-abi-audio-trace-portable.json` and
`runner-abi-audio-trace-native.json` outside the source tree.

Removing the unused standalone `SpcPlayer` adapter retained every replay
artifact hash. Seven adjacent baseline/candidate pairs measured the following
elapsed-time deltas:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.81% | +0.76% |
| `sim_actions` | -0.69% | +0.81% |
| `aitos_wide` | +0.12% | +0.09% |
| `death_heim_wide` | -0.70% | +0.51% |
| **Suite geometric mean** | **-0.52%** | **+0.54%** |

The adapter had no per-frame caller, so these small opposing configuration
deltas are treated as link-layout variation rather than an optimization. Paired
records are `runner-abi-audio-cleanup-portable.json` and
`runner-abi-audio-cleanup-native.json` outside the source tree.

Migrating the live SPC image/sample adapter to a callback-lifetime upload
transaction and an atomic deferred PC control retained every replay artifact
hash. The new control object is deliberately last in the canonical source
list: it is cold, and keeping it there avoids perturbing established hot runner
link order. Seven adjacent baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | +0.90% | -1.03% |
| `sim_actions` | +0.40% | -0.53% |
| `aitos_wide` | -0.09% | +0.17% |
| `death_heim_wide` | +0.94% | -0.57% |
| **Suite geometric mean** | **+0.54%** | **-0.49%** |

The public deferred operation runs only while a resident upload completion is
pending; normal frames make no call. Paired records are
`runner-abi-spc-upload-portable-final.json` and
`runner-abi-spc-upload-native-final.json` outside the source tree.

Migrating native Music/SFX classification to a validated callback-lifetime
route and moving live native-bus gains behind public control retained every
replay artifact hash. The hot DSP-write transaction borrows ARAM and current
bus labels read-only, returning no more than two small updates; it performs no
snapshot or bulk copy. Seven adjacent baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | +0.03% | +0.07% |
| `sim_actions` | -0.36% | +0.27% |
| `aitos_wide` | +0.11% | +0.08% |
| `death_heim_wide` | +0.18% | -0.08% |
| **Suite geometric mean** | **-0.01%** | **+0.08%** |

Paired records are `runner-abi-audio-mixer-portable.json` and
`runner-abi-audio-mixer-native.json` outside the source tree.

Migrating the native extended-audio path to fixed-width game-adapter callbacks
retained every replay artifact hash. ARAM stays zero-copy, DSP mutation is
validated and applied by the runner, and the upload callback reuses the
existing APU lock. Seven adjacent baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.09% | +0.64% |
| `sim_actions` | -0.07% | +0.77% |
| `aitos_wide` | +0.56% | +0.18% |
| `death_heim_wide` | -0.67% | +1.45% |
| **Suite geometric mean** | **-0.07%** | **+0.76%** |

The portable suite is neutral and every native workload remains below the 2%
mandatory-rerun band. Paired records are
`runner-abi-audio-extension-portable.json` and
`runner-abi-audio-extension-native.json` outside the source tree.

Extending the public OBJ service with caller-owned range resolution and direct
resolved/synthetic-part rasterization, then migrating the SIM atlas, retained
every replay artifact hash. The normal path reuses parts already recorded by
the game and writes directly into the final packed atlas rectangle. Seven
adjacent baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.81% | +0.04% |
| `sim_actions` | -0.84% | -0.03% |
| `aitos_wide` | -0.19% | +0.08% |
| `death_heim_wide` | -0.02% | -0.04% |
| **Suite geometric mean** | **-0.47%** | **+0.01%** |

Paired records are `runner-abi-sim-atlas-portable.json` and
`runner-abi-sim-atlas-native.json` outside the source tree.

## Phase acceptance

The target is no measurable regression.  A phase is accepted automatically
when all of the following hold:

1. Every final WRAM, SRAM, CPU state, and dispatch artifact hash matches.
2. No workload's seven-run median regresses by more than 5%.
3. A measured 2-5% regression is rerun as an adjacent baseline/candidate pair
   before acceptance; thermal drift and host load are too large to make a
   one-session JSON comparison decisive in that band.
4. The portable suite geometric-mean throughput should not regress by more
   than 3%.  A larger gain in one scene does not buy a regression in another.

Build each phase in a fresh directory, then compare it with the checked-in
portable result:

```sh
python3 tools/benchmark_runner_replays.py \
  --binary build-abi-phase/ActRaiserRecomp \
  --runs 7 --warmups 1 \
  --compare benchmarks/runner-abi-baseline-portable.json \
  --max-regression-percent 5 --max-suite-regression-percent 3
```

For a marginal result, rerun the frozen baseline binary immediately before or
after the candidate with true adjacent pairs:

```sh
python3 tools/benchmark_runner_replays.py \
  --binary build-abi-phase/ActRaiserRecomp \
  --reference-binary build-abi-baseline-portable/ActRaiserRecomp \
  --workload mode7_worldmap --runs 7 --warmups 1
```

The harness alternates which binary runs first in each pair and reports the
median of the adjacent candidate/reference ratios.  Do not compare numbers
captured with different ROM, replay, save, settings, frame count, CMake mode,
diagnostics, or SDL driver configuration.
