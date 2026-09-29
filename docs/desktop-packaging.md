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
.\ActRaiserRecompBuilder-windows-arm64.exe --verify-bundle
```

```sh
./ActRaiserRecompBuilder.app/Contents/MacOS/ActRaiserRecompBuilder --verify-bundle
./ActRaiserRecompBuilder-steam-deck.AppImage --verify-bundle
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

For validation followed by packaging, set up the
[local quality-check dependencies](../CONTRIBUTING.md#developer-checks) and use
`make release-checked` or `make release-checked-windows-arm64`. These run
`make check-release` (the ordinary checks plus optimized tests) first and stop
if a check fails. `make check-release` also works independently.

`make release DESKTOP=0` selects archive-only packaging. See the
[Builder README](https://github.com/DerrickGold/ar-recomp/tree/main/installer) for building and running the CLI locally.

Builder distributions must not include your ROM, generated game, extracted
content, or private project backups.
