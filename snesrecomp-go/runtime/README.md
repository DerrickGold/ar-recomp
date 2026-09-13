# Portable runner

The portable SNES runner is a standalone library for recompiled games and
native extensions. Its versioned API provides opaque handles, capability
queries, and bounded access to emulated state. Public headers live under
`include/snesrecomp`; headers under `src/` are private.

Building the library requires a C11/C++20 toolchain and CMake, but no SDL,
ROM, or generated game code. See the [SDK guides](docs/README.md) for API usage,
[examples](examples/minimal_game/README.md) for an integration sample, and
[BINARY_SDK.md](BINARY_SDK.md) for the prebuilt distribution layout.

The project-authored sources are MIT licensed. Attributed Snaggletooth S-DSP
portions retain their upstream MIT notice; see [PROVENANCE.md](PROVENANCE.md)
and [NOTICE.md](NOTICE.md).

## Build, install, and consume

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /path/to/sdk
```

The build artifact is `libsnesrecomp_runtime.a` on Unix-like targets and
`snesrecomp_runtime.lib` on Windows. The install tree contains only the static
library, public headers, CMake package metadata, documentation, provenance,
and the applicable MIT licenses—not implementation sources or private headers.

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
└── LICENSE, NOTICE.md, licenses/Snaggletooth-LICENSE.txt, PROVENANCE.md, README.md
```

Windows uses `snesrecomp_runtime.lib`. The driver creates the artifact with
`snesbuild runtime archive --target <zig-target>` and hermetic builds prefer
the exact target-keyed archive automatically. `runner.cmake`, `src/`, private
headers, examples, and tests are development inputs and are not required in a
binary SDK distribution.

From the ActRaiserRecomp root, the normal build selects this runner:

```sh
cmake --preset play
cmake --build --preset play
```

Supported build targets use their compile-time SIMD implementation by default.
Portable SIMD fallbacks remain complete and can be selected explicitly for
portability checks and performance A/B tests:

```sh
cmake --preset play -DSNESRECOMP_ENABLE_SIMD=OFF
cmake --build --preset play
```

The compiler target, rather than the machine running CMake, selects ARM NEON
or x86 SSE2. Cross-compiles therefore cannot accidentally emit instructions
for the build host.

Supported release compilers also use interprocedural optimization by default
for the runtime target. Configure with `SNESRECOMP_ENABLE_IPO=OFF` when
debugging compiler/linker behavior or validating a toolchain without IPO. The
portable Zig-built SDK archive keeps ordinary native objects and instead
compiles the private accuracy core and bridge as one C++20 translation unit.

PPU sprite and resolved-pixel bitsets also follow the target's native pointer
width. A 32-bit target uses 32-bit set-bit iteration without changing the
64-bit NEON/SSE2 desktop path. The width can be forced to exercise either
representation through the parity suite on a different host:

```sh
cmake -S . -B <build-dir> \
  -DSNESRECOMP_PPU_BIT_WORD_BITS=32
```

`auto` is the default; the other accepted values are `32` and `64`.
