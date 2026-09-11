# Desktop Builder and cross-host releases

`make release` produces desktop Builder artifacts for macOS, Windows and Steam
Deck, plus browser-based generic Linux archives. The desktop shell still has
**runtime/UX release gates**, not a claim of qualification on every target:

- macOS: `ActRaiserRecompBuilder.app`
- Linux: `ActRaiserRecompBuilder.AppImage`
- Windows: `ActRaiserRecompBuilder.exe`

Players should need only the artifact and their own ROM, not Go, Node, CMake,
compiler installations, system SDL development packages, or a terminal. The
Builder carries the existing offline installer payload. No CI is introduced.

## Recommendation and boundaries

Use **Wails v2.15.0** as a thin native webview host around the existing Workshop
HTTP frontend. Wails v3 is still beta; avoid a framework migration in the first
desktop packaging milestone. This nested module requires Go 1.25 or newer for
maintainer builds. The existing headless installer remains a separate Go module
and executable, without a Wails dependency. The game's embedded helper must not
acquire a webview dependency.

The shell launches the exact bundled `actraiser-builder gui --no-open` with an
explicit bundled `snesbuild` path. A new `--ready-file` handshake publishes the
private loopback URL after the server starts. Wails initially serves a small
startup page, then navigates the native webview to a second private loopback
HTTP listener owned by the shell. That listener proxies relative asset, JSON,
multipart and media requests to the backend's authenticated session. Both
listeners bind only to `127.0.0.1` and use independent unguessable URL prefixes.
There are no frontend Go bindings or arbitrary URL proxies.

Real HTTP is intentional: the Linux renderer could load the Workshop through
Wails' custom URL scheme, but could not decode even a generated PCM WAV through
that transport. The same test passes over HTTP. The native startup page must
redirect on warm starts too, not accidentally serve the Workshop on that scheme.

Keep future level/background editors in the Workshop's feature modules, with
their own API endpoints, validation and workspace-owned files. Build jobs and
progress belong to the existing backend. The shell should own only the window,
workspace selection, OS integration, child lifecycle and native dialogs. A
general plugin system or a frontend framework rewrite is not needed for this.

## Payload and workspace contract

The clean CMake install tree is bundled under `Contents/Resources/payload` on
macOS. Linux placement is `usr/share/ActRaiserRecompBuilder/payload` in
the AppDir. Windows appends a compressed installer payload and Fixed Version
WebView2 runtime to an ordinary GUI-subsystem PE executable. The bootstrap
checks and extracts that archive before starting Wails; no runtime download
or system WebView2 installation is required by this packaging path.

First launch copies checksum-verified payload files through a temporary sibling
directory into a dedicated writable workspace. Nothing builds inside a signed
`.app` or mounted AppImage. Absolute/escaping paths, symlink inputs, known
mutable inputs, ROM files and checksum mismatches are rejected. This manifest
is an integrity check, not a substitute for publisher authentication. Always
stage from CMake, never from a player's live install. macOS `._*` transfer
metadata and `.DS_Store` are rejected too; when transferring a clean staging
tree to Linux, use `COPYFILE_DISABLE=1 tar --no-xattrs ...` on the macOS host.

Workspace selection is independent of the process working directory:

- A sidecar named `<artifact>.portable` contains a dedicated relative directory,
  for example `BuilderData`. Keep the artifact, sidecar and that directory
  together when moving a portable Builder.
- Without the sidecar, all platforms share the `ActRaiserRecomp` application
  namespace. Builder files live under `installer/`: macOS uses
  `~/Library/Application Support/ActRaiserRecomp/installer/workspace`, Linux uses
  `$XDG_DATA_HOME/ActRaiserRecomp/installer/workspace` (defaulting to
  `~/.local/share/ActRaiserRecomp/installer/workspace`), and Windows uses
  `%LOCALAPPDATA%\ActRaiserRecomp\installer\workspace`. Windows's extracted
  runtime and browser profile are siblings of `workspace`, inside `installer`.
- `--workspace /absolute/new/directory` is a developer override.
- `--jobs 1` limits compilation to one worker on low-memory machines; the 4 GiB
  Linux test VM needs this. Automatic memory-aware sizing remains future work.

