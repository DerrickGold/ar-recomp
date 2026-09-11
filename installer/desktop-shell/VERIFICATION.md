# Feasibility checks — 2026-09-10

These results describe the `builder-desktop-shell` prototype, not a
release qualification. The production installer/game-artifact work was merged
locally into `main` at `1b785490` before this exploration. Nothing was pushed.

## Portable Builder release companions

The release publisher now derives a portable companion from each already-built
macOS, Windows and Steam Deck desktop artifact. This is an archive-only step:
it copies the validated application, adds the exact adjacent `.portable` marker
with `BuilderData`, and compresses the enclosing folder without rebuilding the
payload. Policy tests cover all five names, require both variants before old
release pruning, inspect ZIP/tar.xz contents, and verify that AppImage extraction
retains an executable bit. Full rebuilt-artifact and native-platform acceptance
remain part of the next release qualification; the historical seven-artifact
results below predate these five additional wrappers.

## Open issue: launching the game from the Workshop

The user reports `SDL_Init failed: No available video device` when pressing
Play in the Builder on Steam Deck Desktop Mode, while opening the generated
game AppImage directly works. This remains unresolved at this checkpoint;
direct game-AppImage launch is the workaround. Debugging and the planned
workspace storage/recovery UI are deferred, not included as completed features.

A bounded ARM64 Linux VM comparison reached SDL's X11 video initialization
through direct launch, the headless Workshop backend, and the actual Wails
desktop host. All three then failed GPU-renderer creation in that VM, so this
does not qualify graphical gameplay or reproduce the Deck failure. The reused
desktop smoke image also reported an import-dialog test failure; it was not a
passing full smoke run. Diagnostic logs were retained locally and the VM was
shut down. A detailed SDL log from the Deck is still needed.

## Release cleanup and SteamOS game-packaging follow-up

- Make and the CMake release orchestrator now share a seven-target matrix:
  macOS ARM64/Intel apps, Windows ARM64/x64 executables, the Steam Deck AppImage,
  and generic Linux ARM64/x64 archives. Explicit legacy archive workflows remain
  available; default desktop builds no longer also compress a redundant archive.
- All seven current releases rebuilt successfully on macOS: the Deck target
  first, then the other six through the shared root Make entrypoint. Both
  generic Linux archives passed their private-input leak gates. All seven
  SHA-256 sidecars verify; both macOS ZIPs extract into deep/strict-valid signed
  apps with matching shell architectures. Windows container verification passes
  for x64 and ARM64 with their pinned Fixed WebView2 metadata (native Windows
  execution is still outstanding). Only the seven artifacts and seven checksum
  files remain in `release/`; normal cleanup retained the host compiler and
  dependency caches, with no SDK volume/lock or per-target build tree left over.
- Regression tests verify the release plan, invalid/duplicate target rejection,
  and exact-name pruning only after replacement publication. Missing replacements,
  symlinks and directory collisions are refused without partial pruning.
  Both normal and legacy CMake orchestrators configure successfully.
- Removed obsolete browser-transport/privacy reassurance from the shared Workshop
  UI in all four languages. Backend loopback/session security and warnings about
  sharing ROM-derived material remain unchanged. The installer and shell
  internal/command race suites pass, including catalog/bootstrap checks.
- The user confirmed that the Workshop now opens on real Steam Deck hardware
  after the Wayland-client exclusion, then reported a game-build packaging error:
  `ldd` maps `/lib64/ld-linux-x86-64.so.2` to an absolute `/usr/lib64/` path.
  The game packager now recognizes mapped x86-64/ARM64 system interpreters and
  leaves them on the host. Tests retain SDL bundling and still reject missing
  libraries, relative paths and path-like ordinary dependency names.
- Rebuilt the normal Deck release on macOS with this fix. Its full-artifact
  report covers 365 ELF programs/libraries, with maximum required glibc 2.36;
  SquashFS inspection confirms no bundled Wayland client, and its checksum
  verifies. This is packaging/static coverage; completion of the game build on
  real Deck hardware still needs another player test.
- Removed approximately 20.3 GiB of obsolete prototype binaries, smoke images,
  extracted inspection trees and old staging builds. Logs/provenance records,
  player workspaces, game data, VM disks and shared dependency caches were kept.
  These generated artifacts can be rebuilt, but are no longer present at the
  historical test paths mentioned below.

