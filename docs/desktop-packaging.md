# Desktop application packaging

## Audit and revised implementation

The earlier work is `portable-user-data-paths`, commit `bd94d510` (2026-09-07).
Implementation continues from current main on `portable-app-artifacts`; the old
branch is an audit reference.

Findings from the WIP:

- `BuildMacOSApp` had no CLI or build-flow caller; Linux assembly was absent.
- Writable-directory detection cannot distinguish a portable install from a
  signed, user-owned `.app`. Its write probe could modify signed content.
- Falling back to an arbitrary launch directory on storage errors could make
  saves appear lost. Storage selection must fail visibly instead.
- The proposed app omitted runtime fonts, native language source, manual,
  replacement media, and the archive helper. Relocating it was insufficient.
- New font/pack host injection in main supersedes the WIP's localization edits.
- The app builder deleted the old artifact before validating all inputs and
  treated signing failures as success. Its library copy only covered SDL names.
- ActRaiser artifact assembly now belongs in `installer/`, following the
  installer/engine split. The generic driver still supplies the built binary.

## Storage contract

The native launcher owns storage selection and initializes the data tree before
starting the game. It passes an absolute ROM and config path and runs the game
with the data directory as its working directory. Existing relative settings,
saves, assets, archive caches, and diagnostic paths therefore agree, including
paths used by older code. The game executable and libraries remain immutable.

Selection order: explicit `--data-dir PATH`, `--portable` (the caller's current
directory), or `--global`; then `AR_USER_DATA_DIR`; then a sidecar named exactly
`<artifact>.portable`; then global storage. Conflicting command-line selections
are errors. A portable sidecar contains a relative directory such as `.` or
`utils`, resolved from the artifact's parent. It must stay within that parent.
An empty sidecar means `.`. Global storage is:

| OS | Data directory |
| --- | --- |
| macOS | `~/Library/Application Support/ActRaiserRecomp` |
| Linux | `$XDG_DATA_HOME/ActRaiserRecomp`, or `~/.local/share/ActRaiserRecomp` |

Relative XDG variables are ignored. Diagnostics currently remain under the same
data root (`runs/` when enabled), preserving the game's existing contract.
Separating disposable diagnostics into the OS cache directory is follow-up work.

The Builder's portable output writes a sidecar pointing to the existing project
data (`utils/` in downloaded bundles), so existing saves/settings stay in use.
Copy the app together with its sidecar and data to move a portable installation.
Copying just the app uses global storage. `--global` explicitly ignores a sidecar.
For ordinary launches without overrides, absence of a sidecar intentionally
selects non-portable storage. Nearby legacy folders are not auto-detected or
imported. This keeps copying only the application an explicit, predictable way
to select a per-user installation.

Runtime seeds contain defaults and playable assets only. They exclude saves,
settings, private author projects, build products, and language caches. First
launch copies missing seed files. Later launches update only untouched seeded
files; edited and deliberately deleted files remain user-owned. Shipped
`defaults/` always follows the current application and the existing INI upgrade
merges it into live settings. Initialization errors stop launch. There is no
automatic migration or deletion of another installation's data.

## Artifact contract

Applications are generated locally after the user supplies their ROM. These
private outputs contain that ROM and generated game code; the public release
continues to distribute the Builder and authored inputs without either.

macOS uses `Contents/MacOS` for the game and Builder/launcher,
`Contents/Frameworks` for bundled libraries, and `Contents/Resources` for ROM,
seed content, and notices. Dependencies are relocated, nested code is signed
before the bundle, and verification must succeed before publishing the output.
Ad-hoc signing supports local builds; Developer ID signing and notarization of
public installer artifacts remain a release concern.
Load-command relocation is implemented in Go: players do not need the
`install_name_tool` Xcode shim or Command Line Tools. Only the OS-provided
`/usr/bin/codesign` is used when generating the app. Thin and universal Mach-O
files are supported, including weak and re-exported library dependencies.

