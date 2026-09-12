# Read-only Builder inputs and writable game data

Implemented layout, 2026-09-12. See [verification](VERIFICATION.md) for what has
actually been tested. Existing public artifacts need rebuilding. No CI,
filesystem driver or compiler virtual filesystem is used.

## Storage contract

- **Application payload:** authored source, recompiler configuration, runtime
  SDK, compiler, SDL SDK and build/packaging programs. Read/execute in place in
  the macOS `.app` or Linux AppImage mount. Frontend and embedded runtime seed
  files are read from the backend executable.
- **Private Builder workspace:** ownership/session markers, logs, generated C
  and declarations, metadata/reports, objects, intermediate binaries and
  compiler caches. No authored project or toolkit copy. Portable/global storage
  selection follows the existing Builder sidecar rules.
- **Player game directory:** final `.app`, `.AppImage` or Windows executable
  with runtime dependencies, ROM, assets, settings/configuration, saves,
  notices and small initialization/import records. Native apps contain their
  archive helper; folder outputs keep that runtime-only helper in `tools/`.
  No duplicate loose executable or run-game script accompanies a native app.
  Existing app backups are retained when replacing a build.

The game directory is the deliverable; compilation files/caches are not part of
it. App resources include seed assets and a ROM so copying only the app still
supports global startup. Portable outputs retain editable data beside the app.
Browser-engine profiles and temporary OS packaging files are additional
implementation state, not zero-disk or fully portable-webview guarantees.

**Windows exception:** an EXE's appended archive is not an executable filesystem
for native compiler tools or Fixed WebView2. Windows still extracts a verified,
versioned runtime cache. Source/tools are read from that cache, without a second
copy in the workspace. Avoiding that first extraction needs a different Windows
distribution/runtime model.

## Startup, build and upgrades

1. `host.PrepareSession` validates the payload manifest/platform and hashes
   every listed input, with cancellation between chunks. It creates only a
   `.builder-workspace` marker. Verification still reads files; this does not
   imply a measured startup-speed improvement.
2. `StartBundledBackend` runs the backend and sibling `snesbuild` inside
   `payload/utils/tools`, using the payload as `--root` and a writable CWD.
   Runtime data seeds from the payload/embedded bytes to the game directory,
   preserving player changes.
3. Builds use `workspace/build/<payload-SHA256>/<ROM-SHA256>/`. Generated C,
   `funcs.h`, metadata, RTS reports, native products and explicit Zig local/global
   caches all go there. Tools are discovered beside bundled executables; a
   missing compiler fails without downloading.
4. External generated declarations precede any old bundled `funcs.h`. Cache
   keys include verified input identity and flags; bundle-relative normalization
   avoids temporary AppImage mount paths. Generated source/header changes still
   invalidate objects; unchanged declarations retain their timestamp.
5. Publication copies the game runtime, not the build tree, then creates its
   portable sidecar. The installed font-coverage protocol remains accessible
   through an AppImage without a loose game executable.

A newer Builder reads its own source/tools and chooses its own derived cache.
It does not reuse copied tools or require a workspace reset because the payload
changed. Recognized legacy `.builder-payload` workspaces are accepted without
modifying their stamp/files. Old source edits are not merged into the new
bundle; import runtime data through the installation-import UI. Unmanaged
directories are still refused. Replacing the Builder does not rebuild an
existing game: run Build to generate its update, preserving editable data.

The old destructive “keep only the game” action is disabled in read-only desktop
sessions so it cannot delete bundled inputs. Dedicated private-cache cleanup
and recovery/storage UI remain future work. Legacy generic Linux archives keep
their existing install/cleanup flow. Old workspaces/caches are not auto-deleted.

## Verification

Normal regression tests need no private ROM:

```sh
go -C installer test ./...
go -C installer/desktop-shell test ./internal/... ./cmd/...
go -C snesrecomp-go test ./internal/project ./internal/tooling ./cmd/snesbuild
```

`TestImmutableBuildReadOnlyRelocationAndInvalidation` uses local Zig to build a
tiny ROM-free game against chmod-read-only source, moves the input bundle, then
checks caching and header/input-identity changes. Set `SNESBUILD_ZIG` if Zig is
not in the development cache or PATH.

The opt-in macOS test uses the actual backend HTTP build flow: cold/warm builds,
preserved settings, restart detection, relocation, font probing and bounded
headless launch after the backend exits:

```sh
AR_TEST_BUNDLED_PAYLOAD=/absolute/test/Builder.app/Contents/Resources/payload \
AR_TEST_BUNDLED_ROM=/absolute/private/ActRaiser.sfc \
  go -C installer/desktop-shell test ./internal/host \
  -run '^TestBundledPlayerBuildColdWarmAndIndependentLaunch$' -v -count=1 -timeout 20m
```

Use a freshly packaged, disposable test app. `chmod -R a-w /absolute/test/Builder.app`
before testing enforces the input boundary. The test owns/removes its temporary
build/game directories and never embeds the ROM in the Builder. Normal tests
skip this gate without both variables. Native Linux/Windows checks remain
necessary; player scripts now assert marker-only workspaces instead of copied
`utils/tools`. Cross-compilation is not runtime evidence.
