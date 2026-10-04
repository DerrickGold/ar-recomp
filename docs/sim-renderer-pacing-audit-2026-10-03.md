# SIM vs action renderer: performance and frame-pacing audit — 2026-10-03

What the action (Diorama) pipeline does that the SIM renderer does not, which
of those ideas are worth carrying over, and what the SIM town actually spends
its frame on. Measured on the developer's M2 at 60 Hz with the developer's own
presentation settings; methods, caveats and evidence are at the end.

## Summary

The two renderers differ in one structural way. Action streams: a producer
thread runs emulation, PPU scanout and capture on the source clock and
publishes owned packets; the main thread only uploads, draws and presents, and
picks which packet to show against a predicted refresh. SIM (town, world
navigation, Sky Palace) runs the old synchronous loop: every stage runs
serially on the main thread, and a wall-clock accumulator decides how many
ticks to run each iteration (`src/app/game_loop.c:989`). Streaming is gated to
Diorama frames (`game_loop.c:956`, `HostFramePacket_Supports` in
`src/host/frame_queue.h`).

Measured consequences, SIM Aitos voxel town with connected globe, 60 Hz VSync:

- **Throughput is not the problem on this Mac.** The SIM main thread is busy
  about 5.4 ms of each 16.7 ms frame (action: about 1.2 ms on main plus
  4.0 ms on the producer). Every stage still sits on the critical path.
- **Development hitches are the largest visible artifact.** Each town
  development change, usually a single tile, rebuilds the whole connected-globe
  surface (23,339 quads) and its water mask inline in presentation, alongside
  voxel and upload work. Frames take 33–45 ms, two or three in a row. In 45 s
  of an idle town this happened 9 times.
- **Both renderers judder on the 60.0988/60 Hz beat.** SIM holds a frame on
  1.0–1.3% of presents (each paired with a skipped tick), in 1–2 s bursts
  about every 10 s. In the same session, action's streamed timeline selection
  still scored 4.0–4.3% irregular presents with the same 10 s burst pattern.
  Porting the stream architecture alone would not fix SIM cadence.
- **Root cause of the remaining beat judder (both paths):** the refresh
  predictor filters the period but not the phase
  (`HostFrameRefreshClock_Observe`, `src/host/frame_playout.h:23`), so its
  predicted refresh carries the full present-return jitter (p95 ±1.3 ms here).
  Replaying recorded traces offline, a phase-filtered predictor cuts selection
  errors 2–4× on every clean Mac trace (table below).

## Recommendations, in suggested order

1. **Diagnostics parity first (small).** SIM cadence is currently invisible:
   the per-present sync trace only records Diorama frames
   (`game_loop.c:1076`), and the overlay's source holds/skips only count
   native streamed playback (`game_loop.c:725`). Record every synchronous
   present (tick delta, present return, produced flag) in a format
   `tools/analyze_frame_pacing.py` can score, and count sync-path holds
   (zero-tick presents) and skips (multi-tick presents) in the overlay. This
   turns the per-window evidence below into a per-present before/after gate.

2. **Phase-filter the shared refresh clock (small; measured offline).** Keep
   the period EMA. Add a filtered phase: advance by one period, correct by
   gain × error (1/8 performed well), and resync when the error exceeds half a
   period (missed refresh, VRR). This benefits action immediately. The earlier
   offline analysis (fitted vsync timeline ≈0.35%) pointed the same way; the
   shipped predictor kept the raw phase.

3. **Schedule-based tick release in the synchronous loop (moderate).** Port
   the playout rule, not the thread. Keep a wall-anchored source clock
   (`next_source_ns += T`, as `ProduceFrameStream` does). Each iteration,
   release the ticks whose scheduled time is at or before
   `HostDisplay_NativeFrameSampleTime(now)` minus the native delay. Keep the
   accumulator where there is no refresh estimate and for below-source Limit
   coalescing. Game speed stays wall-clock exact; only the decision phase
   changes. This covers SIM town, world navigation, Sky Palace, flat action
   and menus; the title screen showed the same hold bursts (0.10–0.11 per
   present). Expected SIM gain is unmeasured until item 1 exists.