Linux uses an AppDir with AppRun, desktop entry, icon, `usr/bin`, `usr/lib`, and
`usr/share/ActRaiserRecomp`. AppImage generation uses appimagetool with an explicit
runtime file for offline builds. The native Linux packaging pass checks shared
dependencies; glibc and the platform loader remain supplied by the host. Test on
the oldest supported distribution and on Steam Deck before claiming portability.
Legacy ELF `DT_RPATH` is rejected because it can override the packaged library
search path; relink with `--enable-new-dtags` to use `DT_RUNPATH`. Libraries
loaded dynamically by GPU/audio drivers still depend on the target system.

Assembly uses a staging directory. Failed builds retain the previous output;
replacement is allowed only for an identified ActRaiser artifact. macOS and Linux
share the launcher, storage, and seed policy.

## Usage and verification

The GUI build pipeline creates the native artifact plus a portable sidecar.
`gui --app-format folder` retains the loose-executable-only workflow;
`gui --app-format appdir` builds a Linux directory without AppImage tooling.
The compatibility executable/scripts remain alongside the native app for now,
including a directly invokable game for the Workshop's font probes. The
Workshop still edits its chosen project root, not an unrelated global profile.

After `make dev`, package an existing source build without recompiling:

```sh
./build-release/actraiser-builder package --root . \
  --binary build-release/ActRaiserRecomp --rom ar.sfc \
  --destination build-desktop
```

The command defaults to the host's native format. It does not select portable
storage unless `--portable` is supplied (then `--root` must be within the
destination). Use `--destination . --portable` for the checkout's data. Existing
artifacts require `--replace`, which retains the old app in a uniquely named
`ActRaiserRecomp-previous-*` directory before publishing the verified new one.
These local apps include the user's ROM; never upload them as public releases.

Linux release installers carry checksum-pinned `appimagetool` and an explicit
type-2 runtime alongside `utils/tools/actraiser-builder`. Source builds must
supply `--appimagetool PATH --appimage-runtime PATH`, or use `--format appdir`.
No network access is required during the player's AppImage assembly. A system
without working FUSE can use `APPIMAGE_EXTRACT_AND_RUN=1` to run an AppImage.

For a Mac app, invoke `ActRaiserRecomp.app/Contents/MacOS/actraiser-builder`;
for Linux, invoke the AppImage or `ActRaiserRecomp.AppDir/AppRun`. Both accept:

```text
--print-paths                 display resolved paths without creating data
--global                     use OS application data, ignoring the sidecar
--portable                   use the caller's current directory
--data-dir /absolute/profile  select a particular profile
--prepare-only               initialize the profile without starting the game
```

Launch output is captured under the data root's `logs/`; macOS presents startup
errors in a dialog and Linux uses zenity/kdialog if available. Headless runs
(`AR_HEADLESS=1`) report errors on stderr without opening dialogs. Initialization
uses a process lock released even on a crash, atomic files, and baseline hashes
to preserve edited assets and existing saves/settings.

Run `go -C installer test ./...`. The desktop tests include a native fixture
with transitive shared libraries: assemble, relocate, remove build inputs, and
execute the packaged code. The Mac fixture also disables developer-tool lookup
while packaging. Linux execution, AppImage mount/extract launch, Steam Deck,
and older OS compatibility require native acceptance before release.

### Initial acceptance (2026-09-10)

- Full installer Go suite and desktop/Builder race checks pass.
- Linux amd64/arm64 and Windows amd64 Builder cross-builds pass (not execution).
- The Linux x86_64 AppImage toolchain downloads match the pinned checksums and
  stage with their license notices; package-gate tests reject missing tools.
- A real macOS arm64 game app assembled without developer-tool lookup, bundled
  nine non-system libraries, and passed strict recursive signature verification.
- Headless and Launch Services/Cocoa game launches passed with separate test
  profiles. Signing remained valid after launch. No global player data was
  created or migrated during these tests.
- Native relocation tests passed after removing their original build libraries;
  thin/universal Mach-O edits have structural regression coverage.

References: [Apple bundle contents](https://developer.apple.com/documentation/bundleresources/placing-content-in-a-bundle),
[AppDir specification](https://docs.appimage.org/reference/appdir.html),
[AppImage environment](https://docs.appimage.org/packaging-guide/environment-variables.html),
[appimagetool](https://github.com/AppImage/appimagetool).
