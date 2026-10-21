# Stationary actor-motion rejection prototype

Explored 2026-10-19 after the rendering integration in `f7fb6436`.
`AR_GPU_ACTOR_STATIONARY_REJECT` follows `gpu_prototype_policy.h` since
October 20, 2026: on by default for Metal and Vulkan, reference search on D3D12,
`1` forces it on and `0` forces the reference algorithm. No user setting is added.

## Exact shortcut

Actor motion currently evaluates 225 displacement costs for every occupied
16×16 block before an ordered, neighbour-seeded search. A nonzero vector must
improve the stationary cost by at least 64, and by at least 12 percent. When
the stationary cost is below 64, no nonnegative candidate cost can meet that
threshold. The cost shader can therefore skip the 27×27 target footprint and
225 candidate evaluations, and the ordered search can publish a zero vector.

The prototype first preserves the existing full-block occupancy scan. For an
occupied block, 16 lanes compare the same sample positions used by the search
and reduce their differences. Header state 2 denotes an occupied block that
cannot pass the confidence threshold. It is distinct from an empty block:
otherwise the empty-block halo would incorrectly import motion from a moving
neighbour. Zero vectors still participate in left/top seeding exactly as before.
Unwritten candidate costs are never consumed for rejected blocks.

This adds no GPU pass, resource allocation, CPU pixel processing, GPU readback
or synchronization wait to gameplay. It reuses the existing cost buffer and
workgroup scratch. The analysis-mode uniform selects reference or rejection;
public per-plane enable flags, synthesis, lighting and gameplay are unchanged.
MSL, SPIR-V and DXIL are generated from the same GLSL source.

## Isolated measurements

Apple M2, Metal. The test performs eight analyses per submission, waits for
completion, and divides elapsed time by eight. Each mode has eight warm-up
batches and 64 measured batches. Off/on/on/off order is repeated, with the
variant order reversed for the second sweep. Both textures are resident before
measurement; uploads, downloads, shader creation and CPU-oracle comparisons
are excluded. These are encoding-through-completion measurements, not hardware
kernel timestamps, individual-frame percentiles or full-game frame times.

Mean milliseconds per analysis, averaging four rounds per mode:

| 640×352 synthetic content, four priorities | Reference | Rejection | Change |
| --- | ---: | ---: | ---: |
| Stationary, including a dense layer | 0.938 | 0.352 | −62.5% |
| Moving, including a dense layer | 0.943 | 0.968 | +2.7% |
| Mixed stationary/moving blocks | 1.286 | 0.789 | −38.7% |
| Sparse stationary sprites | 0.345 | 0.315 | −8.9% |
| Sparse moving sprites | 0.339 | 0.344 | +1.4% |
| Empty layers | 0.308 | 0.304 | Within run variation |

The original committed shader was also compiled into the same optimized
harness as a separate control. Its timings were consistent with the new
shader's disabled path. The shortcut has a real cost when nothing can be
rejected; it is not a universal speedup.

Two alternatives were measured and discarded. Fusing the stationary comparison
into the occupancy scan caused extra texture reads even for empty blocks and
regressed sparse scenes. A serial, short-circuiting comparison reduced some
checking work but lost much of the dense stationary gain. The retained version
checks only occupied blocks and reduces 16 samples in parallel.

## Gameplay timing and decision

The same release binary was run eight times under Metal/VSync on the internal
60 Hz display, at 960×540 logical / 1920×1080 backing resolution. Bloodpool
02:01 uses the stationary environmental-effects replay; Marahna 05:01 uses the
scrolling replay. Each scene has off/on and on/off pairs, 2,600 ticks per run,
with ticks 1600–2500 selected to exclude loading/prewarming. Native preparation
workers and direct output are enabled on both sides.

The observer identifies the command containing the 640×5632 endpoint-atlas
render target, then correlates its completed Metal GPU timestamps. This measures
packing plus global and actor motion together; it is not an actor-kernel-only
measurement and can include GPU dependency waits. Each run contributes 900
commands. Binary, replay, settings, assets and final WRAM match within each scene.
All selected commands complete, with foreground/unlocked timing eligibility.

