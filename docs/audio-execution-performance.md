# Audio execution optimization

The runner keeps the slot-accurate DSP signal path. These optimizations reduce
dispatch and bookkeeping overhead, not audio fidelity:

- An optional, size-gated game opcode preflight avoids building mutable extension
  contexts for unrelated instructions. ActRaiser's instruction addresses and
  per-opcode free-cycle bookkeeping stay in its adapter.
- APU advancement batches countdown-only cycles, stopping at SPC instructions,
  timer edges, slot-0 port application, and slot-31 PCM publication. Instructions
  and extension hooks still observe the same clocks and ARAM.
- DSP spans dispatch once and fall through the existing slot bodies. All active
  banks remain interleaved per slot; extended sends still feed the single native
  echo pass. BRR, Gaussian interpolation, envelopes, noise, keying, register
  visibility and saturation are unchanged.

The original cycle stepper remains the tracing/diagnostic path and differential
reference. Production still uses at most 256 cycles per lock chunk, stops at the
exact audio demand, and keeps the existing snapshot format. Extended-voice
allocation, identity-aware blocking/replacement and display-independent audio
clocks are unchanged. Upload operations that wait for a specific SPC PC remain
single-cycle stepped.

Mirror refreshes deliberately remain eager: source-based muting, key-on
observations, state queries and serialization consume them. Lazy refresh is a
separate optimization, not part of this change.

## Validation

Local validation on macOS arm64, 2026-09-19:

- All 39 standalone runtime tests and nine application audio tests passed.
- DSP differential tests cover every starting slot, partial/full spans,
  mid-sample writes, extended banks, non-unity gains, noise, echo and PCM/state
  equality against the scalar path.
- The real SPC/APU differential fixture covers MMIO, timer read-clear and wrap,
  queued ports, BRR/echo RAM mutations, zero-cost hooks, stopped SPC, diagnostics,
  trace fallback and mid-slot save/load continuation.
- Callback-demand checks cover all 32 starting slots; cadence checks cover
  three simulated minutes each at 60/90/120 Hz with 44.1/48 kHz output.
- Focused APU differential, DSP and hook-contract tests passed ASan/UBSan. The
  long unoptimized sanitizer cadence run was stopped; its full non-sanitized
  counterpart passed.
- Five quiet adjacent A/B replay pairs per workload preserved WRAM, SRAM,
  dispatch-log and final-state hashes. These gameplay hashes are not a claim
  of whole-game PCM capture equivalence.

## Measurements

These are local CPU/time measurements, not Steam Deck results. DSP/APU focused
benchmarks used RelWithDebInfo; game replays used the release build. Medians:

| Workload | Reference | Batched | Reduction |
|---|---:|---:|---:|
| Native BRR DSP only | 398.81 ns/sample | 237.29 ns/sample | 40.5% |
| Native SPC + DSP + timers | 686.66 ns/sample | 583.74 ns/sample | 15.0% |
| Native + four paired effect banks, full APU | 1702.57 ns/sample | 1439.91 ns/sample | 15.4% |

DSP measurements used three alternating before/after runs of 500,000 samples;
APU measurements used three alternating runs per path of 1,000,000 samples in
the same binary. Both model a draining PCM consumer and verify matching hashes.
The synthetic APU fixture does not measure the game opcode-preflight gain.

The quiet release replay run reduced adjacent-pair elapsed time by 3.46% for
`sim_actions` and 3.13% for `aitos_wide` (3.29% combined). An earlier run that
overlapped build work was noisy and failed its timing gate; the quiet repeat
passed. No Steam Deck frame-time-tail or device-latency measurement was made.

To reproduce the focused APU check and benchmark:

```sh
cmake -S snesrecomp-go/runtime -B build-audio-check -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-audio-check -j4
ctest --test-dir build-audio-check --output-on-failure
build-audio-check/snesrecomp_runtime_apu_batch_test --benchmark
```

The root `actraiser_dsp_voice_benchmark` target exercises DSP-only cases.
For game A/B measurements use `snesbuild replay-bench` with
`tools/runner-bench.json`, a frozen `--reference-binary`, and native music
(`AR_MUSIC_REPLACEMENTS=0`, already specified by that suite). Run timing checks
without concurrent builds or sanitizer tests.