4. **Take development-change rebuilds off the presentation critical path
   (moderate; measured cost).** This applies action's rule that nothing heavy
   rebuilds inside a draw.
   - `SimGlobeBuildSurface` (`src/sim/world_nav/present_sim_globe.c:96`) keys
     on the global `SimWorldMap_GeographySerial`, which any changed world tile
     bumps (`src/sim/sim_world_map.c:190`). With the detailed town shown, the
     globe drops the active town's own cells (`SimGlobeKeepCell`, line 58), so
     an in-town development change may republish identical geometry. Verify by
     hashing the published source across one event. If it is unchanged, key the
     rebuild on a digest of only the inputs it consumes. If it did change,
     build on a worker (`g_world.dirty` already records changed tiles) and swap
     on a later frame; one stale frame of out-of-town ground is invisible.
   - The water mask (`PresentSimGlobeWater_Prepare`) has the same whole-ground
     key.
   - The "SIM upload" stage peaks at 21–25 ms in the same windows; the
     sub-step was not isolated. Candidates are the per-object palette rebuild
     on a voxel scene-serial change and the mountain-atlas re-upload in
     `SimBackgroundVoxelRenderer_Upload`. The producer-side voxel scene
     rebuild ("canvas enhance", up to 19.5 ms) would move off main under
     item 6.

5. **Request-mask the SIM plane capture and upload (moderate).** Action
   captures and uploads only `request ∩ content` planes. SIM always captures
   all six BG planes (`src/sim/sim3d/sim3d.c:566`), and
   `Sim3D_PlaneTextureUploadMask` (`sim3d.c:106`) uploads BG1 even when voxels
   or the globe replace it; the projected profile then never samples it
   (`present_sim3d.c:1845`). BG1 low and high account for about 1.8 of the
   6.1 MiB/present of mirror comparison measured in the town, plus their
   rasterization (share of the 1.56 ms scanout unmeasured) and an upload
   whenever they change. Constraint: `RestoreTownHudPolicy` reads BG1 under
   the HUD rows, and menus, the flat fallback and comparison picture-in-picture
   need all planes. So narrow BG1's capture rectangle to the HUD rows when the
   resolved profile does not sample it, and restore full capture when a menu,
   the flat stage or comparison is active.

6. **Stream SIM through the producer (large; decide on Deck data).** This
   would move emulation, PPU and capture, town canvas and voxel scene work
   (about 3 ms per tick on this Mac) off the main thread and shield
   presentation from producer-side spikes. It does not fix item 4's
   presenter-side rebuilds. At 60 Hz on the M2 the main thread already idles
   about 11 ms per frame, so the case rests on 90/120 Hz and Steam Deck
   measurements, which were not taken here. Blockers, all present-time
   reads or writes of producer state:
   - draw-time voxel queries (`SimBackgroundVoxels_CellIsMountain`,
     `MountainSurface` and `StructureHeight` from `present_sim3d.c`)
   - the crater-anchor write-back to the metadata producer
     (`PublishSimCraterAnchor`, `present_sim3d.c:1496`)
   - producer dirty-rect cursors consumed during upload (town canvas, voxel
     ground and atlas)
   - boot-owned product buffers borrowed by `FrameSlot`
   - world-map and world-navigation globals read by the globe presenter

   Prefer a generation handoff of resident products (double-buffered,
   serial-tagged, dirty lists carried in the packet) over action-style
   per-packet pixel copies: one action packet reserves 13.5 MB of pixels,
   while SIM's large products change rarely.

Not worth carrying over:

- **Image-space frame interpolation.** The SIM camera is already
  presentation-owned and re-presented at display rate.
- **The GPU BG-packet decode for BG1 in voxel/globe mode.** Skipping the
  capture is cheaper. The packet path only helps the canvas/flat ground.
- **Per-packet full-surface copies.** See item 6.

## Measurements

All numbers come from a single run per configuration on the same pinned
binary. Stage means are frame-weighted over settled one-second windows. Stages
nest: for example, "presentation" includes the globe underlay. Main-thread busy
time is the frame period minus pacing wait.

### Main-thread cost per present (60 Hz VSync)

| | SIM town, idle | SIM town, moving | Action, streamed |
| --- | ---: | ---: | ---: |
| Main thread busy | ≈5.4 ms | ≈5.7 ms | ≈1.2 ms (upload 0.65, draw 0.37, backend 0.15) |
| Producer thread | — | — | 4.0 ms/tick mean, 6.0 p99 |
| Presentation (globe underlay) | 2.37 (2.16) | 3.01 (2.70) | 0.35 |
| PPU + capture (scanout) | 1.84 (1.56) | 1.61 (1.34) | on producer |
| Emulation | 0.61 | 0.53 | on producer |
| Town canvas | 0.31 | 0.29 | — |
| SIM HUD restore | 0.24 | 0.24 | — |
| Present interval max | 42.9 ms | 45.3 ms | 18.8 ms (ticks 1825–3000; one 134.6 ms present when Diorama was enabled at gf 900) |