Workspace identity mismatch versus a live session lock, portable marker setup,
and non-destructive test-workspace reset instructions are documented in the
[desktop README](README.md#resetting-a-test-workspace). Storage/recovery UI is
deferred until after this packaging cleanup.

## Steam Deck EGL failure follow-up

The first reported real Deck launch opened a blank window and logged
`Could not create default EGL display: EGL_BAD_PARAMETER. Aborting...`.
This is a WebKit graphics initialization failure; the previous ARM64/Xvfb PASS
did not exercise that graphics path.

- Inspection of the failing-generation AppImage found bundled Wayland client
  1.21.0, despite using the host's Mesa/EGL. It lacks
  `wl_display_create_queue_with_name`, required by newer Mesa; this is the
  [documented reason to exclude the client library](https://github.com/AppImageCommunity/pkg2appimage/pull/559).
- GUI packaging now leaves `libwayland-client.so.0` to the OS for both native
  and cross builds. Other bundled GUI libraries remain unchanged. This adds
  that client to the documented host runtime prerequisites; it does not require
  running a Wayland session or modify the game/compiler payload.
- Added regression coverage for dependency exclusion and a finished-GUI-tree
  guard against reintroducing host-coupled libraries through resource copying.
  The packaging/SDK race tests and packaging vet checks pass. No GPU-disabling
  flags or system-library changes are used for this candidate.
- `make release-steam-deck KEEP_BUILD=1` rebuilt the normal release on macOS.
  Its full-artifact audit covers 365 ELF programs/libraries with a glibc 2.36
  ceiling. Inspection of the resulting SquashFS confirms the Wayland client
  is absent; checksum verification passes and the SDK volume/lock were cleaned.

The user subsequently confirmed that the rebuilt image opens the Workshop on
Deck. Full game-build acceptance remains in progress as described above.
The historical smoke images and ARM64 VM results below predate this adjustment.

## Installation import and Steam Deck compatibility follow-up

- Full installer race suite passes, including new read-only import previews,
  destination precedence, untouched-default replacement, preserved deletions,
  skip/import receipts, stale-preview refusal, cancellation, symlink/overlap
  rejection and session-scoped HTTP confirmation/concurrency checks.
- A real browser session detected a synthetic legacy `utils/` installation,
  previewed and imported settings/preferences/saves, retained a destination
  conflict, reopened without prompting, and refused a repeat import. No real
  saves or ROM were used. macOS and Windows x86-64 shell builds passed.
- Replaced both GUI SDK locks with Debian 12 Bookworm plus updates/security,
  WebKitGTK 2.50.6, targeting glibc 2.36 instead of 2.41. Explicit lock refresh
  stays on this suite; no package manager runs on a player's machine.
- macOS cross-built the **x86-64 Steam Deck candidate** through
  `make release-steam-deck KEEP_BUILD=1`, with 366 ELF programs/libraries audited
  and a maximum required
  glibc of 2.36, including offline build tools and SDL. Reports are embedded at
  `usr/share/doc/ActRaiserRecompBuilder/linux-abi.json`. These are build/static
  ABI results, not x86-64 or SteamOS execution results.
- First Bookworm staging exposed legitimate systemd `\xHH` filenames. The
  extractor now permits those literal escapes on Unix without decoding them;
  arbitrary backslashes/traversal stay rejected. Tests pass. The mount wrapper
  detached after this failure and after successful normal/smoke builds.
- Clean-player Debian 13 ARM64 testing exposed a real newer-Mesa incompatibility:
  the bundled Bookworm C++ runtime shadowed the host runtime required by its
  LLVM/Z3 graphics stack, preventing EGL initialization. GUI packaging now leaves
  `libstdc++.so.6` and `libgcc_s.so.1` to the OS alongside glibc/graphics. It does
  not disable GPU rendering or add WebKit sandbox overrides.
- Testing also caught fresh output/workspace containment checks that incorrectly
  required both directories to exist, and competing read-only import-state
  requests returning 409. Both have regression tests. Startup progress and
  failures now appear on stderr as well as in the desktop window.
- The final Mac-cross-built ARM64 smoke image passed all four offline clean-player
  Debian 13 probes: fresh portable, relocated/warm, fresh global workspace and
  FUSE-mounted launch, with an unchanged image hash. Each reported explicit PASS
  for JavaScript, installation-import prompt/status, preferences, tabs, PDF bytes,
  WAV decoding and multipart validation. No system GTK3/WebKit/SDL3/build tools,
  graphics-disabling overrides or network access were used. This run did not
  rebuild the game or use a ROM. It validates shared Linux packaging, not SteamOS
  or x86-64 execution. ARM64's separate SDL payload still has a glibc 2.38 floor;
  the GUI is audited at 2.36.
- `test-steam-deck.sh` uses an existing Desktop Mode session, not Xvfb or newly
  installed packages. It requires explicit smoke PASS results; **not yet run on
  a Steam Deck**. Real display/audio/GPU/input and Gaming Mode remain unqualified.

The normal candidate and checksum are in `release/`. Historical test logs remain
under ignored `build-builder-shell/deck-compat-*.log`; obsolete test-only images
were removed during release cleanup. Generate a fresh smoke image using the
README's commands before using the Deck test script. The older all-desktop
release matrix below describes historical checks, not the current default set.

## Storage/output and build-log follow-up

The desktop Builder now exports a standalone `ActRaiserRecomp/` folder beside
its outer artifact, independent of workspace storage. Its game marker selects
`.`; Workshop runtime edits target that output. Global storage shares the
`ActRaiserRecomp` parent with `installer/workspace` and `game` children.

- A freshly built real macOS launcher packaged and ran a synthetic C executable
  in a signed `.app`. Moving its entire portable folder and removing original
  build inputs still worked; moving only the renamed `.app` selected the new
  global game directory and seeded its runtime resources.
- Tests cover outer `.app` discovery, output selection independent of sidecars,
  explicit overrides/translocation refusal, shared OS namespaces, retained
  edits/deletions, symlink rejection, and non-destructive old-global-data import.
- Installer and shell internal/command regression suites pass with race checks;
  compiler tests include real Zig compilation and the timed activity reporter.
  The full session log retains content beyond the browser's bounded tail.
- macOS shell and Windows x86-64/ARM64 shell compile checks passed. Windows
  folder data initialization now uses an exclusive Windows file handle; it
  cross-compiles but still needs native runtime testing.

The full release and Linux VM runs below **predate this follow-up**. The new
storage contract has now passed the Linux player script described above; Windows
runtime acceptance remains outstanding. The macOS check here uses a synthetic
executable, not another full ROM-to-game compilation.

## macOS-native cross-build path

This historical release run included desktop artifacts alongside the original
installer archives. It is build-path coverage, not blanket runtime release
qualification. No CI was added, and no Linux/Windows program is executed by
the cross-build recipe.

**The complete root `make release` run passed on macOS ARM64**, producing all
six desktop artifacts plus the seven existing installer archives in `release/`.
The default cleanup removed per-target build trees and CPack staging while
retaining dependency caches. No SDK volume/lock remained. Steam Deck was
archive-only during that earlier run; see the candidate follow-up above.

- macOS ARM64 host: Go 1.26.3, host Zig 0.16.0, Apple clang/SDK, Wails 2.15.0,
  native SquashFS tools 4.7.5. Linux uses checked-in Debian 13 package locks,
  WebKitGTK 2.52.6 and a glibc 2.41 target baseline.
- Both Linux shells cross-compiled and linked on macOS, including real
  GTK/WebKit CGO dependencies. The SDK uses a native case-sensitive APFS
  sparse image because Linux headers contain case-distinct names. This is
  storage, not a VM. SHA-256-pinned Debian archives are extracted as data;
  package programs and installation scripts are never executed.
- macOS ARM64 and x86-64 `.app.zip` files produced by `make release`, extracted
  on macOS, and passed deep/strict signature verification. The shell and
  installer helper match each archive's target architecture. All 20,342
  payload file hashes per architecture matched after ZIP extraction.
- Both Linux AppImages produced by `make release` and extracted with native
  macOS `unsquashfs`: all 20,361 installer payload hashes and 1,424 runtime
  receipt hashes per architecture matched. All 377 runtime-receipt ELFs per
  artifact matched its target, as did the shell. An ELF search-path audit found RUNPATH entries and no legacy
  RPATH entries. Runtime receipts identify the Darwin ARM64 packaging host
  without embedding the maintainer's local paths.
- A Mac-built ARM64 test-only AppImage passed offline clean-player Debian 13
  checks: portable workspace, relocation/warm start, fresh global workspace,
  extract-and-run, FUSE launch, unchanged artifact, embedded JavaScript/API,
  preferences/tabs, PDF bytes, WAV decoding and multipart validation. This VM
  supplied only the runtime acceptance environment; it did not create the
  Builder AppImage. The OS still supplies glibc, graphics interfaces and FUSE.
- SDK/version/dependency/path and native SquashFS round-trip regression tests
  pass, including architecture mismatches and refusal to overwrite an image.
  The shell's internal/command packages pass race tests; vet passes. An
  intentionally mixed-architecture macOS payload was rejected before creating
  output. Preflight rejects unknown targets; the CMake orchestrator configures
  in both desktop and archive-only modes.
- Automatic SDK volume creation, successive architecture builds using the same
  cached image, and detach/lock cleanup passed. A deliberate failing subprocess
  also detached the image and removed the lock/mountpoint while preserving cache.
- Both Windows `.exe` artifacts produced by the root release command and
  extracted/verified on macOS: 20,904 files for x86-64 and 21,104 for ARM64,
  matching GUI PE architectures and pinned Fixed WebView2 metadata. All six
  desktop release SHA-256 sidecars verified. Windows execution is still untested.
- The existing installer suite passed again after release integration.
- The Mac-created ARM64 test-only Builder completed a full offline private-ROM
  upload, regeneration, compilation and playable game AppImage build in the
  clean Debian 13 VM. It used the bundled compiler/SDL SDK, 4 GiB RAM / 2 vCPUs
  and `--jobs 1`; no system Go/CMake/compiler/GTK/WebKit/SDL3 or network access.
  The embedded-page probe reported its explicit full-build PASS before exit.
- That newly generated game then passed 60-frame headless launches in portable
  mode and in a renamed, sidecar-free copy using isolated OS-global storage,
  still offline. Both path-selection probes and process exits passed. Private
  game data remains only in the disposable test workspace, never in release files.

The Zig/Debian headers emit identical 64-bit integer-constant macro
redefinition warnings during CGO compilation; linking succeeds. The headless
Linux VM reports GPU and optional GStreamer video/subtitle warnings. These
checks do not qualify accelerated rendering, audible playback, native dialogs
or human visual parity.

Cross-build logs and private validation outputs are under ignored
`build-builder-shell/`: `make-release-macos.log`, `mac-cross-linux-player.log`,
`mac-cross-linux-full-game.log`, `mac-cross-linux-game-launch.log`,
`cross-path-tests.log`, and `release-verify/`.

## Passed

- Existing installer: `go -C installer test ./...`, including the new readiness
  callback's real-loopback lifecycle checks and private descriptor tests.
- Shell host tests with the race detector on macOS ARM64 and Debian 13 ARM64:
  checksums, edit preservation, unsafe workspace rejection, sidecar/global
  selection, locking, session URL validation and HTTP request forwarding.
  Additional macOS race tests cover private renderer URL gating, binary HTTP
  bodies, warm-start transport, GUI environment isolation and narrow WebKit
  path relocation.
- macOS ARM64 `.app` assembled from a fresh native CMake installer staging tree.
  Ad-hoc signature verified with `codesign --verify --strict`. The window
  executable's linked libraries are Apple system libraries/frameworks; no
  Homebrew library linkage was present.
- Launching the packaged macOS app prepared a separate workspace and started
  its exact bundled backend, without opening an external browser. Status,
  Workshop HTML and bundled manual endpoints returned successfully.
- Test-only embedded renderer checks passed on both macOS ARM64 and Linux
  ARM64: existing interface/file-input JavaScript, buildable status, preference
  POST, tab navigation, PDF bytes, generated PCM WAV decoding/duration, empty
  multipart upload rejection, and clean shutdown through the backend.
  The WAV test initially failed over Wails' custom scheme. Serving the Workshop
  over a private token-authenticated HTTP listener fixed it on both platforms;
  a warm-start regression check also passed.
- The packaged macOS renderer probe also passed with an adjacent `.portable`
  sidecar selecting `BuilderData`, launched from an unrelated working directory.
  The app's signature remained valid afterward. OS-global path selection was
  unit-tested with isolated environment settings; live tests used disposable
  workspaces rather than the user's real application-data directory.
- Linux build: Go 1.26.7, GTK 3.24.49, WebKitGTK 2.52.6, Wails v2.15.0 with
  `production,webkit2_41`. Tests ran under a session bus and Xvfb. The packager
  adds no sandbox-disabling settings; actual WebKit process sandbox enforcement
  has not been qualified. The VM has no usable GPU acceleration; EGL warnings
  occurred, but the renderer probe passed.
- Linux ARM64 normal and test-only Builder AppImages produced using the
  existing payload's AppImage tool and runtime, with GTK/WebKit helpers,
  resources, and GStreamer modules bundled. Native packaging was exercised
  through `cmd/package` using a clean cross-staged installer payload. The new
  full `package-linux.cmake` entrypoint has not yet been run end-to-end on Linux.
- A maintainer-VM `strace` of the bundled AppDir confirmed both WebKit network
  and web processes executed from the relocated packaged helper paths, not
  `/usr`. The runtime receipt records package/source versions, file hashes,
  notices and the exact WebKit modifications.
- Clean-player Debian 13 ARM64, without system Go, CMake, C/C++ compilers,
  GTK3, WebKitGTK or SDL3: a test-only AppImage accepted the private ROM as a
  Blob-backed multipart request from its actual embedded page, regenerated the
  game, compiled it with its bundled toolchain/SDL SDK, and produced a game
  AppImage. The VM had 4 GiB RAM / 2 vCPUs, used `--jobs 1`, and had only
  loopback networking inside a network namespace. Xvfb/xauth/session D-Bus
  supplied the display test harness. The generated game then launched for 60
  headless frames in both portable and isolated global modes, including paths
  containing spaces and a renamed copy without the game sidecar.
- This test exposed a WebKit profile/global-workspace collision. Wails sets
  `ProgramName` after creating the WebKit context, too late to avoid the
  collision. Setting GLib's program name in `framework_linux.go` before Wails
  initializes fixes fresh global preparation; a native renderer probe confirmed
  separate `ActRaiserRecompBuilder` and `ActRaiserRecompBuilderWebView` directories.
- macOS transfer metadata found in the cross-staged test payload was removed
  by restaging, and `WriteManifest` now rejects `._*` / `.DS_Store` inputs with
  regression coverage. The full-build test predates that metadata cleanup and
  profile initialization fix; its compiler, SDL SDK and build pipeline are
  unchanged by those fixes.
- Final corrected AppImage: the clean-player script passed fresh portable
  workspace selection, warm startup after moving the artifact/sidecar/workspace
  together, fresh global workspace selection without the sidecar, and an
  unchanged artifact checksum. All three modes exercised JavaScript, status,
  preferences, tab navigation, PDF bytes, WAV decoding and multipart validation
  over extract-and-run. A fourth probe passed using an actual FUSE mount.
  These checks ran offline without system GTK/WebKit/SDL3 or build tools.
  The minimal VM initially lacked `fusermount`; mounted startup passed after
  installing Debian's standard `fuse3` package (3.17.2-3). Extract-and-run
  required no FUSE helper. This OS prerequisite is documented, not bundled or
  silently downloaded by the Builder.
- Windows `amd64` and `arm64` GUI-subsystem PE executables cross-compiled on
  macOS with CGO disabled and `production,wv2runtime.error`. This proves the Go
  shell compiles for Windows, not that Windows runtime behavior is validated.
- Windows x64 and ARM64 self-contained `.exe` packages assembled directly on
  macOS ARM64 through `package-windows.cmake`, using freshly staged installer
  payloads and SHA-256-verified Microsoft Fixed WebView2 152.0.4191.62 CABs.
  SDL3 3.4.16 and SDL3_ttf 3.2.2 are bundled. The x64 compiler is Zig 0.16.0;
  ARM64 retains the existing Windows-specific Zig 0.17.0-dev.1413+addc3c3b8 pin.
  No Windows VM, CI, Node or Electron was used to produce these artifacts.
- Both final Windows packages were extracted into fresh paths containing spaces
  on macOS and every extracted file was hash-verified: 20,904 files for x64 and
  21,104 for ARM64. The final PE headers identify the correct GUI subsystem and
  target architecture. The CMake recipe also rejected a deliberately wrong CAB
  before creating its staging tree. These are archive/packaging checks, not
  Windows launch, Windows filesystem or sandbox validation.
- Host-side Windows bundle race tests pass for both target architectures:
  round-trip extraction, cached-byte checks, unsafe/device/ADS names,
  case collisions, file/directory collisions, symlinks, corrupt archive bounds,
  PE header overlap, checksums, mixed architectures, ROM rejection and refusal
  to overwrite existing files. A synthetic certificate-table layout test
  passes; it is explicitly not a valid Authenticode signature test.
- The installer suite and `snesrecomp-go` suite pass on macOS after the Windows
  background-child-process changes. The compiler diagnostic test initially
  hit a sandbox-denied default Zig cache; rerunning with the writable dedicated
  `ZIG_GLOBAL_CACHE_DIR` passed. Windows-only bootstrap/ACL and console-policy
  tests compile to x64 test executables; native execution is still pending.

The refreshed macOS app is approximately 484 MiB including the existing
toolchain payload; the shell executable itself is about 9.5 MiB.
The metadata-clean Linux ARM64 AppImage is approximately 222 MiB compressed.
Windows is approximately 510 MiB x64 / 492 MiB ARM64 compressed; runtime/tools
extraction plus the separate writable SDK consume additional disk space.
Copying the build payload to the writable workspace consumes additional disk
space. The Linux packager still relies on host glibc and graphics interfaces:
this Debian 13 / glibc 2.41 build is not a portable-to-older-distros claim.

Updated artifacts from the private-HTTP transport checks are under:

- `build-builder-shell/macos-http/ActRaiserRecompBuilder.app` (normal).
- `build-builder-shell/http-smoke/ActRaiserRecompBuilder.app` (test-only).
- `build-builder-shell/linux-arm64-validated/ActRaiserRecompBuilder.AppImage` (normal).
- `build-builder-shell/linux-arm64-validated/ActRaiserRecompBuilder-smoke.AppImage` (test-only).
- `build-builder-shell/linux-arm64-validated/linux-runtime.json` (runtime provenance).
- `build-builder-shell/linux-player-results/` (full-build, game-launch and
  final Builder storage/mount-mode logs).
- `build-builder-shell/windows-amd64-final/ActRaiserRecompBuilder.exe` (normal, unsigned).
- `build-builder-shell/windows-arm64-final/ActRaiserRecompBuilder.exe` (normal, unsigned).

These build outputs are ignored by Git. The normal macOS app's signature and
Apple-only library/framework linkage were rechecked after the transport change.

## Not yet verified / not yet produced

- Native Windows execution, WebView2 extraction/profile/ACL behavior, console
  suppression for the entire compiler process tree, Windows signing/resources.
- Native Linux x86-64 execution and older-distro/Steam Deck GUI compatibility;
  native-Linux maintainer CMake packaging from configure through final Builder
  AppImage. Mac-host CMake cross-packaging of both Linux architectures passed.
- Windows first-run progress/cancellation before the webview exists; extraction
  currently finishes before showing the window. The new Windows player harness
  is prepared but has not run, and PowerShell syntax is not host-validated.
- Human visual inspection and native file-picker checks: the macOS screen was
  locked and the user was away. Programmatic DOM checks are not visual QA.
- Embedded PDF rendering, new-window links, native downloads/save dialogs,
  audible preview playback, scenery/level editor GPU behavior and accessibility.
- Full webview-driven game builds on macOS/Windows; the new end-to-end full
  build above was Linux ARM64. Memory-aware default worker selection is still
  needed; the low-memory VM test explicitly selected one worker.
- Fully portable browser profiles: the sidecar relocates the build/project
  workspace, but macOS/Linux browser-engine files can still use OS data/cache
  locations. Automatic double-click fallback on a Linux desktop without FUSE
  is not implemented; the explicit extract-and-run fallback is verified.
- Upgrade/migration/repair, cancellation during payload copying, untrusted
  filesystem races, release notices, publisher signing and notarization.

Use the [README](README.md) for repeatable commands and the remaining release
gates. Never distribute a `smoketest` build or treat a compile probe as the final
player artifact.
