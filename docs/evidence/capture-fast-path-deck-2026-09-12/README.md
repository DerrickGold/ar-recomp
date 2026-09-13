# Capture fast-path Deck evidence — 2026-09-12

See the [audit](../../capture-fast-path-audit.md#native-steam-deck-comparison)
for results and qualifications. This directory archives diagnostic outputs, not
new product code or a portable benchmark API.

- `timing/`: eight serial ABBAABBA logs, memory guards and aggregate results.
- `visual/`: two separate strict-composite validation logs, guards and hashes.
- `visible-unlimited/`: four normal fullscreen ABBA runs with interpolation,
  including host FPS and per-call CPU stage normalization.
- `visible-vsync/`: a separate two-run 90 Hz panel-rate check, same scene and
  interpolation settings. Both builds meet 90 Hz; this pair is not an upload
  speedup experiment.
- `fillmore-unlimited/`: four visible ABBA trials of the recovered Fillmore
  skybox preset, with dynamic camera, 32-row vertical extension, CRT and
  interpolation off. `visible_fillmore_benchmark.py` selects map 01/01 and
  validates 2,000 ticks; the fixture manifest and input hashes identify the
  original private save/settings.
- `fillmore-vsync/`: the same recovered Fillmore preset in a separate visible
  90 Hz Vsync control/candidate pair; both hold approximately 90 FPS.
- `fillmore-interp-unlimited/`, `fillmore-interp-vsync/`: the same four-run
  comparison and two-run panel check with interpolation enabled. Only that
  setting and isolated writable/output paths differ from interpolation off;
  all binary/fixture/harness hashes match.
- `ppu-test.log`: native x86/SSE2 independent-reference PPU test result.
- `control-build.log`, `candidate-build.log`: matched hermetic Linux builds.
- `compare_pipeline_performance.py`: frozen copy of the existing harness with
  private Deck-only timeout, RAM/GPU-memory and competing-process guards, plus
  platform/library fingerprints. Its `/proc`, `/sys` and SDL paths are specific
  to this device. It terminates only its own probe process group on a limit.
- `visible_deck_benchmark.py`: separate visible-run harness, importing the
  unchanged diagnostic guards. Validates 3,000 final emulation ticks, rather
  than incorrectly demanding one tick per presentation with frame generation.

The logs/results reference absolute paths in the original isolated Deck root:
`/home/deck/argame/capture-fast-path-20260912.XW1iYR/`.
Full local build sources, binaries and copied evidence remain at
`/private/tmp/actraiser-deck-capture.ViJHxd/`. Screenshot images and run dumps
remain in the Deck run bundles; their exact hashes are in `visual/results.json`.
No ROM, save dump, shared library or game binary is included in this archive.

## Binary identity and build

| Binary | SHA-256 |
| --- | --- |
| Control, HEAD `7f3f218b` | `a9da71e2647eb852bf01b342be57d0a6d29c42257f3f562cad9fb0df709020c1` |
| Candidate, only the PPU fast-path patch | `b60d989debd14ba9303f332a4f2124ebcb4221ff8b7e76e36e4467606064a314` |
| Native PPU test | `0da97209e8be769d643e23af4722233f809ee07662332bf205f97eca0084d62a` |

Both game binaries use the same archived source snapshot plus copied generated
`src/gen/` and `recomp/funcs.h`. Both already include the hidden-window GPU
retirement fix and finite-skybox fix. Only candidate `ppu.c` differs; the
existing build cache reused every other translation unit. Build command from
the original Mac workspace (cache paths are isolated, not shared user caches):

```sh
env ZIG_GLOBAL_CACHE_DIR=/private/tmp/actraiser-deck-capture.ViJHxd/zig-cache \
  ZIG_LOCAL_CACHE_DIR=/private/tmp/actraiser-deck-capture.ViJHxd/zig-local \
  snesrecomp-go/build/snesbuild build --hermetic \
  --root /private/tmp/actraiser-deck-capture.ViJHxd/source --rom ar.sfc \
  --target x86_64-linux-gnu \
  --build-dir /private/tmp/actraiser-deck-capture.ViJHxd/build \
  --zig /Users/derrick/Documents/Programming/ActRaiserRecomp/build/toolchain/zig-aarch64-macos-0.16.0/zig \
  --sdl-include /private/tmp/actraiser-deck-capture.ViJHxd/sdl3/include \
  --sdl-lib /private/tmp/actraiser-deck-capture.ViJHxd/sdl3/lib \
  --optimize=-O2 --jobs 4 --verbose
```

Runtime source fallback has SIMD enabled, deep instrumentation/recorder off;
this Linux build is not the Mac thin-LTO configuration. The PPU test links the
exact candidate PPU, color-LUT and saveload object files with the expanded
`runtime/tests/ppu_test.c` and existing `runner_event_stub.c`, compiled for the
same Linux target with `-O2 -g -DSNESRECOMP_ENABLE_SIMD=1`.

## Target run

From the isolated Deck root, with its copied assets/config/replay fixtures and
ROM symlink in place:

```sh
env LD_LIBRARY_PATH=/home/deck/argame XDG_RUNTIME_DIR=/run/user/1000 \
  WAYLAND_DISPLAY=wayland-0 python3 tools/compare_pipeline_performance.py \
  --control "$PWD/control" --candidate "$PWD/candidate" \
  --manifest tests/fixtures/benchmark/action-render-checkpoints.json \
  --checkpoint aitos-diorama --scene 'Action 3D' --quit-frames 3000 --timeout 90 \
  --set AR_INTERP_ENABLE=0 --set AR_SHOW_FPS=Off --set AR_AUDIO_VOLUME=0 \
  --set AR_WINDOW_MODE=Windowed --set AR_WINDOW_SCALE=3 --output timing-aitos
```

Run the same command separately with a new output directory and
`--verify-only --capture-from 1400 --capture-to 2400 --capture-every 200` for
pixel validation. Existing output directories are deliberately refused. The
harness makes private writable settings/seed copies and validates pinned input
hashes, final WRAM, exact tick count and logged graphics errors. It does not
change installed user settings. Camera mode is the fixture/default free camera;
CRT and interpolation are off, enhanced action effects on.

The old installed binary was not run or replaced. Its SHA-256 was checked
before and after testing and remained
`a38e659a0fa535983d565f5d4b3ddb28223e018a554cf1d9ec91e8acb97e7ee1`.
It predates the hidden-window retirement fix; do not substitute it as control.
The SDL/library fingerprints and power/display operating point are in each
result JSON's `platform` record. Steam/compositor activity and dynamic clocks
remain uncontrolled; this is a targeted CPU-direction check, not Game Mode FPS.

## Visible fullscreen follow-up

The user subsequently requested a visible FPS test and confirmed seeing the
running game. Unlike the earlier hidden timing command, this uses normal paced
emulation, interpolation enabled, the panel's full 1280×800 output and uncapped
host presentation. It reports repeated/generated presentations separately from
actual PPU calls. The display remained 90 Hz, desktop Wayland, active/unlocked.

```sh
env LD_LIBRARY_PATH=/home/deck/argame XDG_RUNTIME_DIR=/run/user/1000 \
  WAYLAND_DISPLAY=wayland-0 python3 tools/visible_deck_benchmark.py \
  --interpolation 1 --output visible-interp-unlimited
```

For the paired panel-rate check, add
`--refresh Vsync --order control candidate` and use a new output directory.
The normal frame loop may consume catch-up ticks before a present; the harness
therefore checks `dump_state.txt` for exactly 3,000 emulation ticks, identical
WRAM and no rendering/fallback failures. It never uses headless completion
counts as a proxy for visible cadence. Screenshots are not collected during
these FPS runs.

For the subsequent Fillmore comparison, the existing `saves/fillmore-act.rec`
was copied into the isolated root. The original skybox settings and save seed
from `/private/tmp/actraiser-capture-fast-path.rpepqO/` were copied to
`tests/fixtures/benchmark/fillmore-skybox-settings.ini` and
`fillmore-skybox-seed.srm` in that root; the archived manifest references them.
Their full content is retained privately rather than duplicating user save and
settings data in this report. Run:

```sh
env LD_LIBRARY_PATH=/home/deck/argame XDG_RUNTIME_DIR=/run/user/1000 \
  WAYLAND_DISPLAY=wayland-0 python3 tools/visible_fillmore_benchmark.py \
  --interpolation 0 --output visible-fillmore-unlimited
```

The same `--refresh Vsync --order control candidate` options apply to the
Fillmore panel-rate pair, with a fresh output directory.
The interpolation-enabled follow-up uses `--interpolation 1`, with output
directories `visible-fillmore-interp-unlimited` and
`visible-fillmore-interp-vsync` respectively. No new harness or executable was
introduced for the toggle comparison.
