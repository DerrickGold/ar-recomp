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

`sky_palace_wide` is an opt-in 1,200-frame workload for the active Sky Palace
margin patch/restore path. It uses the same `fillmore-r1-natural.rec` replay
with a 16:10 square-pixel framebuffer. It is deliberately not part of the
default four-workload suite, preserving comparability with stored historical
baselines; select it explicitly with `--workload sky_palace_wide` when a phase
touches that seam.

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

Migrating the separated SIM capture from retained concrete PPU access to one
coherent callback-lifetime transaction retained every replay artifact hash.
CGRAM and OAM stay zero-copy, caller-owned plane storage remains directly
bound, and all selected capture policies are exchanged atomically and restored
exactly after scanout. Seven adjacent baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | +0.71% | -0.83% |
| `sim_actions` | +0.46% | -0.50% |
| `aitos_wide` | +0.18% | -0.68% |
| `death_heim_wide` | +0.94% | -1.19% |
| **Suite geometric mean** | **+0.57%** | **-0.80%** |

The portable movement remains below 1% in every workload. The native result
moves in the opposite direction across all four workloads, consistent with
ordinary code-layout sensitivity around a small active-SIM transaction rather
than broad per-frame overhead. Paired records are
`runner-abi-sim-capture-portable-final.json` and
`runner-abi-sim-capture-native-final.json` outside the source tree.

Migrating the render-only Sky Palace repair to a bounded sparse VRAM
compare/exchange retained every replay artifact hash. The service checks all
expected values before writing any replacement, and the adapter restores only
words that still hold its temporary value. Sorted-address validation avoids a
per-transaction duplicate-detection clear. Seven adjacent baseline/candidate
pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.23% | +0.48% |
| `sim_actions` | +0.22% | +0.00% |
| `aitos_wide` | +0.49% | +0.56% |
| `death_heim_wide` | +0.72% | +0.66% |
| **Suite geometric mean** | **+0.30%** | **+0.43%** |

The opt-in `sky_palace_wide` adjacent run, which executes the transaction from
gf276 onward, measured +2.15% portable and +0.79% native-SIMD. That focused
portable movement was already measured with the required adjacent A/B method
and remains below the 3% suite threshold and 5% per-workload threshold. Paired
records are `runner-abi-sky-vram-{portable,native}-final.json` and
`runner-abi-sky-vram-targeted-{portable,native}.json` outside the source tree.

Migrating widescreen exact-position and camera-relative OBJ metadata to a
validated batched ABI retained every replay artifact hash. Action mode makes
one clear and one bounded publish per object scan; SIM mode reuses its coherent
PPU snapshot and publishes at most once per source record. Seven alternating
adjacent baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.28% | -0.11% |
| `sim_actions` | -0.25% | -0.25% |
| `aitos_wide` | -0.76% | -0.84% |
| `death_heim_wide` | -0.23% | -0.26% |
| **Suite geometric mean** | **-0.38%** | **-0.37%** |

All movements favor the candidate and every final WRAM, SRAM, CPU-state, and
dispatch artifact hash matches. Paired records are
`runner-abi-obj-metadata-{portable,native}-final.json` outside the source tree.

Migrating action-background policy from concrete PPU/DMA access to fixed-width
DMA snapshots, bounded extent batches, atomic virtual-provider replacement,
and authentic-camera publication retained every replay artifact hash. Virtual
tile spans and preflight VRAM remain zero-copy. Seven alternating adjacent
baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.66% | +1.17% |
| `sim_actions` | -0.90% | +0.75% |
| `aitos_wide` | +0.60% | +0.19% |
| `death_heim_wide` | -0.61% | +0.95% |
| **Suite geometric mean** | **-0.39%** | **+0.76%** |

The native movement is distributed across active and inactive workloads and
never exceeds 1.17%, while the portable target improves overall. Eleven
GPU-backed Fillmore frames from gf1100 through gf2100 match the frozen
reference pixels exactly, as do final WRAM, SRAM, CPU state, and dispatch
artifacts. Paired records are
`runner-abi-action-bg-{portable,native}-final.json` outside the source tree.

Moving synchronous PPU scanout into the runner retained the exact line order,
all eight HDMA channels, hold-first/hold-last vertical margins, and vertical
IRQ handoff while removing the game's concrete per-line loop. Optional
diagnostics receive callback-lifetime state and zero-copy surface views; normal
frames do not take the line callback. Seven alternating adjacent
baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | +0.41% | -1.26% |
| `sim_actions` | +0.17% | -1.58% |
| `aitos_wide` | -0.45% | -0.31% |
| `death_heim_wide` | +0.08% | -0.64% |
| **Suite geometric mean** | **+0.05%** | **-0.95%** |

Every replay artifact hash matches. The portable result is neutral, while all
four native-host workloads improve. Eleven GPU-backed Fillmore frames from
gf1100 through gf2100 and the final WRAM, SRAM, state, and dispatch log also
match the frozen pre-scanout reference byte-for-byte. Paired records are
`runner-abi-scanout-{portable,native}-final.json` outside the source tree.

