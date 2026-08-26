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
