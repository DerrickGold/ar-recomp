# Cross-platform build tooling

ActRaiserRecomp uses the Go-based `snesbuild` driver for regeneration and build
orchestration. `v2regen` exposes individual recompiler operations;
`snesbuild` owns the ordered project workflow.

## Current commands

When working from source, run the driver from the repository root through Go:

```sh
go -C snesrecomp-go run ./cmd/snesbuild doctor --root .. --rom ar.sfc
go -C snesrecomp-go run ./cmd/snesbuild regen --root .. --rom ar.sfc
go -C snesrecomp-go run ./cmd/snesbuild build --root ..
```

The inherited stub backlog makes strict `regen` exit nonzero after producing all
outputs. Use `--allow-stubs` for local work; keep release automation strict.

Build a reusable host binary with:

```sh
go -C snesrecomp-go build -o build/snesbuild ./cmd/snesbuild
```

Because `-C snesrecomp-go` changes the output base, this creates
`snesrecomp-go/build/snesbuild`, which can run without Go or Bash:

```sh
snesrecomp-go/build/snesbuild all \
  --root . --rom ar.sfc --allow-stubs
```

On Windows the equivalent downloaded or locally built executable is:

```powershell
snesbuild.exe all --root . --rom ar.sfc --allow-stubs
```

`tools/regen.sh` and `tools/build-macos.sh` are compatibility launchers for
existing developer muscle memory. They prefer `$SNESBUILD` or
`snesrecomp-go/build/snesbuild`, then fall back to `go run`. New automation
should call `snesbuild` directly.

## Hermetic builds (no CMake, no system compiler)

`snesbuild build --hermetic` compiles the runtime, game sources, and generated
banks with a pinned [Zig](https://ziglang.org) toolchain. It does not require
CMake, a system C compiler, or SDL3 development packages. The SDL3 runtime
remains an input and is bundled where redistribution permits it.

```sh
snesbuild toolchain fetch          # one-time: download + sha256-verify + extract
snesbuild build --hermetic --root . --rom ar.sfc
snesbuild all --hermetic --root . --rom ar.sfc --allow-stubs   # regen + build
snesbuild gui --root .         # local graphical ROM picker + hermetic build
snesbuild audio-preview --rom ar.sfc --out build/audio-previews
```

Runner comparisons use the same build entry points:

```sh
cmake --preset dev                         # inherited release runner
cmake --preset dev-next                    # independent portable runner
snesbuild build --hermetic --runner next   # isolated Zig object tree
```

`legacy` remains the default comparison target. `next` contains only the
independently authored MIT runner; its manifest has no legacy source or include
fallbacks. Projects can switch between them without sharing object caches.

Zig 0.16.0 is resolved from `$SNESBUILD_ZIG`, `build/toolchain/`, then `PATH`.
`snesbuild toolchain status` reports the pin and local availability. Fetches are
verified against a checksum embedded in `snesbuild`.

`audio-preview` is the same MIT-licensed, pure-Go audio-only emulator used by
the GUI's Assets tab. It directly interprets the SPC700 driver and renders the
S-DSP/BRR stream to stereo WAV; it does not launch, link, or copy the inherited
C runner. The GUI keeps those ROM-derived WAVs in the operating-system user
cache, outside `game-assets`, the runtime manifest, and release archives.

The macOS/Linux Zig pins are `.tar.xz` archives, unpacked via the host `tar`,
which must support `.xz` (standard on modern macOS and Linux). The Windows Zig
pins are `.zip`, extracted in-process via Go's `archive/zip`, so a Windows host
never depends on `tar` having `.xz` support.

Inputs are split along the same boundary as the redistribution rules:

- the selected `snesrecomp-go/runtime/runner.cmake` or
  `snesrecomp-go/runtime-next/runner.cmake` stays the source of truth for the
  engine's source list (parsed directly, so the CMake and hermetic builds
  cannot drift);
- `snesbuild.ini` at the project root declares the game half: target name,
  game sources, includes, defines, and SDL3 usage. `snesbuild doctor`
  cross-checks it against the game target in `CMakeLists.txt` and warns on
  drift;
- generated banks are globbed from `src/gen` as always.

Legacy output lands in `build/hermetic/`; next output lands in
`build/hermetic-next/`. Both use flat per-source objects and incremental
source/header mtimes plus a flags hash. Use CMake for normal development,
tests, and sanitizers; use the hermetic path for distribution.

### Cross-target link checks

Because `zig cc` includes target headers and linkers, it can compile and link for
another platform:

```sh
make check-cross                   # currently: Windows x86_64
```

which is equivalent to

```sh
snesbuild sdl stage  --root . --target x86_64-windows-gnu
snesbuild build --hermetic --root . --target x86_64-windows-gnu
```

`sdl stage` downloads and sha256-verifies the *same* pinned SDL3
redistributable the platform bundle ships and lays it out as
`build/hermetic/<target>/sdl3/{include,lib}`. A cross build never falls back to
the host's SDL3 — linking macOS SDL into a Windows binary would fail in a way
that teaches nothing — so an unstaged target is an error naming the fix.