Replacing the game-specific RDNMI callback's concrete `Snes *` with an 8-byte
fixed-width flag context retained the existing single indirect call. Seven
alternating adjacent baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | +0.11% | +0.15% |
| `sim_actions` | +0.03% | +0.04% |
| `aitos_wide` | -0.06% | +0.10% |
| `death_heim_wide` | -0.09% | -0.41% |
| **Suite geometric mean** | **+0.00%** | **-0.03%** |

Every WRAM, SRAM, CPU-state, and dispatch artifact hash matches the frozen
post-scanout reference. Both configurations are neutral at suite scale. Paired
records are `runner-abi-rdnmi-{portable,native}-final.json` outside the source
tree.

Moving the recompiled game's frame/NMI lifecycle behind runner-owned timing
control retained the fresh RDNMI token, cancellation cleanup, `$4200` hardware
gate, and the distinction between persistent interrupt state and a newly
entered NMI. External consumers receive a fully validated public transaction;
the linked once-per-frame consumer uses a fixed direct adapter with the same
semantics. Seven alternating adjacent baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.30% | -0.01% |
| `sim_actions` | -0.13% | -0.24% |
| `aitos_wide` | +0.49% | +0.53% |
| `death_heim_wide` | +0.88% | +0.87% |
| **Suite geometric mean** | **+0.24%** | **+0.29%** |

Every WRAM, SRAM, CPU-state, and dispatch artifact hash matches the frozen
RDNMI reference. The public path packs state flags branchlessly, and the hot
linked path avoids descriptor validation; both configurations remain neutral
at suite scale. Paired records are
`runner-abi-game-timing-{portable,native}-final.json` outside the source tree.

Replacing the linked game's scattered concrete PPU display-control reads with
a packed fixed-width accessor retained force-blank load pacing, tile-animation
gating, visible-layer fallback, and developer diagnostics. External consumers
continue to use the full public PPU snapshot; the in-process hot seams copy only
the four control bytes they need. Seven alternating adjacent
baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.48% | -0.12% |
| `sim_actions` | -0.20% | -0.28% |
| `aitos_wide` | +0.21% | +0.32% |
| `death_heim_wide` | +0.10% | +0.88% |
| **Suite geometric mean** | **-0.10%** | **+0.20%** |

Every WRAM, SRAM, CPU-state, and dispatch artifact hash matches the frozen
game-timing reference. Both configurations remain neutral at suite scale.
Paired records are `runner-abi-ppu-display-{portable,native}-final.json`
outside the source tree.

Publishing copied input state removed the game's final direct controller-latch
reads and gives external tools both the runner-packed and SNES auto-joypad bit
orders with coherent frame identity. The only application consumers are an
explicit frame logger and an opt-in cheat, so normal frames do not invoke the
query. Seven alternating adjacent baseline/candidate pairs measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | +0.01% | +0.81% |
| `sim_actions` | -0.26% | +1.27% |
| `aitos_wide` | +0.16% | -0.73% |
| `death_heim_wide` | -1.36% | -1.01% |
| **Suite geometric mean** | **-0.37%** | **+0.08%** |

Every WRAM, SRAM, CPU-state, and dispatch artifact hash matches the frozen PPU
display-accessor reference. Both configurations remain neutral at suite scale.
Paired records are `runner-abi-input-state-{portable,native}-final.json`
outside the source tree.

Replacing the linked game's unversioned callback bag with cached, versioned
identity/lifecycle/execution/state/audio tables preserved all final artifacts.
Registration and provider binding remain outside the frame hot path. Seven
alternating adjacent baseline/candidate pairs against the frozen input-state
checkpoint measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | -0.83% | -0.11% |
| `sim_actions` | -0.58% | +0.10% |
| `aitos_wide` | -0.36% | +0.33% |
| `death_heim_wide` | -0.64% | +0.69% |
| **Suite geometric mean** | **-0.60%** | **+0.25%** |

Both configurations remain neutral at suite scale. Machine-readable paired
records are `runner-game-module-{portable,native}.json` outside the source
tree.

The bounded PPU frame-policy migration replaced ActRaiser's direct margin,
fill, row-band, vertical-clip, capture-padding, and HUD setters with a validated
BEGIN/publish/FINALIZE transaction. Seven alternating adjacent pairs against
the frozen versioned game-module checkpoint measured:

| Workload | Portable | Native-SIMD |
| --- | ---: | ---: |
| `mode7_worldmap` | +0.78% | -1.39% |
| `sim_actions` | +0.86% | -1.24% |
| `aitos_wide` | +0.24% | -0.45% |
| `death_heim_wide` | +0.92% | -1.60% |
| **Suite geometric mean** | **+0.70%** | **-1.17%** |

Positive is slower. Every WRAM, SRAM, CPU-state, and dispatch artifact hash
matches the frozen reference. The portable result remains below the 3% suite
gate and every workload remains below 2%; native-SIMD records a small gain. The
policy path avoids allocation, framebuffer copies, emulated-memory copies, and
surface generation invalidation. Machine-readable paired records are
`runner-abi-frame-policy-transaction-{portable,native}.json` outside the source
tree.

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
