# Frame-pacing first pass

This pass targets occasional CPU/I/O stalls, not steady-state GPU throughput.
It does not change simulation cadence, graphics settings, rendering quality or
the runner ABI.

## Changes

- Camera preference saves use `Settings_SaveDeferred`. The owner serializes
  the live registry; an SDL lifecycle adapter copies immutable path/text bytes
  and performs the existing atomic durable write on one sleeping worker.
  Storage is bounded to one active and one pending snapshot. New pending
  snapshots replace older ones for the same destination. A writer cannot
  switch destinations. Menu saves use that same queue and wait for the latest
  durable result. Success clears prior failure state, including after a failed
  background attempt; no separate direct-write path bypasses bookkeeping.
- Shutdown drains the worker. A failed latest write is reported and retried
  using the live registry. Replay/diagnostic no-write protection remains on the
  submitting owner. Accepted does not mean durable yet: abrupt termination can
  lose a pending **preference** change. Battery SRAM saves remain synchronous
  with their existing durability and fatal-error behavior.
- Music manifest loading retains up to 128 MiB of **compressed** OGG data at
  boot, sharing identical paths. Cached streams open/decode/seek from memory
  without accessing the filesystem under the audio lock. The bounded immutable
  cache needs no runtime eviction. Assets that cannot fit retain existing
  file-backed streaming; startup logs report both counts and resident bytes.
  Decoding, resampling, synchronization, pause and one-shot timing are unchanged.
  Load/reload and shutdown require the audio producer to be stopped; decoders
  close before their backing bytes are freed.
- Cold world height sampling, globe ground samples, and SIM globe grid
  construction/embedding use the existing bounded render helper pool. The
  owner prepares mutable inputs; jobs read frozen inputs and write disjoint
  rows, then join before publication or GPU use. Serial execution uses the same
  functions. Existing `AR_RENDER_WORKERS=0..3` diagnostics remain available.

## Validation

- Immutable preference snapshots, coalescing behind a deliberately blocked
  writer, caller-buffer independence, destination rejection, failure recovery,
  synchronous write ordering, replay protection and shutdown draining.
  A real-filesystem integration test blocks the atomic temporary path, then
  verifies that a successful synchronous retry prevents an erroneous shutdown
  rewrite of later unsaved preferences. It also checks genuine synchronous
  failures and rejection of another destination preserve failure state.
- A generated sine-wave OGG checks identical file/memory PCM, cached playback
  after source removal, pause/resume, restart, looping, duplicate-path ownership,
  reload and idempotent shutdown. No copyrighted audio is used.
- Metal world/SIM GPU tests compare exact serial/helper pixels, including cold
  caches and navigation-to-SIM transitions. The cold-build fixture warms each
  worker count once, then runs two ABBA blocks for each scene. It reports narrow
  CPU scope timings, not game FPS or GPU execution time. Use an optimized build
  for timings; correctness checks also run in Debug.
- Settings, music and metrics pass AddressSanitizer/UndefinedBehaviorSanitizer.
  The settings writer also passes ThreadSanitizer with real SDL synchronization.
  Existing save-system, helper-pool, terrain, camera and layer-boundary tests
  remain part of the targeted regression set.

### Local optimized probe (2026-09-21)

Mac, RelWithDebInfo, Metal, synthetic world/SIM fixtures, four measured cold
builds per worker count after warm-up (two ABBA blocks). Median **sum of the
targeted CPU scopes**, serial versus three helpers:

| Scene | Serial | Three helpers |
| --- | ---: | ---: |
| Navigation: terrain + ground samples | 0.86 ms | 0.55 ms |
| SIM: terrain + ground samples + curved grid | 1.34 ms | 0.72 ms |

These are worker-path comparisons, not whole-game before/after frame times;
serial runs use the same refactored implementation. The synthetic prior is
already warm. GPU readback, the rest of terrain construction, asset loading
and presentation are outside these scopes. Serial/helper images match exactly.

An isolated 150-tick headless game smoke test loaded all eight available music
entries into 29,753,151 bytes of compressed storage (about 28.4 MiB), started
the title track, and shut down cleanly with no file-backed entries. User saves
and settings were not used or modified.

## Remaining scope

SRAM disk latency, decoder CPU work under the audio lock, uncached music I/O,
and owner-side terrain allocation/prior construction/GPU uploads can still
cause spikes. The new log stages distinguish these from ordinary frame work;
they are not a complete cross-scene hitch recorder. No Steam Deck end-to-end
frame-pacing gain is claimed by these local correctness/microbenchmark checks.