Game output is **independent of Builder workspace mode**. The desktop Builder
creates an `ActRaiserRecomp/` child beside the outer `.app`, `.AppImage`, or
`.exe`, not inside its private workspace or application resources. Finder's
working directory and an AppImage's temporary mount directory are not used.
`--output-dir /absolute/folder` overrides this destination. A read-only location
reports an error; move the Builder to a writable folder or specify an output.
Translocated macOS apps require relocating the app with Finder and relaunching,
or an explicit output directory; temporary translocation paths are not output.

The generated game is portable by default: its `.portable` sidecar contains
`.` and its saves, settings, assets, ROM and archive helper remain inside that
output folder. Workshop asset/language edits target this playable data directly;
compiler inputs and intermediates stay in the Builder workspace. Move the whole
output folder to move the game without the Builder. macOS/Linux users can copy
only the game application without its sidecar to use global `ActRaiserRecomp/game`
data instead. Windows currently generates a portable `.exe` **folder**, not a
self-contained game application with the macOS/Linux global-launcher contract.

Old `ActRaiserRecompBuilder` prototype workspaces are not deleted or silently
upgraded. Keep a backup of their `utils` runtime assets, author projects and
settings; those can be transferred to the new output folder. `--workspace`
can still select an existing workspace when its payload identity matches.
Legacy `run-build` archive launchers keep their existing `utils/` data contract.

Build logs show each compilation start/completion, unit counts and elapsed time,
plus a status line every ten seconds during long compilation/linking phases.
The UI retains a bounded tail; the full log is retained under the workspace's
`logs/` directory. Compiler diagnostic blocks remain grouped by source file.

Unlike the existing game's sidecar, the prototype Builder sidecar does not
accept `.` or an empty value: it must own a separate tools/workspace directory.
It never silently adopts a legacy install. The generated game retains its
existing portable/global rules; Builder storage and game storage are separate
choices. Browser-engine caches can also exist outside the build workspace.
Linux's WebKit profile uses the distinct `ActRaiserRecompBuilderWebView` name:
using the Builder workspace name caused WebKit to pre-create an unmanaged
directory and block global workspace preparation. `framework_linux.go` sets
GLib's name before Wails creates WebKit's default context; the Wails option
alone is applied too late. Fully portable webview-profile
handling remains a release gate; the sidecar currently relocates build/project
data, not every browser-engine file.

Repeated launches of the **same payload** preserve workspace edits. Concurrent
sessions are rejected with a workspace lock. A different payload version or an
unmanaged directory is refused, not overwritten. Conflict-aware upgrades,
repair after cleanup, workspace selection/migration and editor unsaved-state
integration remain release gates. First-run
copying is not yet cancellable mid-file.

### Resetting a test workspace

There are two distinct startup checks:

- **Another Builder is using this workspace**: an OS file lock on
  `.builder-session.lock` is held by a running Builder. Quit that process first.
  The OS releases the lock when the process exits, including after a crash;
  the mere presence of the file is not a stale lock. Do not delete it while a
  Builder is running, since that can bypass mutual exclusion.
- **Workspace belongs to another payload or is not managed**: the directory
  exists, but `.builder-payload` is missing or does not match the current
  embedded payload. This also rejects an empty directory you created manually.

Close the Builder, then rename the exact workspace directory as a backup and
relaunch. Leave the original workspace path absent so the Builder can initialize
it. Do not just delete its stamp or contents. On Steam Deck, the default is
`~/.local/share/ActRaiserRecomp/installer/workspace`, unless `XDG_DATA_HOME`, a
Builder `.portable` sidecar or `--workspace` selects another location. Deleting
the adjacent `ActRaiserRecomp/` game output does not reset this workspace.

Alternatively, launch with `--workspace /absolute/new/workspace` for testing.
This override applies only to that launch. Keep the playable game output and
its saves intact; it is separate from the compiler workspace. A changed payload
currently needs this fresh-workspace procedure even if only the Builder changed.

## Build the macOS prototype

On a native macOS maintainer host with Go, CMake and Xcode Command Line Tools,
first configure either macOS installer packaging preset (ARM64 or x86-64) in
`installer/packaging`. Then, from the repository root:

