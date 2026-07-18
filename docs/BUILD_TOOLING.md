# Cross-platform build tooling

ActRaiserRecomp uses `snesbuild`, a native Go project driver, for regeneration
and CMake orchestration. The command is deliberately separate from `v2regen`:
`v2regen` exposes individual recompiler operations, while `snesbuild` owns the
ordered per-project workflow and host-tool invocation.

## Current commands

When working from source, run the driver from the repository root through Go:

```sh
go -C snesrecomp-go run ./cmd/snesbuild doctor --root .. --rom ar.sfc
go -C snesrecomp-go run ./cmd/snesbuild regen --root .. --rom ar.sfc
go -C snesrecomp-go run ./cmd/snesbuild build --root ..
```

The current inherited stub backlog makes strict `regen` exit nonzero after all
outputs and sidecars are complete. `--allow-stubs` is available for local work
on the existing backlog; new release automation should remain strict.

Build a reusable host binary with:

```sh
go -C snesrecomp-go build -o build/snesbuild ./cmd/snesbuild
```

Because `-C snesrecomp-go` changes the output base, that command creates
`snesrecomp-go/build/snesbuild`. It can then be invoked without Go or Bash:

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

## Dependency boundary

| Operation | Required on the user's machine |
|---|---|
| Run a downloaded `snesbuild doctor` or `regen` | `snesbuild`, this project, and the user's local ROM |
| Build `snesbuild` from source | Go 1.24+ |
| Compile the game today | CMake, a C11 compiler, SDL2 development files, and platform SDK/linker support |
| Run the compiled game | SDL2 runtime plus the user's local ROM |

The Go binary has no third-party Go or runtime dependencies. It uses Go's
standard library for path handling, worker selection, process execution, RTS
census deltas, and cross-platform exit status handling; it does not call
`grep`, `find`, `cp`, `readlink`, `sysctl`, or other Unix utilities.

## Distribution roadmap

The intended end state is a platform bundle that users unpack beside a clean
checkout and their own ROM:

1. Publish signed `snesbuild` binaries for macOS, Linux, and Windows on the
   architectures supported by the game runtime.
2. Add CI smoke tests for each binary's `doctor` and synthetic-ROM
   regeneration path.
3. Bundle or bootstrap a pinned native build stack: CMake, Ninja, a C compiler
   and linker, SDL2 headers/libraries, and the minimum platform SDK material
   legally permitted for that host.
4. Teach `snesbuild doctor` to validate the pinned bundle and `snesbuild all`
   to select it without modifying global `PATH` or package-manager state.
5. Optionally add a small ROM-picker/progress UI around the same Go APIs; the
   CLI remains the automation and CI surface.

Only generic tools and runtime sources should be distributed. The ROM,
generated C, generated manifests, and the resulting ROM-derived game binary
must continue to be produced locally from the user's legally obtained ROM.
