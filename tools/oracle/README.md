# Differential oracle harness

Finds the **first WRAM divergence** between the recompiled build and a
known-good snes9x reference, so bugs are caught at their root instead of at
the downstream symptom.

## Pieces

- `snesref.cpp` — macOS minimal SDL2 libretro frontend, carried forward from
  the historical recompiler oracle. Loads a libretro SNES core and emits
  per-frame WRAM-change JSONL. Build with `./build.sh` → `snesref`.
- `snes9x_libretro.dylib` — the reference core (arm64, from buildbot.libretro.com).
- recomp side — `AR_WRAM_TRACE` in `src/main.c` registers a per-frame
  `g_framedump_callback` emitting the identical JSONL shape.
- `diff_trace.py` — compares the two traces.

## Run

```sh
# 1. Oracle (snes9x) — headless, N frames, no input
SNESREF_HEADLESS=1 SNESREF_QUIT_FRAMES=240 SNESREF_TRACE_FILE=oracle_trace.jsonl \
  ./snesref snes9x_libretro.dylib ../../ar.sfc

# 2. Recomp — headless, same N frames, same input
cd ../..
AR_HEADLESS=1 AR_QUIT_FRAMES=240 AR_WRAM_TRACE=recomp_trace.jsonl \
  ./build/ActRaiserRecomp ar.sfc

# 3. Diff
python3 tools/oracle/diff_trace.py tools/oracle/oracle_trace.jsonl recomp_trace.jsonl
```

To reproduce an input-triggered bug, drive both sides deterministically:
`SNESREF_FORCE_B_AFTER=N` (oracle) and `AR_FORCE_INPUT_AFTER=N` (recomp) both
hold the B button from frame N.

## Env vars

| Oracle (snesref)        | Recomp                | Meaning                          |
|-------------------------|-----------------------|----------------------------------|
| `SNESREF_HEADLESS=1`    | `AR_HEADLESS=1`       | no window, run uncapped          |
| `SNESREF_QUIT_FRAMES=N` | `AR_QUIT_FRAMES=N`    | stop after N frames              |
| `SNESREF_TRACE_FILE=p`  | `AR_WRAM_TRACE=p`     | output JSONL path                |
| `SNESREF_TRACE_LO/HI`   | `AR_TRACE_LO/HI`      | WRAM byte range (default 128 KB) |
| `SNESREF_FORCE_B_AFTER` | `AR_FORCE_INPUT_AFTER`| hold B from frame N              |

## Limitations

- Diffs **WRAM only** ($7E/$7F). snes9x libretro doesn't expose CPU regs /
  CGRAM / VRAM, so register- or palette-only divergence is seen indirectly
  (via the WRAM it eventually corrupts).
- The two emulators don't boot frame-for-frame identically (our coroutine
  batches boot into "frame 1"), and start from different power-on RAM, so
  `diff_trace.py` compares cumulative **written-value** state over commonly
  written addresses rather than raw byte images. Absolute frame numbers align
  only after boot.