```sh
cmake \
  -DBUILDER_DIST_BUILD=/absolute/path/to/configured-native-installer-build \
  -DBUILDER_OUTPUT=/absolute/new/path/ActRaiserRecompBuilder.app \
  -P installer/desktop-shell/package-macos.cmake
```

The script checks the target architecture, builds and stages the existing
offline installer, builds the Go shell, assembles the `.app`, then ad-hoc signs
and verifies it. Output must be new; an existing app is never overwritten.
It needs neither Node nor the Wails CLI. `framework_darwin.go` supplies the
UniformTypeIdentifiers linker flag normally provided by Wails' CLI.

For development without repackaging, build the shell and use
`--payload /path/to/clean/manifested/install --workspace /path/to/new/workspace`.
The stdlib-only `go -C installer/desktop-shell run ./cmd/payload --root ...
--os linux --arch arm64` command can manifest a cross-staged test payload.

## Build the Linux prototype

On macOS, `package-linux.cmake` automatically uses the cross-host recipe
described below. On Linux, `-DBUILDER_LINUX_CROSS=ON` selects the same pinned SDK
path. Without that flag, Linux retains the native-maintainer implementation:

The Linux shell builds **on Linux**, with GTK3 and WebKitGTK 4.1 development
packages available to the maintainer:

```sh
go -C installer/desktop-shell build -tags production,webkit2_41 \
  -trimpath -ldflags '-s -w' -o /absolute/output/ActRaiserRecompBuilder .
```

That executable alone is **not** a self-contained Linux release. The native
Debian-family packager now builds the AppDir dependency closure and uses the
existing installer payload's pinned AppImage tool/runtime to make the container:

```sh
cmake \
  -DBUILDER_DIST_BUILD=/absolute/path/to/configured-native-installer-build \
  -DBUILDER_OUTPUT=/absolute/new/path/ActRaiserRecompBuilder.AppImage \
  -P installer/desktop-shell/package-linux.cmake
```

Maintainer prerequisites include Go, CMake, a native C compiler, `pkg-config`,
GTK3/WebKitGTK 4.1 development packages, GStreamer base/good plugins, pixbuf and
GTK query utilities, GLib schema tools, shared MIME data, Adwaita/hicolor icons,
`bwrap` and `xdg-dbus-proxy`. The packager does not install or download these.
It currently understands Debian-family multiarch paths; other layouts fail
explicitly. For an already clean installer tree and native shell, the equivalent
assembly command is:

```sh
go -C installer/desktop-shell run ./cmd/package \
  --source /absolute/clean/installer-tree \
  --shell /absolute/native/ActRaiserRecompBuilder \
  --output /absolute/new/ActRaiserRecompBuilder.AppImage
```

