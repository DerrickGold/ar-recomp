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
| macOS | `~/Library/Application Support/ActRaiserRecomp/game` |
| Linux | `$XDG_DATA_HOME/ActRaiserRecomp/game`, or `~/.local/share/ActRaiserRecomp/game` |

Relative XDG variables are ignored. Diagnostics currently remain under the same
data root (`runs/` when enabled), preserving the game's existing contract.
Separating disposable diagnostics into the OS cache directory is follow-up work.

The desktop Builder writes a self-contained playable `ActRaiserRecomp/` child
beside its outer application, with a `.` sidecar selecting that output folder.
Its Workshop edits the output's runtime assets directly; build inputs remain
in the separate installer workspace. Legacy archive launchers still write a
sidecar pointing to the existing project data (`utils/` in downloaded bundles),
so those installs retain their existing saves/settings.

Public desktop Builder releases offer both storage choices without duplicating
the build. The direct macOS app ZIP, Windows executable, and Steam Deck AppImage
omit a Builder sidecar and use per-user storage. Their recommended portable
companions wrap that same already-built artifact plus a matching sidecar whose
contents are `BuilderData`; first launch creates that sibling directory. The
macOS/Windows wrappers are ZIP files. The AppImage wrapper is tar.xz so its
executable mode survives extraction. These Builder markers do not alter the
generated game's independent portable/global selection.

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
automatic migration or deletion of another portable installation's data.

Older global installs used `ActRaiserRecomp` itself. On first launch using the
new `game/` child, a recognized old global seed history triggers a one-time,
non-destructive import of saves, settings, defaults and runtime assets. Existing
files in `game/` win, original files remain in place, and the import is not
repeated after initialization. Builder data under `installer/` is never imported.
The desktop Builder uses the same OS application-data parent, with its own
`installer/workspace` child (and Windows runtime/profile children). See the
[Builder storage and output contract](../installer/desktop-shell/README.md).

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
while packaging. A skipped Linux acceptance test is not evidence that a
finished AppImage was created or launched.

### Local Linux AppImage acceptance (no ROM)

Run this maintainer check on a Linux amd64 or arm64 machine, including a VM:

```sh
make check-appimage
```

This is a local check, not a CI workflow and not a new installation step for
players. Players still use the portable Builder's existing build button. Its
normal build pipeline passes the compiled game to the same `desktop.Package`
implementation tested here and writes the portable sidecar automatically.
The installer carries AppImage tooling in `utils/tools`; players do not need
Go, CMake, appimagetool, a system SDL development package, or a packaging-time
download. Every installer now includes its platform's pinned SDL SDK. Linux
still needs compatible OS desktop/audio/font runtime libraries.

The maintainer check requires Go 1.24+, CMake 3.21+, Make, a C compiler,
pkg-config, desktop-file-validate, and SDL3/SDL3_ttf development packages. For
example, Debian 13 supplies the latter packages as `libsdl3-dev` and
`libsdl3-ttf-dev`, and the desktop validator in `desktop-file-utils`. These are
test-harness requirements, not additional player dependencies. The probe tests
packaging and font loading, not compilation of the full game against that
distribution's SDL version.

The check reuses `installer/packaging/appimage.cmake` to fetch, checksum-verify,
and stage exactly the tools shipped by the release installer. Cached downloads
are reused. It builds the real Go launcher and a synthetic C game with a
two-level shared-library dependency plus SDL3 and SDL3_ttf, then:

- Builds finished AppImages using tools beside the Builder, without explicit
  tool overrides, and exercises the replacement/backup path.
- Moves/renames the image and removes all original compiler inputs, packaging
  tools, and private test libraries before launching it.
- Runs through the actual AppImage runtime without FUSE, using both the runtime
  environment switch and `--appimage-extract-and-run`.
- Verifies global storage without a marker, existing portable data with one,
  explicit overrides, and caller-relative paths after runtime extraction.
- Opens a real redistributable font and checks the running process's library
  paths: non-glibc libraries must come from the package, not the test host.