### Cadence

| | Presents | Holds | Pattern |
| --- | ---: | ---: | --- |
| SIM town, idle | 2,693 | 36 (1.34%) | bursts near 5–7, 16–17, 22, 32 s |
| SIM town, moving | 2,691 | 28 (1.04%) | bursts near 6–7, 16, 20–21, 30 s |
| Action, streamed (ticks 1825–3000) | 1,174 | 25 held, 27 skipped (4.34% phase mismatch) | bursts near 4–5, 14–15, 24–25 s |

SIM holds come from the per-window re-present counts. At 60 Hz with a
60.0988 Hz source, a held frame should not occur except after a stall. Each
SIM run also ran 17 more ticks than presents, which includes the missed-vsync
frames below.

### Development-change hitches (SIM town, idle run)

Nine `[sim-worldmap] … development change` events (eight of them one tile)
each preceded a full `[sim-globe-town] source town=4 quads=23339` rebuild:

| Window | Present max | p95 | Presentation peak | SIM upload peak | Events |
| --- | ---: | ---: | ---: | ---: | --- |
| 18 | 42.7 ms | 18.8 ms | 39.6 ms | 24.6 ms | gf 1994, 2001 |
| 20 | 36.6 ms | 33.1 ms | 33.0 ms | 21.4 ms | gf 2110–2126 (4) |
| 39 | 42.9 ms | 33.7 ms | 38.1 ms | 24.5 ms | gf 3266–3297 (3) |

The moving-camera run hit the same events at the same game frames.

### Offline replay of native selection

`replay_selection.py` re-selects ticks against the recorded present returns.
It assumes the selected packet was available, which reproduced the shipped
predictor closely (88 vs 83 errors today; 35 vs 35 on the audit's interactive
run).

| Trace | Recorded | Shipped predictor | Phase-filtered (g = 1/8) |
| --- | ---: | ---: | ---: |
| This session, action | 4.00% | 4.24% | 1.20% |
| 2026-10-03 audit, timeline/interactive | 1.69% | 1.69% | 0.72% |
| 2026-10-03 audit, latest/interactive | 3.67% | 1.65% | 0.44% |
| 2026-10-03 audit, timeline/initiated | 1.79% | 1.02% | 0.53% |
| 2026-10-03 audit, noisy repeat | 8.20% | 5.25% | 4.10% |

The irreducible floor is about 0.16% (one skip per beat). The noisy repeat
stays high because OS stalls dominate it, not selection.

## Method and caveats

- **Machine and display:** M2 MacBook Air (4P+4E), built-in 60 Hz display,
  Metal, windowed at a 2160×1344 output with 16:10 square pixels, VSync,
  one frame in flight.
- **Binary:** `build-release` at HEAD `695d2d14`, SHA-256 `7b30009c…`, pinned
  before any concurrent edits.
- **Machine load:** a parallel Codex session was active and the window
  changed focus during runs, so these are indicative single runs, not ABBA
  batches.
- **SIM workload:** the D7 Aitos voxel-town fixture (`aitos-voxel-seed.srm` and
  `aitos-eruption.rec`), with the town active from gf 907 to 3600. The moving
  variant flies the angel in a d-pad square from gf 1100. Presentation
  settings are the developer's (Quality voxels, connected globe, shadows, rim,
  clouds, haze, CRT, Dynamic Cam) with two exceptions: **Native** text and the
  **Original** menu. Enhanced text re-flows dialogue, which shifts the
  replay's button presses, so the run never returns to Aitos after the gf 787
  interlude. Audio was disabled.
- **Action workload:** the 2026-10-03 audit's Fillmore 01/01 replay (warp at
  gf 500, Diorama at gf 900) with the same presentation settings, streamed
  native playback, interpolation off.
- **Not measured:** Steam Deck, Windows/D3D12, 90/120 Hz, a per-present SIM
  trace (item 1), the sub-step behind the SIM upload spike, and BG1's share of
  scanout. The first one-second window of each run (VSync not yet blocking
  before the window is composited) was excluded.

Evidence, harness and scripts: `runs/sim-pacing-audit-2026-10-03/`
(`run_sim.py`, `summarize.py`, `replay_selection.py`; logs in `town-idle/`,
`town-move/`, and `action-vsync/`, which includes `pacing.csv`).
