# ActRaiser Recomp Builder

The Builder builds your local game and provides a Workshop for replacing art,
music, and text. It also includes command-line tools for language-pack
authoring, ROM extraction, and packaging. For the desktop download, start with
the [Quick Start](../README.md#quick-start).

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

From the repository root, `make release` builds the configured Builder downloads.
See [Building Builder distributions](../docs/desktop-packaging.md#building-builder-distributions)
for prerequisites. The equivalent direct command is:

```sh
cd installer/packaging
cmake --workflow --preset release
```

The downloads include native macOS, Windows, and Steam Deck applications,
portable companions, and generic Linux archives. `make release DESKTOP=0`
selects archive-only packaging.

For storage locations, launcher options, and packaging a locally compiled game,
see [Desktop packages and user data](../docs/desktop-packaging.md).
