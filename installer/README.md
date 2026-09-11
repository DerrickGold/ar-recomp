# ActRaiser Recomp Builder

This module owns the ActRaiser-specific installer, Workshop UI, localization
authoring and extraction tools, ROM-derived previews, and release packaging.
The separate `snesrecomp-go/` module owns the reusable recompiler, build driver,
and portable runtime. This module intentionally does not import any
`snesrecomp-go/internal` package; it invokes the `snesbuild` executable through
the versioned JSONL [project integration
contract](../snesrecomp-go/docs/PROJECT_INTEGRATION.md).

## Develop from a checkout

Build and test both modules independently from the repository root:

```sh
go -C installer test ./...
go -C snesrecomp-go test ./...
mkdir -p installer/build snesrecomp-go/build
go -C installer build -o build/actraiser-builder ./cmd/actraiser-builder
go -C snesrecomp-go build -o build/snesbuild ./cmd/snesbuild
```

Prepare ActRaiser's game-owned Native US source before calling the generic
driver directly:

```sh
installer/build/actraiser-builder native-source --root . --rom ar.sfc
snesrecomp-go/build/snesbuild regen --root . --rom ar.sfc --allow-stubs
```

Run the local Builder and Workshop with both freshly built executables:

```sh
installer/build/actraiser-builder gui --root . \
  --snesbuild snesrecomp-go/build/snesbuild --allow-stubs
```

`--snesbuild` accepts an absolute path or a path relative to `--root`. Without
it, the Builder checks for a sibling `snesbuild[.exe]`. In a recognized source
checkout only, it then checks `build/`, `snesrecomp-go/build/`, and
`snesrecomp-go/` below the project root before consulting `PATH`. A packaged
Builder never falls back to an unrelated executable on `PATH`.

## Commands

- `gui` opens the local Builder and Workshop and orchestrates the complete
  project build.
- `native-source` validates the US ROM and prepares the runtime's native
  language source.
- `language` validates, packages, installs, enables, disables, and removes
  `.arlang` packages; run `actraiser-builder language help` for its subcommands.
- `localization-extract` and `localization-graphics` create private regional
  authoring/evidence exports from user-supplied ROMs.
- `audio-preview` renders local ActRaiser soundtrack previews.
- `quintet-lzss` decodes game content for diagnostics.

Run `actraiser-builder help` or `actraiser-builder <command> --help` for command
options. Public language-pack workflows are documented in
[`docs/language-packs.md`](../docs/language-packs.md) and the Workshop UI in
[`docs/builder-workshop.md`](../docs/builder-workshop.md).

## Release packaging

ActRaiser bundle recipes and embedded distribution resources live in
`installer/packaging/`. From the repository root, `make release` builds all
configured targets; the equivalent direct workflow is:

```sh
cd installer/packaging
cmake --workflow --preset release
```

The default release runs seven platform builds and publishes twelve downloads:
direct macOS, Windows and Steam Deck artifacts; portable archive companions for
those five desktop artifacts; and generic Linux `.tar.xz` archives for ARM64 and
x86-64. Each portable companion reuses its direct artifact and adds a
`BuilderData` sidecar without compiling the payload again. macOS can build all
seven targets without a VM. See the
[desktop release prerequisites and qualification limits](desktop-shell/README.md#one-command-releases-from-macos).
`make release DESKTOP=0` explicitly opts into the old archive-only flow. The
normal release does not also build the redundant legacy macOS, Windows or Deck
full-distribution archives.
Make and the CMake orchestrator share `packaging/release.cmake`; successful
publication prunes only the selected target's superseded generated release.

Each bundle places `actraiser-builder[.exe]` and `snesbuild[.exe]` together in
`utils/tools/`. The launcher passes the sibling path explicitly, preserving the
module and executable boundary in installed packages.

## Desktop shell prototype

The [Go desktop shell](desktop-shell/README.md) explores native Builder
artifacts for macOS (`.app`), Linux (`.AppImage`) and Windows (`.exe`) while
retaining the same Workshop frontend and headless helper. It has its own Go
module and shares the existing offline installer payload. Root `make release`
uses these artifacts for macOS, Windows and Steam Deck, retaining the generic
Linux archives; no CI is added. Documented runtime/UX release gates still apply.
