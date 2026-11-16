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

On Windows, the Builder automatically stages `zig.exe` at a short path when
the bundled compiler's path exceeds the Windows process-launch limit. It uses
the current build's scratch directory when possible, otherwise a private
temporary directory under `TEMP` or `LOCALAPPDATA`. The bundled SDK, portable
data, and game output stay in their chosen locations. The temporary compiler
is removed after success, failure, or cancellation; no registry change or
administrator access is required.

To move a portable installation, copy the entire folder, including its sidecar
and data. Copying only the app switches it to per-user storage. Switching modes
does not automatically transfer saves or settings; back up and copy the data
you want to keep. Updates preserve existing saves, settings, and edited seed
files. Diorama scenery is release-managed: changed bundled
`defaults/diorama-layers.ini` content replaces the live `diorama-layers.ini` on
the next game launch, so corrections to existing rooms and removed tile patches
reach upgraded installations. Numbered `diorama-layers.ini.pre-update-N` backups
preserve previous copies. `diorama-layers.ini.installed` records the last applied
bundle; local authoring survives normal launches until bundled content changes.
Settings, saves and custom asset entries retain their existing preservation rules.
Use the Workshop's explicit import workflow to bring in another project's
content while retaining the original.

## RecompLauncher on Windows

The Windows portable ZIP includes a RecompLauncher profile. Play opens the
Builder, where you can build from your ROM, rebuild after an update, or run
the existing game. The Builder waits while the game runs and returns its exit
status when it closes. RecompLauncher uses the generated `ActRaiserRecomp`
folder for saves and game logs.

Easy Setup accepts up to six local `.sfc` or `.smc` files. Select ActRaiser (USA)
for the core game. The Japanese, European English, German, and French ROMs add
regional text references, artwork, and music. ActRaiser 2 (USA) adds Winged
Master and ActRaiser 2 development content.

On first Play with a valid ActRaiser (USA) ROM selected, the Builder starts the
local build and imports any supplied extras. In Big Screen, Play is focused
when the build finishes. All Easy Setup fields can be skipped so an existing
installation can use its saved ROM or build. You can also select files in the
Builder.

