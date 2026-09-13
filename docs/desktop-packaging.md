# Desktop packages and user data

The Builder produces a macOS `.app`, a Linux/Steam Deck `.AppImage`, or a Windows
folder containing the game executable, libraries, and data. Keep the Windows
folder together when moving the game. For downloads and installation, see the
[Quick Start](https://github.com/DerrickGold/ar-recomp#quick-start).

Generated games contain content derived from your ROM and are for your own use.
Do not redistribute them. Language and asset packs have separate sharing rules
in the [Workshop guide](builder-workshop.md).

## Portable and per-user storage

On macOS and Linux, the application and its writable data can live separately.
A portable install keeps data beside the application; an app-only install uses
the operating system's per-user location:

| Platform | Default game data directory |
|---|---|
| macOS | `~/Library/Application Support/ActRaiserRecomp/game` |
| Linux / Steam Deck | `$XDG_DATA_HOME/ActRaiserRecomp/game`, or `~/.local/share/ActRaiserRecomp/game` when unset |
| Windows | The game folder (portable storage) |

The macOS/Linux launcher chooses its data directory in this order:

1. An explicit `--data-dir PATH`, `--portable`, or `--global` option.
2. `AR_USER_DATA_DIR`, if set.
3. A sidecar file named after the application, with `.portable` appended.
4. The per-user directory above.

For example, `ActRaiserRecomp.app.portable` belongs beside
`ActRaiserRecomp.app`. Its contents are a relative directory such as `.` or
`utils`; an empty sidecar means `.`. `--portable` instead uses the caller's
current working directory.

The Builder's portable download uses a `BuilderData` directory. Builder data
and the generated game's saves/settings are separate. The default game output
is an `ActRaiserRecomp` folder beside the Builder.

To move a portable installation, copy the entire folder, including its sidecar
and data. Copying only the app switches it to per-user storage. Switching modes
does not automatically transfer saves or settings; back up and copy the data
you want to keep. Updates preserve existing saves, settings, and edited seed
files. Use the Workshop's explicit import workflow to bring in another project's
content while retaining the original.

## Inspecting paths and troubleshooting

The macOS launcher is `ActRaiserRecomp.app/Contents/MacOS/actraiser-builder`.
On Linux, pass options directly to the `.AppImage` (or `AppRun` in an AppDir):

```sh
./ActRaiserRecomp.AppImage --print-paths
./ActRaiserRecomp.AppImage --global
./ActRaiserRecomp.AppImage --data-dir /absolute/path/to/profile
./ActRaiserRecomp.AppImage --prepare-only
```

`--print-paths` reports the selected locations; `--prepare-only` prepares the
data directory without starting the game. Launcher logs live under `logs/` in
that directory. Startup errors appear in a dialog, or on stderr when no desktop
session is available.

If an AppImage cannot mount because FUSE is unavailable, try:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 ./ActRaiserRecomp.AppImage
```

## Packaging a source build

The Builder handles packaging automatically. If you build from source, first
follow the [source build instructions](https://github.com/DerrickGold/ar-recomp#build-from-source), then
package your local executable and ROM:

```sh
./build-release/actraiser-builder package --root . \
  --binary build-release/ActRaiserRecomp --rom ar.sfc \
  --destination build-desktop
```

This selects the native package format for the host. Add `--portable` to create
a sidecar; `--destination . --portable` uses the checkout's data. Replacing an
existing package requires `--replace`, which saves the previous package as
`ActRaiserRecomp-previous-*`.

For a Linux source build, supply `--appimagetool PATH` and
`--appimage-runtime PATH`, or choose `--format appdir` to leave an unpacked
application directory. The distributed Builder already includes these tools.
Run `actraiser-builder package --help` for all options.

## Building Builder distributions

To build customized Builder downloads from macOS, install Xcode Command Line
Tools, Go 1.25 or newer, and the packaging prerequisites:

```sh
brew install go cmake pkgconf xz zstd squashfs glib shared-mime-info sevenzip
make release
```

Run `make release` from the repository root. It builds the configured platform
downloads without a VM, including native desktop packages and their portable
companions. `make release DESKTOP=0` selects archive-only packaging. See the
[Builder README](https://github.com/DerrickGold/ar-recomp/tree/main/installer) for building and running the CLI locally.

Builder distributions must not include your ROM, generated game, extracted
content, or private project backups.