- Checks seed upgrades preserve saves, settings, edited/deleted assets, and
  add/update only application-owned content.
- Validates the desktop entry, icon, private-input exclusions, and read-only
  extracted AppDir; rejects data inside the package and checks package hashes
  remain unchanged after launch.

An explicit invocation fails on macOS, unsupported architectures, missing
dependencies, or missing tools; it never reports a skipped suite as acceptance.
The default suite uses extraction and does not require FUSE. To additionally
require native mounting, run `AR_APPIMAGE_TEST_FUSE=1 make check-appimage` on a
host with working FUSE. This checks the ROM's filesystem type inside the running
probe, not just the process exit code. Missing FUSE fails that opt-in check.
Real desktop video/audio/input, Steam Deck, x86_64 execution, and older-distro
compatibility still require target-machine acceptance.

### Bundled Linux SDL SDKs

Linux installers include matched SDL3/SDL3_ttf development and runtime
packages. Generic x86_64 and Steam Deck share Valve's Sniper SDK; ARM64 uses
Valve's Steam Runtime 4 core SDL and Debian 13 SDL_ttf. macOS and Windows use
official SDL redistributables. All are selected by the stable-3.x policy below.

`installer/packaging/sdl-linux.cmake` stages those exact development/runtime
package pairs through the existing release presets. It verifies each SHA-256,
checks the ELF architecture without executing target code, and copies only
public headers, link/runtime shared libraries and publisher notices. It does
not install Debian packages on the packaging host or player's OS. The SDK
is assembled on macOS as well as Linux; no Linux compiler, VM or CI is needed
to create the installer archives. No SDL download is needed at player build time.

The build driver discovers `utils/tools/sdl3` before system SDL and copies the
selected runtime beside the game. Native AppImage packaging prioritizes those
private libraries during dependency discovery, even if the caller's
`LD_LIBRARY_PATH` names another SDL. Missing SDK files fail the archive gate
and the Linux launcher; missing OS loader dependencies fail before regeneration.
Source-checkout builds can still intentionally use system SDKs.

The Linux binaries are publisher builds, not universal Linux binaries. Current
generic x86_64 SDL uses Valve's Sniper build (glibc 2.29+); ARM64 uses Valve's
Steam Runtime 4 build (glibc 2.38+). SDL_ttf uses Valve's package on x86_64 and
Debian 13's package on ARM64. Both require the usual OS X11/Wayland, audio and
font runtime libraries. The generated README lists Debian runtime packages for
minimal installations; it no longer asks players to install SDL development
packages. These runtime requirements and actual graphical-session compatibility
must not be confused with the independently selected SDL API version. The
generated README records the selected packages' actual dependency requirements;
these can change in future publisher builds.