To bring over an existing installation in a different folder, use the Builder's
[installation import](../README.md#4-update-or-import-an-existing-installation).
RecompLauncher updates the Builder download. The Builder checks whether the
locally generated game needs to be rebuilt the next time you open it.

## Builder Big Screen

Play from RecompLauncher’s Big Picture or SteamOS Gaming Mode opens the
Builder in fullscreen Big Screen mode. SteamOS Desktop Mode opens the
Workshop. Pass `--big-screen` to request Big Screen or `--big-screen=false`
to request the Workshop. In Steam’s Launch Options, use
`%command% --big-screen` to select Big Screen manually.

Use the D-pad or left stick to move, the bottom face button to select, and
the right face button to go back. Arrow keys, Enter and Escape also work.
Press F8 or the controller’s Menu / Options button to switch between Big
Screen and the desktop Workshop during the same session.
Big Screen shows the button glyphs for the current controller. When present,
RecompLauncher’s controller hint supplies the glyphs at startup.

In Customize, you can switch title artwork, enable or disable installed
language packs, and listen to installed music replacements and ROM previews.
Use the desktop Workshop to add music files or import and edit languages.
On first launch, use **Use suggested folder** to approve the Builder's
suggested destination. **Browse ROMs** in Big Screen can select a file from
your game folder, Downloads, home folder or mounted media. You can also
use the system file picker with a keyboard or mouse.

On a fresh install, **Choose US ROM** opens the ActRaiser (USA) entry and offers
a discovered copy when available. Choose optional ROMs from the checklist.
Both views check file-picker selections and show what needs attention before
enabling Build.

The **ROM checklist** is available on Home in both views and in **Build & play**
in the desktop Workshop. It shows each ROM's purpose, files supplied by the
launcher, files found beside the Builder or in the game folder, and imported
content. Select a row to use a found file, browse for another one, or clear its
selection. The desktop file picker can add or replace ROMs for the next build.
**Clear chosen files** clears the file picker and keeps the checklist choices.
If the game is already playable, use **Import selected extras** to add content
without rebuilding.

The build view shows progress and a live log side by side. Use LB / L1 and
RB / R1 or Page Up and Page Down to scroll the log. Save build report writes
a snapshot of the current phase, error and recent log to the game's `logs/`
folder.
The same buttons page through ROMs, music and language packs. Launch errors
have a separate report action in the same view.

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

### Faster startup and full verification

Packaged Builders on Windows, macOS, and Linux save a successful verification
result. Later launches check the manifest and file metadata, then hash only
files whose size, modification time, or mode changed. Unchanged tools and SDK
files are not reread, including when opening the Workshop just to launch a game.
Windows also reuses its embedded-archive checksum when the package metadata and
manifest match. First launches, new bundles, and missing or unreadable cache
records receive a full check. Explicit development payloads always get a full
check.

To force a complete integrity check, close the Builder and start it with
`--verify-bundle`:

```powershell
.\ActRaiserRecompBuilder-v0900-windows-arm64.exe --verify-bundle
```

```sh
./ActRaiserRecompBuilder.app/Contents/MacOS/ActRaiserRecompBuilder --verify-bundle
./ActRaiserRecompBuilder-v0900-steam-deck.AppImage --verify-bundle
```

Use your download's actual filename. Successful checks refresh the cache;
failed or cancelled checks do not. Corrupt files are reported without silently
replacing them. macOS/Linux cache records live in the Builder workspace;
Windows records live beside its extracted runtime directories. Verification
records contain file metadata, not ROM contents or saves.

This cache detects ordinary changes; it does not protect against deliberate
tampering that also restores file timestamps, or corruption that leaves
metadata unchanged. Use `--verify-bundle` to check file contents in those cases.
Platform security checks such as Gatekeeper and Application Control still apply.

### Windows WebView2 Runtime

The Windows Builder draws its window with the Microsoft Edge WebView2 Runtime
that Windows 11 includes and up-to-date Windows 10 PCs receive automatically;
the download does not bundle its own copy. If the runtime is missing or out of
date, the Builder says so before preparing anything and offers to open
[Microsoft's WebView2 page](https://developer.microsoft.com/microsoft-edge/webview2/).
Install the **Evergreen** runtime there, then start the Builder again. On a
managed device, ask your administrator to install it.

### Windows Application Control

Windows releases include unsigned executables. Windows can allow the outer
Builder to open and still block an internal tool such as `snesbuild.exe` when
you press **Build game**:

```text
start snesbuild regen: fork/exec ...\snesbuild.exe: An Application Control policy has blocked this file.
```

This is Windows refusing to start the tool, before compilation begins. The
Workshop displays recovery guidance and keeps the original error under
**Error details** and in the build log.

1. Open **Windows Security → App & browser control → Smart App Control settings**.
2. If Smart App Control is **On**, it has no exception for an individual app.
   On your own device, you may choose **Off** if you trust the download and
   accept disabling that protection for **all apps**. Read the Windows
   confirmation first; the ability to re-enable it depends on your Windows
   version and updates. Then return to the Builder and retry **Build game**.
3. If this is a managed device, the setting is unavailable, or the error
   persists, give your administrator the copied error report. An administrator
   must approve the blocked tool under the applicable policy.

Smart App Control is separate from antivirus and SmartScreen's **Run anyway**
prompt. Antivirus exclusions, **Run as administrator**, or unblocking the ZIP
do not grant an Application Control exception. Other bundled tools and the
locally generated game can also be assessed separately.

For diagnosis, check **Event Viewer → Applications and Services Logs → Microsoft
→ Windows → CodeIntegrity → Operational**. Policy ID
`{0283ac0f-fff1-49ae-ada1-8a933130cad6}` identifies Smart App Control's enforcement
policy; the generic error alone does not distinguish it from an administrator's
policy. See Microsoft's [Smart App Control FAQ](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-frequently-asked-questions)
and [built-in Application Control policies](https://learn.microsoft.com/en-us/windows/security/application-security/application-control/app-control-for-business/operations/inbox-appcontrol-policies).

### macOS Gatekeeper

The macOS packager uses ad-hoc (local) signatures for the Builder app and
generated game. These provide no verified publisher identity; our releases
are not Developer ID signed or notarized. Downloaded copies can therefore be
blocked even when a local source build runs normally. Bundled helper executables
can also be subject to Gatekeeper; opening the outer Builder is not proof that
every helper will be allowed.

For an unidentified-developer or unverified-app warning on a download you trust,
try opening the app, then go to **System Settings → Privacy & Security → Open
Anyway** and confirm **Open**. This creates an exception for that app. If a
helper is named in a later warning, check the named component and the same
settings panel before retrying the build. If no exception is offered, retain
the exact warning and build log for diagnosis (or contact the administrator
of a managed Mac).

A damaged-app or malware warning needs separate investigation; do not assume
it is just missing notarization. See Apple's [instructions for opening Mac apps](https://support.apple.com/en-us/102445)
and [Gatekeeper's checks on helper code](https://developer.apple.com/videos/play/wwdc2019/701/).

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
```

Run `make release` from the repository root to build the configured platform
downloads without a VM, including native desktop packages and their portable
companions. For one platform, use a target such as `make release-windows-arm64`.
These commands run packaging directly; they do not require Python lint modules,
Node.js lint tools, or the game test dependencies.

Download names include the exact release tag, for example
`ActRaiserRecompBuilder-v0900-windows-x64-portable.zip` and
`ActRaiserRecompBuilder-v0900-linux-arm64.tar.xz`. The compact tag `v0900`
represents version 0.9; `v1000` would represent 1.0. Starting at 1.0, use dotted
tags such as `v1.0`. Both tag styles are preserved as written in filenames.
Windows download names use `x64`; its build target remains `windows-x86_64`.

By default, packaging uses `git describe --tags --always --dirty`. A clean
checkout at a tag uses that tag exactly; development builds retain the commit
distance/hash and `-dirty` suffix when present. To set the release version
explicitly, pass `RELEASE_VERSION` to either local or remote packaging:

```sh
make release RELEASE_VERSION=v0900
make release-remote build-host RELEASE_VERSION=v1.0
```

The selected version is used for filenames, checksums, and the bundled tools'
version stamp. The executable inside a portable archive keeps its stable name.

For validation followed by packaging, set up the
[local quality-check dependencies](../CONTRIBUTING.md#developer-checks) and use
`make release-checked` or `make release-checked-windows-arm64`. These run
`make check-release` (the ordinary checks plus optimized tests) first and stop
if a check fails. `make check-release` also works independently.

`make release DESKTOP=0` selects archive-only packaging. See the
[Builder README](https://github.com/DerrickGold/ar-recomp/tree/main/installer) for building and running the CLI locally.

Each download bundles the pinned Zig compiler with only the parts of its
library that the platform's game builds read: the compiler's support code and
that platform's own C library headers and sources. Every license and notice
file from the Zig release is kept (see `installer/THIRD_PARTY_NOTICES.md`).
`snesbuild toolchain trim`
holds the per-platform list and stops packaging if a listed folder is missing,
so a Zig update that reorganizes its library fails instead of shipping an
incomplete compiler. `make release TRIM_ZIG=0` bundles the complete Zig
release, for example to compare a game build against the untrimmed compiler.

### Packaging across build hosts

Configure an SSH alias in your own SSH config, then pass its name to Make.
For example, if your alias is `build-host`:

```sh
make release-remote build-host
make release-remote build-host other-host localhost
make release-remote build-host PLATFORMS="macos-arm64 windows-arm64"
make release-remote build-host other-host localhost REMOTE_DRY_RUN=1
```

Supply one or more SSH config names, with optional `localhost` to build on the
current machine directly without SSH. Each active host starts with one target
in the order listed in `PLATFORMS`. Remaining targets enter a shared queue:
whenever a host finishes, it takes the next queued target, so faster machines
can pick up work while slower machines are still building. Active builds remain
on their original host. Each target is assigned exactly once; its desktop and
portable artifacts stay together. Repeated host names or targets are rejected,
and hosts beyond the number of selected targets remain idle. Use
`REMOTE_DRY_RUN=1` to inspect the initial assignments and shared queue. Later
assignments depend on which host finishes first and are printed as they happen.

The coordinating machine needs Git, SSH (when using remote hosts), Make, and
Python 3.9+. Each build host, including `localhost` when selected,
needs Python 3.9+, Make, Go 1.25+, CMake 3.25+, and the same packaging
prerequisites as a local release. Use macOS for the full desktop release matrix;
archive-only builds with `DESKTOP=0` also support Linux hosts. Each active host
must support all selected targets, since it may take any target from the queue;
host capabilities are not detected automatically.
SSH must work without an interactive password prompt. The alias and credentials stay in your
local SSH configuration; no machine configuration is saved in the repository.

Install the build prerequisites on the remote host itself. For a macOS host,
connect with `ssh build-host` and run the `brew install …` command above there.
A successful SSH connection only establishes access; it does not supply Go,
CMake, or the native packaging tools. Missing prerequisites include an install
hint, and remote build failures retain their cause in the final error message.
SSH connection and transport failures are reported separately.

The command sends the current working tree, including tracked edits and
non-ignored new files. Deleted files, `.git`, ignored ROMs, local settings,
build output, and internal archives are omitted. No commit or push is needed.
One source snapshot is uploaded once per active host. A worker session stays
open to build and return each newly assigned target, reusing that source copy
and the host's caches. The local Git version, or explicit `RELEASE_VERSION`, is
shared by every build host, so all artifacts retain the same source and version
stamp. `REMOTE_DRY_RUN=1`
reports the selection without connecting or building. Like `make release`, this target packages directly;
run the developer checks locally when needed.

Build output streams live to the terminal, prefixed with its host name,
including individual platform configuration, compilation, packaging, and error
diagnostics. Frequent download updates are throttled in the terminal while the
logs retain every update. Each attempt
also saves a complete log under `runs/release-remote/<attempt>/build.log` and
separate host logs under `hosts/<host>.log`. Every log entry and live logged
message includes an ISO 8601 UTC timestamp with millisecond precision, such as
`[2026-10-06T00:39:12.345Z] [build-host] Building linux-arm64`. Host output uses
the coordinator's receipt time, with the same timestamp in both logs, so entries
can be compared across hosts without depending on their clock settings.
Carriage-return download updates are saved as individual timestamped lines.
Log paths are printed at
the start and the combined log path at finish, including on failure. The logs
record the version, target assignments, local checkout, and build workspaces.
Temporary source paths in diagnostics correspond to the same relative paths
in the local checkout. A failed host stops the remaining workers and returns a
nonzero exit status. Build, transfer, or checksum failures preserve existing
local release artifacts; nothing is published until every active host succeeds.

Finished packages and SHA-256 sidecars are verified on their build host and
again by the coordinator, then copied into `release/`. Returned target manifests
must match their assignments, and duplicate artifact names are rejected.
Only the selected downloads are replaced; unrelated local files remain.
Each build host retains reusable dependency downloads,
host toolchain, and Go compiler caches in
`~/.cache/actraiser-recomp/release-remote/`. Override this with `REMOTE_DIR=…`
to choose another dedicated cache directory on each host, including `localhost`.
Local builds use an isolated copy of the snapshot, leaving the checkout's
packaging build trees untouched. An existing cache directory must have
been created by this workflow; an unrelated checkout or directory is refused.

Each job's temporary sources, build trees, and artifacts are removed after
handoff, build failure, or interruption, including with `KEEP_BUILD=1`.
Abandoned job directories from forced termination are removed on the next run.
Builds sharing one cache directory run one at a time to protect its caches.
Local logs remain available independently of build host cleanup and are excluded
from Git and release packages.

Builder distributions must not include your ROM, generated game, extracted
content, or private project backups.