| Scene | Reference means, two runs | Rejection means, two runs | Pooled mean |
| --- | ---: | ---: | ---: |
| Bloodpool | 0.694 / 0.683 ms | 0.738 / 0.645 ms | 0.688 → 0.692 ms |
| Marahna | 2.903 / 2.900 ms | 2.754 / 2.879 ms | 2.901 → 2.816 ms |

Bloodpool's mean is effectively unchanged, with the paired differences changing
sign. Marahna's pooled mean improves about 2.9%, but its command-duration p99
does not improve: 5.689 → 5.814 ms. The isolated dense stationary win is real,
but these replays do not establish a broad gameplay pacing improvement.

OS display-interval p99 stays near 16.667 ms. Isolated longer intervals remain:
100 ms in one Bloodpool reference run, and 50/33 ms in the second Marahna
reference/rejection pair. Their cause is not established by this experiment.
The previously observed launch-dependent extra refresh of presentation delay
also persists. Neither the favorable tails of one run nor the display's 60 Hz
limit is evidence that this shortcut solves those stalls or reaches Deck 90 Hz.

At the time of this measurement the experiment stayed off: it is useful for
target-GPU comparisons and helps workloads with many stationary actors, but it
had shown no pacing effect. It was promoted on October 20, 2026 as an exact
work reduction once the Deck arms showed cadence was never the limiting factor.

### Steam Deck follow-up, October 20

[Vulkan validation](steam-deck-prototype-validation.md) passes the CPU oracle
and 134 combined-prototype gameplay frame comparisons. The isolated benchmark
again helps stationary/mixed content, but the four Bloodpool game comparisons
show only a small CPU difference and no compelling host-pacing benefit.
The recommendation remains to keep this optional; its synthetic savings should
not be treated as a general 90 Hz pacing improvement.

## Validation

- Both modes match the CPU oracle for 13 inputs at three dimensions, including
  partial blocks, empty/disabled priorities, disappearing sprites, transparent
  RGB, stationary blocks next to moving blocks, and stationary costs 63/64/65.
  The cost headers explicitly verify the rejection boundary. Mesh vertices,
  companion material alignment and current endpoints are checked as well.
- Thirteen focused Metal rendering tests pass, including reference/direct
  output, resident effects and native preparation workers.
- All 134 whole-game comparison frames match exactly: 48 in scrolling Marahna
  05:01 at phase 0.5, and 43 each in Fillmore 01:01 at phases 0.25 and 0.75.
  BG1 travel is 1,024×147 and 1,044×188 pixels respectively. Each pair uses the
  same binary, assets, replay and final game state. Both sides use direct output;
  Fillmore retains resident effects with no motion metadata download.
- Shader regeneration checks pass for all three formats. The changed native
  module compiles with strict C warnings, and renderer/private-header boundary
  checks pass. Vulkan/DX12 runtime and Steam Deck performance are not tested here.

## Reproduction

Build `actraiser_gpu_block_motion_test`; CTest entries
`actraiser_gpu_block_motion` and `actraiser_gpu_block_motion_stationary` exercise
the two modes. Run the executable with `--benchmark` for the isolated ABBA probe.
`tools/compare_action_gpu_frames.py --actor-stationary-reject 0|1` selects the
mode for matched gameplay captures and checks the enabled diagnostic.

Ignored local evidence is in `runs/gpu-motion-pruning-20261019/`: all attempted
shader variants, original-shader controls, benchmark binaries/logs, matched
game captures, build/check logs and presentation traces. The experiment remains
off until its workload tradeoff is validated on the target GPUs.

## Next optimization candidates

The background motion cost shader prunes against nine displacements centred
on zero. Scrolling by more than one pixel can leave that upper bound loose,
requiring much more of each of the 225 candidate evaluations. A useful next
experiment is to centre the nine initial candidates on known camera movement
or a previous GPU motion result, then evaluate their costs against the current
textures. Every initial candidate must stay within the existing legal search
range, and pruning must remain strictly greater-than. That would preserve the
global winner and tie order while potentially reducing work on scrolling
backgrounds. This is a candidate for measurement, not an implemented speedup.

Removing endpoint packing remains a larger architectural option. It requires
two immutable GPU input versions and correct RGBA/BGRA conversion; retaining a
mutable SDL texture handle is insufficient. Measure packing separately from
motion search before trading its copies for more complicated resource ownership.