Each target keeps its own object tree under `build/hermetic/<target>/`, so a
cross check does not evict the native build's objects (and vice versa); both
stay incremental.

This proves compilation and linking, not runtime behavior. It catches uncompiled
`#ifdef _WIN32` branches, `<windows.h>` macro collisions, and missing system
libraries. The check has already found examples of all three.

Only Windows x86_64 is wired in, because it is the only platform with both an
official SDL3 redistributable to link against and no other way to test it here:
macOS is built natively, and Linux x86_64 is validated on the Steam Deck. Other
triples work by hand with `--target` plus `--sdl-include`/`--sdl-lib`.

## Dependency boundary

| Operation | Required on the user's machine |
|---|---|
| Run a downloaded `snesbuild doctor` or `regen` | `snesbuild`, this project, and the user's local ROM |
| Build `snesbuild` from source | Go 1.24+ |
| Build through a self-contained bundle GUI | The bundle and the user's local ROM |
| Compile through the developer CMake path | CMake, a C11 compiler, SDL3 development files, and platform SDK/linker support |
| Run the compiled game | SDL3 runtime plus the user's local ROM |

The Go binary has no third-party Go or runtime dependencies. It uses Go's
standard library for path handling, worker selection, process execution, RTS
census deltas, and cross-platform exit status handling; it does not call
`grep`, `find`, `cp`, `readlink`, `sysctl`, or other Unix utilities.

## GPU shaders

The SDL GPU renderer and SIM3D's D32 pipeline are baseline requirements;
optional post-effects share that backend. Shader formats remain
backend-specific: SPIR-V for Vulkan, MSL for Metal, and DXIL for D3D12.
**No shader compiler runs during a game build.** Compiled blobs are committed,
so both build paths compile only C.

| Path | What it is |
|---|---|
| `src/shaders/*.{vert,frag}.glsl` | The authored source — the one place a shader is written |
| `src/shaders/*_{vert,frag}.h` | Generated **and committed**: byte arrays holding SPIR-V + MSL + DXIL |
| `tools/build_shaders.py` | Developer-only generator |
| `tests/shader_blob_test.c` | Asserts every blob compiles on the live backend |
| `tests/diorama_frame_generation_test.c` | Exercises frame-generation geometry on both a deterministic headless renderer and the production GPU renderer |
| `tests/sim3d_depth_pass_gpu_test.c` | Builds the production D32 pipelines, submits occluded geometry, and reads the cycled target through SDL's texture wrapper |

```bash
tools/build_shaders.py            # regenerate after editing shader GLSL
```

`--check` verifies the committed headers match their sources without writing,
which is what CI should run. Regenerating needs `glslc` (Homebrew `shaderc`,
Debian `glslc`), `spirv-cross`, and Microsoft's `dxc`; **building the game
needs none of them.** If DXC is not on `PATH`, point the generator at it with
`DXC=/path/to/dxc`. The variable accepts a command prefix such as
`DXC='wine /path/to/dxc.exe'` for non-Windows developer machines.

Because `src/` is installed wholesale into bundles, `src/shaders/` ships to all
platforms with no packaging or leak-gate change.

Two constraints worth knowing before touching a shader:

- The authored GLSL must follow SDL's documented binding convention
  (`SDL_gpu.h`, "Shader Resources"): fragment stage uses descriptor **set 2**
  for sampled textures and **set 3** for uniform buffers, with SDL's render
  pipeline supplying `COLOR0` at location 0 and `TEXCOORD0` at location 1. The
  generator refuses to emit a blob whose Metal bindings drifted off those slots.
- Uniform blocks must mirror their C struct field-for-field. Packing two floats
  into a `vec2` where the C side has separate scalars shifts every later member.
- The generator translates the same readable SPIR-V module to HLSL, verifies
  SDL's `space2` sampler / `space3` uniform convention and `TEXCOORD` stage
  semantics, compiles Shader Model 6.0 DXIL, then asks DXC to parse the final
  container before committing it as bytes.

`main.c` requests the `gpu` renderer **with properties**, declaring SPIR-V,
MSL, and DXIL, so SDL can select Vulkan, Metal, or D3D12 while always receiving
a native shader format. Renderer creation, D32 support, and depth-pipeline
creation are validated at startup; there is no software renderer or
painter-order fallback. The ROM-free GPU integration test requests this exact
renderer configuration and skips only when the host has no usable GPU device;
running it on each platform exercises the backend's shader, resource-cycling,
depth-compare, command-ordering, and SDL_GPUTexture-to-SDL_Texture interop path.
The frame-generation test has a separate production-GPU invocation so geometry
state and blend behavior are also checked by each native renderer backend.

## Self-contained distribution bundles

`snesrecomp-go/packaging/` is a standalone CMake project that produces a
self-contained bundle for each platform. The user runs one script to open a
local ROM picker and build log; no checkout, compiler, or system-wide install is
needed. The CGO-free Go module cross-builds every target from one machine.

Build all seven from the repository root:

```sh
make release
# or, pure CMake, from the packaging dir:
#   cd snesrecomp-go/packaging && cmake --workflow --preset release
# single platform:  make release-macos-arm64   (or the per-platform presets)
# Steam Deck only:  make release-steam-deck
```

