# Portable runner

This directory contains the independently authored portable C runner. It is the
sole runtime used by project, hermetic, and distribution builds.

Current status:

- `runner.cmake` is the sole source and include manifest for source builds.
- The implementation covers hashing, ROM/SRAM and LoROM/HiROM mapping, 65816
  generated-code ABI and dispatch, DMA, the SNES bus/register model, PPU,
  APU/SPC700/S-DSP, MSU-1, frame/audio pacing, save state, diagnostics,
  widescreen and overlay presentation, key bindings, and launcher utilities.
- ABI v2 exposes a versioned capability table, opaque component handles,
  generation counters, bounded frame transactions, and thread-confined memory
  views. Supported headers live under `include/snesrecomp`; hardware layouts
  and singleton state remain private under `src`.
- `cmake -S . -B <build-dir>` from this directory produces the standalone
  `snesrecomp::runtime` static library from independently authored MIT sources
  and runs without SDL, a ROM, or generated game code.

Repository-ready layout:

- `include/snesrecomp/` is the installed public SDK surface;
- `src/core/`, `src/runner/`, `src/snes/`, and `src/support/` are private
  implementation areas;
- `examples/` contains public-header-only integration fixtures;
- `tests/` owns standalone conformance and device tests; and
- [`docs/`](docs/README.md) contains all runner-owned integration and
  engineering documentation.

The parent `snesrecomp-go` module still supplies the recompiler and project
build orchestration. The runner does not rely on the parent's source layout
for its own API boundary, standalone build, tests, provenance, or license.
The smaller source-free distribution layout is documented in
[`BINARY_SDK.md`](BINARY_SDK.md).

## Build, install, and consume

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /path/to/sdk
```

The build artifact is `libsnesrecomp_runtime.a` on Unix-like targets and
`snesrecomp_runtime.lib` on Windows. The install tree contains only the static
library, public headers, CMake package metadata, documentation, provenance,
and the MIT license—not implementation sources or private headers.

An installed consumer uses the exported target:

```cmake
find_package(snesrecomp-runtime CONFIG REQUIRED)
target_link_libraries(MyGame PRIVATE snesrecomp::runtime)
```

The target supplies the public include path and C11 requirement. A prebuilt
archive is specific to an operating system, CPU architecture, object format,
and build configuration. Distribution tooling must select the matching
archive; unsupported targets can build the same target from `runner.cmake`.

Hermetic distributions use this source-free SDK layout:

```text
runtime/
├── include/snesrecomp/...
├── lib/<zig-target>/libsnesrecomp_runtime.a
└── LICENSE, PROVENANCE.md, README.md
```

Windows uses `snesrecomp_runtime.lib`. The driver creates the artifact with
`snesbuild runtime archive --target <zig-target>` and hermetic builds prefer
the exact target-keyed archive automatically. `runner.cmake`, `src/`, private
headers, examples, and tests are development inputs and are not required in a
binary SDK distribution.

Design requirements for every replacement subsystem:

- portable C11 at the core, with fixed-width public ABI types and no compiler,
  operating-system, graphics, or audio-library types in core headers;
- host integration through narrow adapters rather than platform conditionals
  spread through emulation code;
- no allocation, locking, or mutable one-time initialization in hot paths;
- deterministic contract tests that can be compiled by Clang/GCC, MSVC, and
  Zig cross targets; and
- performance measurements before a replacement becomes the release default.

From the ActRaiserRecomp root, the normal build selects this runner:

```sh
cmake --preset play
cmake --build --preset play
```

Supported build targets use their compile-time SIMD implementation by default.
The portable C11 paths remain complete and can be selected explicitly for
portability checks and performance A/B tests:

```sh
cmake --preset play -DSNESRECOMP_ENABLE_SIMD=OFF
cmake --build --preset play
```

The compiler target, rather than the machine running CMake, selects ARM NEON
or x86 SSE2. Cross-compiles therefore cannot accidentally emit instructions
for the build host.

PPU sprite and resolved-pixel bitsets also follow the target's native pointer
width. A 32-bit target uses 32-bit set-bit iteration without changing the
64-bit NEON/SSE2 desktop path. The width can be forced to exercise either
representation through the parity suite on a different host:

```sh
cmake -S . -B <build-dir> \
  -DSNESRECOMP_PPU_BIT_WORD_BITS=32
```

`auto` is the default; the other accepted values are `32` and `64`.

The historical comparison runner was retired after parity validation. The MIT
grant covers this runner and its manifest. It does not cover ROMs, generated
game code, or extracted media; see [`LICENSE`](LICENSE) and
[`PROVENANCE.md`](PROVENANCE.md) for the precise boundary.

The completed component-access and low-copy ABI work, plus explicitly deferred
ideas, is recorded in [`docs/ABI_ROADMAP.md`](docs/ABI_ROADMAP.md). It keeps
game semantics in adapters above the portable runner.

New game projects should start with
[`../docs/PROJECT_INTEGRATION.md`](../docs/PROJECT_INTEGRATION.md) and use the
producer-oriented widescreen/audio workflow in
[`docs/GAME_ENHANCEMENT_INTEGRATION.md`](docs/GAME_ENHANCEMENT_INTEGRATION.md).
The capability matrix, result codes, lifetime rules, and common call sequences
are in [`docs/API_REFERENCE.md`](docs/API_REFERENCE.md). A compile-checked
public-only integration starts in [`examples/minimal_game`](examples/minimal_game).

Measured hotspots and the clean-room scanline optimization sequence are
tracked in [`docs/PERFORMANCE_ROADMAP.md`](docs/PERFORMANCE_ROADMAP.md).