Use `.AppDir` as the output suffix to inspect staging without compression.
The package contains GTK/WebKit libraries, WebKit helpers and injected bundle,
GIO/pixbuf/GTK resources, and the installed GStreamer plugins. It intentionally
leaves glibc, graphics-driver interfaces, and the C++/unwinder runtimes
(`libstdc++.so.6`, `libgcc_s.so.1`) to the OS. The Wayland client
(`libwayland-client.so.0`) also comes from the OS: modern Mesa may require newer
client symbols even when the GUI uses X11. Bundling the older SDK client can
abort WebKit with `Could not create default EGL display: EGL_BAD_PARAMETER`.
This follows the [AppImage host-library policy](https://github.com/AppImageCommunity/pkg2appimage/pull/559).
An assembly-time guard rejects host-coupled libraries in the finished GUI tree.
The OS libraries must satisfy the bundled
WebKit's requirements (Debian 12-era C++ runtime or newer); bundling an older
C++ runtime can break newer Mesa/LLVM drivers. The ABI report records versioned
library requirements as well as the maximum glibc version. Library/module overrides
are removed before starting the build helper, so they do not leak into the
compiler or generated game.

Debian's release WebKit has absolute helper paths compiled in and ignores the
developer-only `WEBKIT_EXEC_PATH` override. The packager relocates only known
NUL-delimited path prefixes in the copied WebKit library, without changing ELF
offsets, and keeps the GUI's working directory at `AppDir/usr`. It records
replacement counts and before/after hashes. Missing expected paths fail the
package step for review. No sandbox-disabling flag is added; enabling/auditing
the framework's upstream sandbox behavior is a separate hardening task.

`usr/share/doc/ActRaiserRecompBuilder/linux-runtime.json` records copied runtime
files, hashes, Debian package/source versions, build OS, and relocation details;
adjacent `runtime-licenses/` contains package copyright notices. This is useful
provenance, not a completed redistribution/source-offer audit or a complete hash
index of generated caches and the final container.

Normal mounted AppImage launch still needs the OS's FUSE device and mount
helper (`fusermount3` or `fusermount`; Debian's `fuse3` package supplies one).
This is separate from bundled GUI/build dependencies. On a machine without
FUSE, `APPIMAGE_EXTRACT_AND_RUN=1 ./ActRaiserRecompBuilder.AppImage` runs the
same artifact by extracting it first. That fallback needs temporary disk space;
it is not yet an automatic double-click fallback on a FUSE-less desktop.

The cross-build GUI SDK now targets **Debian 12 / glibc 2.36**. Every packaged
GUI ELF—including WebKit helpers and plugins—is checked against that ceiling.
The complete artifact has a separate `linux-abi.json` report covering its offline
compiler and SDL payload too: ARM64 SDL currently requires glibc 2.38, while the
Steam Deck variant enforces 2.36 for the **entire** artifact. The OS must still
supply compatible graphics/display interfaces. Native runtime tests are separate
from this static ABI check. See
[VERIFICATION.md](VERIFICATION.md) for precisely which paths were exercised.

### Steam Deck candidate and acceptance

`make release-steam-deck` generates
`release/ActRaiserRecompBuilder-steam-deck.AppImage`, using the existing
Steam Runtime/SDL payload and the Debian 12 GUI SDK. This replaces the old Deck
archive in the default release. The user has confirmed the Workshop opens on
Deck after the Wayland-library fix; full game-build and desktop/input acceptance
remain in progress. The game packager also accepts SteamOS's absolute-path
`ldd` loader mapping without bundling the system loader.
The default window is 1180×740 so it fits the Deck's 1280×800 desktop.

For a normal player check, copy the AppImage into a writable folder in Desktop
Mode, mark it executable in file properties, and launch it. It creates its game
output beside itself; no `pacman`, OS read-only unlock, or system compiler install
is required. Do not copy the Builder into a root-owned system directory.

For repeatable acceptance, build a **separate test-only** image:

```sh
(cd installer/packaging && cmake --preset steam-deck)
cmake -DBUILDER_DIST_BUILD=installer/packaging/build/steam-deck \
  -DBUILDER_OUTPUT=/absolute/new/ActRaiserRecompBuilder-smoke.AppImage \
  -DBUILDER_SMOKE_TEST=ON -P installer/desktop-shell/package-linux-cross.cmake
```

Run in a Deck Desktop Mode terminal (no additional test packages needed):

```sh
sh test-steam-deck.sh /absolute/ActRaiserRecompBuilder-smoke.AppImage
```

The script retains logs in a fresh folder and requires explicit renderer PASS
results for mounted launch, relocation, global storage and extraction fallback.
It checks the import prompt, JavaScript, preferences, tabs, PDF bytes, WAV
decoding and multipart requests. `AR_BUILDER_SMOKE_ROM=/absolute/your.sfc`
optionally adds a real, private game build. Keep that output private. A normal
release has no test hooks and cannot satisfy this script. Fonts/scaling, audible
audio, folder dialogs, GPU rendering and controller/touch input still require
human checks; Desktop Mode acceptance does not qualify Steam Gaming Mode.

## Importing an older installation

The shared Workshop frontend checks the directory containing the outer Builder
artifact, not its CWD, for legacy `utils/` data and adjacent game sidecar targets.
It offers a first-launch import review even if output assets were already seeded.
The sidebar's **Import previous installation…** action remains available after
skipping. The folder chooser also accepts a pasted absolute path when the OS
chooser is unavailable. Multiple detected data roots require an explicit choice.

Only `config.ini`, `settings.ini`, `diorama-layers.ini`, `saves/`, and
`game-assets/` are considered (including Workshop language preferences and
authored language projects). No old code, build tools, ROM, or shipped defaults
are imported. Browser-local appearance choices are not recoverable from a folder.
Existing user files/deletions win; an exact, untouched seeded asset can be replaced.
Preview hashes are rechecked before import, and copies are verified before atomic
publication. Source files are never altered. Symlinks and overlapping data paths
are rejected. Failed imports leave no success receipt and can be previewed/retried.

`.actraiser-import.json` records skips and completed sources per output, separately
from `.actraiser-seed.json`. It prevents repeated imports from resurrecting old
saves. Import is blocked during builds/cleanup/audio generation and concurrent
Workshop requests. Close both games and save all editor changes before import;
reload the Workshop and any other open tabs afterwards. This never changes the
game launcher's portable/global selection or automatically merges global data.

## Build the Windows prototype

Both Windows architectures can be packaged directly on macOS, Linux or Windows.
The maintainer needs Go, CMake and the existing installer packaging dependencies,
plus `7zz`/`7z` on non-Windows hosts (`expand.exe` is used on Windows):

```sh
cmake -S installer/packaging -B build-builder-windows-amd64 \
  -DSNESBUILD_GOOS=windows -DSNESBUILD_GOARCH=amd64
cmake \
  -DBUILDER_DIST_BUILD=build-builder-windows-amd64 \
  -DBUILDER_OUTPUT=/absolute/new/path/ActRaiserRecompBuilder.exe \
  -P installer/desktop-shell/package-windows.cmake
```

Use `arm64` and a separate build/output directory for Windows ARM64. The script
stages the existing offline installer, cross-compiles the GUI shell, verifies
the pinned WebView2 CAB, unpacks it, then embeds every runtime file and the
installer payload. Players need neither those maintainer tools nor a VM.
Existing output files are refused. Current packages are unsigned prototypes;
sign **after** packaging, never append the archive to an already signed shell.

[`windows-webview2.json`](windows-webview2.json) is the single version/URL/SHA-256
pin for each architecture. Maintainers update these together from Microsoft's
[Fixed Version downloads](https://developer.microsoft.com/en-us/microsoft-edge/webview2/)
and repeat acceptance tests. There is no player-side auto-updater. For an
offline maintainer run, supply `-DBUILDER_WEBVIEW_CAB=/path/to/downloaded.cab`;
the same hash check remains mandatory. Keep runtime files and their notices
intact; redistribution/license review is still a release gate.

First launch extracts into `<workspace>-runtime/<archive-hash>/`; WebView2's
writable profile is `<workspace>-webview`. The build/project workspace remains
`<workspace>`. In portable mode, keep the executable, its `.portable` sidecar,
workspace and both sibling directories together. Without the sidecar, all three
directories are under the user's local application-data directory. Runtime
caches are byte-verified on warm launches too; modified/conflicting caches are
refused rather than repaired or overwritten. Windows path/device-name hazards,
archive corruption, wrong architectures and ROM-bearing payloads are rejected.

The runtime cache uses an explicit current-user/SYSTEM DACL, with sandbox
read/execute grants limited to the extracted WebView2 tree. The prototype
requires a local ACL-capable filesystem such as NTFS, not a network share or
FAT/exFAT portable drive. Fixed Runtime network-path restrictions and Windows 10
sandbox permission requirements come from
[Microsoft's distribution guidance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution).
Inherited `WEBVIEW2_*` overrides are removed for packaged startup and from the
build helper's environment. No sandbox-disabling flags are added.

This is approximately a 510 MiB x64 / 492 MiB ARM64 download. Plan for roughly
2 GiB before a game build: the compressed `.exe`, extracted runtime/tools and
a separate writable copy of the toolchain all consume space. Actual compiler
caches, generated game data and browser profiles add more. Initial extraction
runs before the webview exists and has no progress/cancel window yet; this is
a remaining usability gate, especially with antivirus scanning many files.

The shell, backend, build driver, compiler/archive/version checks and folder
picker launch path now avoid creating console windows when their Go parent
has no console. Existing console/Windows Terminal launches retain their console
behavior. This applies the per-process
[Windows creation flag](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags)
at each Go launch boundary; full native process-tree observation is still needed.

For developer-only builds, the raw Windows shell can also be cross-compiled
without CGO, separately for `amd64` and `arm64`:

```sh
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 \
  go -C installer/desktop-shell build -tags production,wv2runtime.error \
  -trimpath -ldflags '-s -w -H windowsgui' \
  -o /absolute/output/ActRaiserRecompBuilder.exe .
```

`wv2runtime.error` deliberately disables Wails' default runtime-download prompt.
This developer shell still needs `--payload`, an existing WebView2 runtime,
and native Windows testing. `--webview-runtime` selects a fixed runtime folder.
Its webview profile lives beside the selected workspace as `<workspace>-webview`.

Cross-host inspection of a packaged executable (no Windows code is executed):

```sh
go -C installer/desktop-shell run ./cmd/windows-package --mode extract \
  --input /absolute/ActRaiserRecompBuilder.exe --output /absolute/new/inspection
```

`extract` verifies the ZIP/footer, architecture, manifest and every extracted
file's hash. `--mode verify` checks container integrity and metadata only. The
low-level `--mode pack` accepts an already unpacked runtime; use the CMake
entrypoint to guarantee that tree came from the hash-pinned CAB. Authenticode
certificate-table layout is supported, but the Go inspector does not validate
publisher signatures. Use native Windows signing/verification tools for that.

## One-command releases from macOS

Install the native maintainer tools once (Xcode Command Line Tools must also be
installed and usable; Go must be 1.25 or newer):

```sh
brew install go cmake pkgconf xz zstd squashfs glib shared-mime-info sevenzip
make release
```

No Docker, Linux VM, Node, Electron or Wails CLI is used by the release build.
Here “native host build” means macOS tools **cross-compile** target executables;
it does not mean Linux/Windows programs run on macOS.

`release/` receives seven deliverables, each with a SHA-256 sidecar:

- `ActRaiserRecompBuilder-macos-{arm64,x86_64}.app.zip` (each contains the
  ad-hoc-signed `ActRaiserRecompBuilder.app`).
- `ActRaiserRecompBuilder-steam-deck.AppImage` (candidate; device testing required).
- `ActRaiserRecompBuilder-windows-{arm64,x86_64}.exe` (unsigned).
- `actraiser-recomp-linux-{arm64,x86_64}.tar.xz` (generic browser-based Builder).

The redundant macOS/Windows/Deck archives and generic Linux Builder AppImages
are no longer default release outputs. Low-level packaging entrypoints remain
available for diagnostics. The Deck image enforces a glibc 2.36 ceiling over the
complete payload. Bundling SDL alone would not fix GTK/WebKit's baseline.
Real SteamOS acceptance is not inferred from cross-builds.

`make release-linux-arm64`, etc. build one target. `make release DESKTOP=0`
retains the original archive-only workflow on any supported maintainer host.
The pure-CMake release orchestrator uses the same matrix and driver
(`installer/packaging/release.cmake`); configure it with
`-DSNESBUILD_DESKTOP=OFF` for archive-only builds. Individual `package-*` CMake
workflow presets still explicitly produce their CLI archive. Inspect the
default plan without downloads or filesystem changes with:

```sh
cmake -DBUILDER_PRINT_PLAN=ON -P installer/packaging/release.cmake
```

The release publisher stages into a fresh directory, then replaces only the
explicitly named generated artifact and checksum. Only after success does it
remove that target's superseded release and checksum. Per-target build trees
are removed unless `KEEP_BUILD=1` (CMake: `SNESBUILD_KEEP_BUILD=ON`); download
caches and the host compiler are retained. Direct packaging commands still
refuse existing outputs. Existing portable workspaces are never touched.
No portable sidecar ships by default with the Builder: place a matching
`<artifact>.portable` containing `BuilderData` next to it to opt into portable
storage. This applies after extracting the macOS ZIP, not next to the ZIP itself.

### Linux SDK cross-build

`linux-sdk-amd64.json` and `linux-sdk-arm64.json` pin the Debian 12 GTK3,
WebKitGTK 4.1 and related dependency closure, including versions, official
package URLs, sizes, SHA-256 hashes and source-package provenance. Ordinary
release builds use these locks; they do not silently refresh GUI dependencies.
Downloads/extraction live in `installer/packaging/cache/desktop` and survive
normal release cleanup. Windows Fixed WebView2 CABs are cached there too.

`internal/linuxsdk/resolve.go` defines the Bookworm suite and glibc 2.36 baseline.
Refreshes include Bookworm updates/security, retaining current security-patched
[WebKitGTK 4.1 packages](https://packages.debian.org/bookworm/libwebkit2gtk-4.1-0)
without moving the OS baseline. Changing the suite or baseline requires ABI and
native acceptance again. The audit reads ELF version-needs, not strings output;
it handles [GLIBC_ABI_DT_RELR](https://sourceware.org/pipermail/glibc-cvs/2022q2/078142.html)
as a 2.36 requirement and rejects unknown/private glibc ABI tags.

On macOS the script automatically creates/mounts a 12 GiB **sparse, case-sensitive
APFS disk image** for the SDK, consuming only the data actually written. Linux
headers can differ only by capitalization, so a default case-insensitive Mac
volume is insufficient. This disk image is storage, not a virtual machine. The
script serializes access with a lock and detaches on success or failure. After
a crash, inspect active builds and mounts before clearing a stale lock; never
recursively delete a mounted SDK directory. `make clean-packaging-mounts`
detaches packaging-cache images; normal cache cleanup is `make clean-all`.

Packages are extracted as data; Debian programs and maintainer scripts are
never run. Go/CGO uses the existing pinned **Mac-host Zig** with target SDK
headers/libraries. Runtime dependency discovery reads ELF `DT_NEEDED` and
private search paths instead of invoking Linux `ldd`; conflicting library
names, escaping paths and legacy `DT_RPATH` (which could override the bundled
library search path) fail for review. Native host schema/MIME compilers produce data
caches. Native `mksquashfs` creates the filesystem, which is concatenated with
the installer's pinned target AppImage runtime as bytes. The resulting image
includes the SDK lock, runtime provenance and copied package notices.

To refresh GUI SDK pins deliberately, generate **new** files, review their
dependency/version changes, replace the checked-in locks, and repeat native
acceptance tests for both architectures:

```sh
go -C installer/desktop-shell run ./cmd/linux-sdk --mode resolve \
  --arch amd64 --output /absolute/new/linux-sdk-amd64.json
go -C installer/desktop-shell run ./cmd/linux-sdk --mode resolve \
  --arch arm64 --output /absolute/new/linux-sdk-arm64.json
```

The resolver is deliberately scoped to Debian binary indexes, not a general
APT solver: unsupported/conflicting dependency constraints fail explicitly.
Metadata is fetched over official HTTPS; package bytes are checked against the
reviewed SHA-256 pins. Debian Release-file GPG verification is not implemented.
Live mirrors may retire old package URLs; refresh pins explicitly if a pinned
download disappears. Extracted SDK reuse trusts the maintainer-owned local
cache; delete/restage that specific cache if its contents were modified.

Build-host independence does not replace native acceptance. See
[VERIFICATION.md](VERIFICATION.md) for build, archive and runtime evidence,
including remaining Windows, x86-64 Linux, UI, update and signing gates.

## Automated checks

```sh
go -C installer test ./...
go -C installer/desktop-shell test -race ./internal/... ./cmd/...
```

Tests cover private readiness files, server lifecycle, the authenticated proxy,
checksum staging, edit preservation, version refusal, locking, sidecar/global
selection, application-boundary checks, GUI environment isolation, narrow
WebKit relocation rules, Windows PE/archive round-trips, architecture checks,
unsafe archive paths, corruption and executable-cache tampering. The loopback integration tests need
permission to listen locally. Native Windows lock/path tests still need a
Windows test host.

The optional **`smoketest` build tag is test-only**. It injects an embedded-page
probe, prints `BUILDER_RENDERER_SMOKE PASS ...` or `FAIL ...`, and requests a
clean shutdown. Use a disposable workspace and an external timeout. The test
must require an explicit PASS line; exit zero alone is insufficient. All three
desktop packaging scripts accept `-DBUILDER_SMOKE_TEST=ON`. Linux can also add
`smoketest` to its build tags and run under
`dbus-run-session -- xvfb-run -a`. Never ship that build.

The probe checks actual embedded JavaScript, backend status, preference writes,
tab navigation, PDF bytes, generated WAV decoding/duration, and multipart
validation. Set `AR_BUILDER_SMOKE_ROM` to an existing private local ROM to also
submit its bytes as a Blob-backed multipart request and wait for a full game
build. No ROM is embedded in the test binary. Do not set this variable for a
normal renderer-only test. Decoding is not a test of audible playback.

The Windows player harness uses a **packaged smoketest executable**, not the raw
shell, on a disposable Windows VM/session without system build tools:

```powershell
.\installer\desktop-shell\test-windows-player.ps1 -Builder C:\tests\Builder-smoke.exe
# Optional: append -Rom C:\private\ar.sfc for a full game build.
```

It retains results in a new directory, isolates `LOCALAPPDATA`, tests portable
storage and relocation with paths containing spaces, preserves a workspace edit,
then tests global storage without the sidecar. It requires an explicit renderer
PASS and observes WebView2 running from the extracted runtime directory. It
does not install dependencies or disconnect the network; disconnect the VM
before running for an offline test. A full-build PASS does not launch the
generated Windows game. The harness and Windows-only ACL/console tests have
not yet been executed natively; PowerShell syntax/execution is also pending.

The repeatable Linux player check refuses system Go/CMake/compilers and
GTK3/WebKit/SDL3, tests paths with spaces, portable relocation, global storage,
extract-and-run, and FUSE when both its device and mount helper are available
(otherwise the mounted check explicitly reports SKIP). With the private ROM option it also
launches the generated game headlessly in portable and global modes:

```sh
AR_BUILDER_SMOKE_ROM=/absolute/private/ar.sfc \
  sh installer/desktop-shell/test-linux-player.sh /absolute/Builder-smoke.AppImage
```

Run in a disposable player VM with Xvfb, xauth and a session D-Bus installed.
For an offline test, enter a network namespace with only loopback enabled
before invoking the script as an unprivileged player. It retains test results
and generated private data in a new directory under the caller's working
directory (override its parent with `AR_BUILDER_TEST_ROOT`). Its extraction
directory is disk-backed too, avoiding a RAM-backed `/tmp` during a full build.
It does **not**
verify visual appearance, native file dialogs, downloads, PDF rendering,
audible output, or editor GPU behavior. Those remain separate acceptance tests.

To validate a game already generated by an earlier desktop-host run without
recompiling it, use `sh installer/desktop-shell/test-linux-game.sh
/absolute/generated/ActRaiserRecomp.AppImage` in the same offline player VM.

## Release gates / next milestones

1. **Shell integration:** finish native file/save dialogs, export downloads,
   PDF rendering/fallback, keyboard shortcuts, editor unsaved-state handling,
   profile placement, cancellable staging and crash diagnostics. Repeat actual
   desktop-host game generation/launch across supported platforms.
2. **Portable distributions:** broader Linux AppDir/AppImage qualification and
   native qualification of the Windows embedded-payload/fixed-WebView2 bootstrap. Test offline on clean
   player VMs, with paths containing spaces, relocated sidecars, no system build
   tools, FUSE and extract-and-run, and supported Windows architectures.
3. **Release hardening:** upgrades that preserve user projects, cleanup/repair,
   all third-party notices/source provenance, icons/version resources, package
   smoke checks, and publisher signing/notarization decisions. The prototype
   app is only ad-hoc signed; this does not solve downloaded-app Gatekeeper or
   Windows SmartScreen behavior. No public redistribution yet.

## Upstream references

- [Wails build documentation](https://wails.io/docs/gettingstarted/building/)
  and [options](https://wails.io/docs/reference/options/).
- [Wails Linux runtime dependencies](https://wails.io/docs/guides/linux-distro-support/).
- [Tauri's WebKit AppImage bundler](https://github.com/tauri-apps/tauri/blob/dev/crates/tauri-bundler/src/bundle/linux/appimage/linuxdeploy.rs)
  is a packaging reference, not a dependency or a proven recipe for this app.
- [Microsoft WebView2 distribution](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution).