Each platform tree extracts a Zig toolchain, then removes it after staging the
bundle. Set `KEEP_BUILD=1` to retain it. The packaging download cache is kept for
faster reruns. `make clean` removes regenerable artifacts but keeps the cache;
`make clean-all` removes the cache too.

Bundles (plus `.sha256` sidecars) land in `release/`, named
`actraiser-recomp-<os>-<arch>.{tar.xz,zip}` (~55–65 MB tar.xz, ~100 MB Windows
zip).

The bundle root contains only the instructions, run script, and `utils/`:

```
actraiser-recomp-<platform>/
├── README.txt              plain-text instructions
├── run-build.command/.bat/.sh   the one thing to run
└── utils/                  hidden: the whole build (ignore it)
    ├── snesbuild.ini, config.ini
    ├── recomp/ src/ third_party/stb/ snesrecomp-go/runtime/   authored source (no generated C)
    ├── game-assets/        manifest template + managed audio/HD assets; manual appears on first launch
    ├── tools/snesbuild     the driver (stripped, git-describe-stamped)
    ├── tools/toolchain/zig-*/   pinned C compiler (Zig 0.16.0)
    ├── tools/sdl3/         bundled SDL3 (macOS, Windows x86_64, Steam Deck)
    └── LICENSE, ATTRIBUTION.md
```

The bundle excludes the ROM, ROM-derived generated C, and user-provided
replacement assets. The embedded manual is written to
`utils/game-assets/manual.pdf` on the first launch unless one already exists.
The builder also embeds the project's optional HD title treatment: the Assets
tab installs or removes it and can copy selected Ogg Vorbis tracks into the live
`game-assets/` tree while updating the manifest. macOS and Windows x86_64 use official SDL
redistributables; Steam Deck uses Valve's Steam Runtime SDL. Generic Linux uses
the system SDL.

The user unpacks the archive and runs `run-build`. It starts a local interface on
`127.0.0.1` behind a per-session URL. The selected ROM is copied locally to
`utils/user-rom.sfc` and never uploaded. Alongside Build and Manual, the Assets
tab manages the bundled title option and all 17 soundtrack images from the ROM
song table. Unidentified entries remain visible by table slot so their
extracted previews can be auditioned and named without encountering them during
gameplay. After the hermetic build, the GUI:

1. Copies the executable and bundled SDL library, where applicable, to the root.
2. Generates a `run-game` script beside it for later launches without a rebuild.

On later launches, the GUI independently detects two capabilities:

- **can launch** — the `run-game` launcher and the game binary are both present in
  the output folder. Nothing about the toolchain matters.
- **can rebuild** — every non-regenerable input is present: `recomp/`,
  `snesbuild.ini`, `snesrecomp-go/runtime/`, `src/`. Deliberately *not* gated on
  the Zig toolchain or `src/gen`, which the build fetches and regenerates —
  gating on those would refuse a rebuild that would have succeeded.

Together they select the `buildable`, `ready`, `launcher`, or `unusable` page.
The server also enforces the rebuild gate so stale browser state cannot start an
invalid build.

After a successful build, **Keep just the game** deletes an allowlist of build
inputs: `utils/{tools,build,src,recomp,snesrecomp-go,third_party}`. It never
deletes all of `utils/`, which is also the runtime working directory for config,
layer overrides, assets, and saves. Cleanup is refused unless a built game is
present.

`snesbuild` finds bundled Zig and SDL beside itself in `utils/tools/`. The game
uses an executable-relative SDL rpath, and `run-game` sets its working directory
to `utils/` for config and assets. This flow is verified from an extracted
bundle outside the repository without the development machine's toolchain.

A post-package **leak gate** re-extracts every archive. It rejects unexpected
top-level entries and build-machine paths in first-party files. Toolchains,
SDL, and third-party payloads are exempt from the path scan.

## Remaining distribution work

1. **Signing/notarization.** macOS Gatekeeper blocks the unsigned
   `run-build.command` (right-click → Open is the current workaround); the
   binaries want signing before public release.
2. **CI** to run `make release` on a clean tagged checkout (today's archives
   carry the `-dirty` stamp) and publish the artifacts + checksums.
3. **Windows/generic Linux runtime validation.** All seven bundles cross-build
   and the full standalone flow is verified end-to-end on macOS. The game and
   input path are now hardware-validated on Steam Deck. Windows now
   cross-compiles *and links* here via `make check-cross` (see above), which
   closes the build question; what remains for Windows is purely runtime —
   SDL3 window/GPU/audio init, the fiber-based coroutine path in
   `actraiser_rtl.c`, and `MoveFileExA` save durability, none of which a link
   can exercise. The generic Linux system-SDL fallback still needs a real-host
   run.
4. **`--allow-stubs` decision.** The one-click flow currently passes it so
   regen always completes; closing the hard-stub backlog would let the shipped
   flow drop it.
Only generic tools and runtime sources should be distributed. The ROM,
generated C, generated manifests, and the resulting ROM-derived game binary
must continue to be produced locally from the user's legally obtained ROM.