Current selections were checked against publisher metadata on 2026-09-10:
[Valve Sniper amd64 Packages](https://repo.steampowered.com/steamrt-sniper/dists/sniper/main/binary-amd64/Packages.gz),
[Valve Steam Runtime 4 ARM64 Packages](https://repo.steampowered.com/steamrt4/apt/dists/steamrt4/main/binary-arm64/Packages.gz),
[Debian 13 ARM64 Packages](https://deb.debian.org/debian/dists/trixie/main/binary-arm64/Packages.xz).
Repository pool files may eventually be retired. A fresh resolution selects
the currently available matching pair; a locked build fails if its exact
archive is unavailable and not cached. Neither falls back to system SDL.

### SDL version policy

The installer packager defaults to `SNESBUILD_SDL3_VERSION=3` and
`SNESBUILD_SDL3_TTF_VERSION=3`. Every configure selects the newest supported
stable 3.x SDK in the target publisher's release catalog. Linux selects the
newest **available matching dev/runtime package pair** in the repositories
above, which may lag upstream releases. SDL3 must be at least 3.4.0 for the
game's GPU API; SDL3_ttf must be at least 3.2.2.

This follows [SDL's version policy](https://wiki.libsdl.org/SDL3/README-versions):
newer stable 3.x releases are backward-compatible. Both minor and patch must
be even; odd-numbered development versions, release candidates, GitHub
prereleases/drafts, SDL2 and SDL4 are excluded. Updating SDL does not change
the separate, deliberately pinned Zig compiler or AppImage tools.

No minor-version edit is needed for normal releases. To select an exact pair:

```sh
cmake -S installer/packaging -B build/dist-linux-arm64 \
  -DSNESBUILD_GOOS=linux -DSNESBUILD_GOARCH=arm64 \
  -DSNESBUILD_SDL3_VERSION=3.4.14 -DSNESBUILD_SDL3_TTF_VERSION=3.2.2
cmake --build build/dist-linux-arm64
cpack --config build/dist-linux-arm64/CPackConfig.cmake
```

The same two options work on the all-platform orchestrator and are forwarded
to every target. Set both back to `3` to resume automatic updates; existing
CMake caches retain explicit overrides. The defaults live together near the
top of `installer/packaging/CMakeLists.txt`.

Resolution writes `sdl-sdk.lock.json` in the build directory and includes it
under both `utils/tools/sdl3` and `utils/licenses` in the installer. It records
exact versions, URLs, SHA-256 checksums and Linux runtime requirements. To
reproduce a previous selection, add
`-DSNESBUILD_SDL_LOCKFILE=/absolute/path/to/sdl-sdk.lock.json` to the matching
single-platform configure command. This bypasses metadata lookups; cached
archives are reused only if their checksums match. Missing archives still
need downloading. A lock for another OS/architecture or a conflicting exact
version fails. Unset the option to return to live resolution.

Inspect a selection without making an installer:

```sh
go -C snesrecomp-go run ./cmd/snesbuild sdl resolve --goos linux --goarch arm64
```

The resolver uses publisher metadata over HTTPS and verifies archive SHA-256
values. For older official releases predating GitHub asset digests, only an
existing reviewed checksum for that exact artifact is accepted; an unknown
checksum fails closed. Legacy `toolchain pin --sdl*` commands remain the fixed
developer cross-build inputs, not the automatic installer selection.
Changing the selected SDK invalidates cached staging so old headers or
libraries cannot survive an upgrade. Run packaging/target acceptance and
review dependency notices for each release; API compatibility does not promise
unchanged OS requirements or eliminate upstream regressions. This adds no CI.

All selection and downloads happen while **creating the installer archive**.
Players' local Builder continues to compile/package offline using its bundled
SDK; it does not need Go, CMake, a system SDL SDK, or an update service.

Validation on 2026-09-10: resolver unit tests and the real CMake offline-lock /
whole-SDK cache-invalidation test pass. Actual SDK staging passed for Linux
ARM64/x86_64, Steam Deck, macOS ARM64 (universal frameworks), and Windows
ARM64/x86_64. Official macOS/Windows selection was SDL3 3.4.16; the Linux
repositories selected 3.4.14; all selected SDL3_ttf 3.2.2. A Linux ARM64 installer
was rebuilt and passed CPack's leak/dependency gate (613 first-party files).
These are packaging checks, not additional graphical gameplay tests. The
Linux SDK is the same version exercised in the offline player VM below.

### Headless VirtualBox acceptance (2026-09-10)

VirtualBox 7.2.16 on an Apple Silicon Mac successfully booted the official
Debian 13 generic ARM64 cloud image headlessly: two CPUs, 4 GiB RAM, and a
24 GiB dynamically allocated disk. A `CIDATA` ISO supplied cloud-init's
`user-data`/`meta-data`, an isolated test account, and a task-specific SSH key.
SSH used NAT forwarding bound only to `127.0.0.1`; no shared folders, clipboard,
Guest Additions, or changes to existing VMs were required. ARM hosts only
provide ARM guest coverage, not x86_64/Steam Deck execution.

The VM image's SHA-512 was checked against Debian's published checksum before
booting. The selected `debian-13-generic-arm64.tar.xz` (2026-08-31) checksum was:

```text
d741ee4bdf78427446f47d330b283cc18cb04f5ee6ec003a15f6405c740f41815d9e55592b86c9e88e832d709228a6c6ea6764c1384f8f7987d487ff30dec513
```

The checks deliberately separated two environments:

1. **Player environment:** only the distro SDL3/SDL3_ttf development packages,
   with no system Go, CMake, or C compiler. A fresh release installer was
   generated through the production packaging recipe and its real GUI build
   endpoint was invoked with a private local ROM. An isolated network namespace
   allowed loopback HTTP but no external downloads. This exposed an unnecessary
   Zig download: the Builder called `toolchain fetch` even though `toolchain
   status` could locate its bundled compiler. The Builder now tries discovery
   first; regression tests cover the bundled, missing, and broken-override cases.
   A rerun reached compilation using bundled Zig without networking. The full
   game then failed against Debian's SDL 3.2.10 headers: its GPU renderer requires
   newer APIs. That failure motivated the bundled Linux SDK described above;
   renderer features were not disabled to bypass the error. The compiler-free
   environment was saved as a VM snapshot before adding developer tools.
2. **Maintainer environment:** added Go 1.24.4, CMake, a C compiler, and the
   desktop-entry validator, then ran the ROM-free acceptance suite with the
   installer's exact pinned AppImage tools and external networking disabled.
   All extraction, storage, relocation, private-library, upgrade, and immutable
   payload checks passed. Installing Debian's standard `fuse3` runtime also made
   the opt-in native-mount check pass. Without `fusermount`, the extraction
   fallback passed but native mounting failed, as expected.
3. **Bundled-SDK player acceptance:** restored the compiler-free snapshot, then
   removed all four system SDL3/SDL3_ttf development/runtime packages without
   removing their OS dependencies. The new installer correctly rejected missing
   `libXtst.so.6` and `libfribidi.so.0` before regeneration. Added only Debian's
   `libxtst6` and `libfribidi0` runtime packages, not any SDL package or compiler.
   With external networking disabled, the real GUI build action selected the
   bundled SDL3 3.4.14/SDL3_ttf 3.2.2 SDK, compiled and linked the game, produced
   its AppImage and `utils` portable marker, initialized data and ran the game
   headlessly for 60 frames. Copying/renaming that finished AppImage without its
   marker also ran in a fresh global profile; loader tracing confirmed both SDL
   libraries came from the extracted AppImage, with system SDL still absent.
   The fixture library-precedence/relocation regression
   also passed on Linux using the shipped Zig as its test compiler.

   The 4 GiB/no-swap VM exhausted memory with two concurrent generated-bank
   compilations. The successful rerun used `--jobs 1`; no renderer features or
   optimization settings were disabled. Memory-aware default build parallelism
   is a separate existing limitation, not resolved by SDL bundling. This is
   headless ARM64 acceptance, not graphical/input/audio or x86_64/Deck execution.

VM disks, seed files, SSH keys, bootstrap/test scripts, and raw logs stay in
ignored `build-appimage-vm/` storage, not public releases. The real ROM remained
on the Mac and its local VM. No CI workflow was added.

Setup references: [VirtualBox ARM limitations](https://download.virtualbox.org/virtualbox/7.2.10/UserManual.pdf),
[Debian cloud images](https://cloud.debian.org/images/cloud/trixie/latest/),
[cloud-init NoCloud bootstrap disks](https://docs.cloud-init.io/en/latest/reference/datasources/nocloud.html).

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
- The new AppImage acceptance test binary and C/SDL probe cross-compile for
  Linux amd64 and arm64. Explicit invocation correctly fails on a macOS host;
  native Linux ARM64 acceptance subsequently passed in the VM described above.
- Inspection of the pinned x86_64 appimagetool image confirms it contains
  `mksquashfs` and `desktop-file-validate`, with its launcher preferring those
  bundled tools. They are not new dependencies for players to install.

References: [Apple bundle contents](https://developer.apple.com/documentation/bundleresources/placing-content-in-a-bundle),
[AppDir specification](https://docs.appimage.org/reference/appdir.html),
[AppImage environment](https://docs.appimage.org/packaging-guide/environment-variables.html),
[appimagetool](https://github.com/AppImage/appimagetool).
