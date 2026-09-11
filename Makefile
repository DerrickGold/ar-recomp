# Root convenience targets for producing desktop and generic Linux Builders.
#
# `make release` cross-builds every platform's self-contained bundle and
# writes them (plus SHA-256 sidecars) into ./release/: macOS .app.zip, Windows
# .exe, Steam Deck .AppImage, and generic Linux .tar.xz. Requires Go and CMake; the C
# toolchain and supported SDL3 redistributables are downloaded and bundled by
# the packaging project. Desktop releases also need native host packaging tools;
# macOS can cross-build all release targets without a VM.
# See installer/desktop-shell/README.md for prerequisites and validation limits.
# DESKTOP=0 explicitly requests legacy archives instead of the default matrix.
# Steam Deck enforces glibc 2.36; full device/game qualification is separate.
#
# The equivalent pure-CMake command (run from the packaging directory) is:
#   cd installer/packaging && cmake --workflow --preset release
# Individual platforms: `make release-macos-arm64`, `make release-steam-deck`,
# etc.
#
# Each platform's CMake build tree (which holds a freshly extracted ~180 MB Zig
# toolchain) is removed as soon as that bundle is staged into release/, so the
# large intermediate build data does not accumulate. The download cache
# (installer/packaging/cache) is kept so re-runs need no re-download. Pass
# KEEP_BUILD=1 to retain the per-platform build trees for debugging.
#
# Local development:
#   make dev          bootstrap a runnable optimized build from a clean or fresh
#                     tree in one step: regenerate the C (only if missing),
#                     configure, and build the `play` preset. Use this after a
#                     `make clean` or a fresh clone. Override the ROM with
#                     `make dev ROM=path.sfc`.
#   For the normal inner loop (after editing src/ or runtime C) just run
#   `cmake --build --preset play` directly. `make dev` always reapplies the
#   release preset first, so newly promoted feature defaults cannot remain
#   stale in an existing CMake cache.
#
#   make check-constants  reject high-risk duplicate literals in authored code.
#   make check-appimage   Linux-only, ROM-free finished AppImage acceptance.
#                     Uses the same packaging code and pinned tools shipped in
#                     the portable Builder; see docs/desktop-packaging.md.
#   make check-localization-roms  run the OPTIONAL five-ROM localization
#                     acceptance gate. `ctest` and a plain `go test` SKIP these
#                     cases when the regional ROMs are absent, so a green
#                     default run is not evidence that this gate ran. This
#                     target fails loudly instead of skipping. Override the ROM
#                     directory with `make check-localization-roms ROM_ROOT=...`.
#   make check-localization-workflow ACTRAISER_BUILDER=/path/to/actraiser-builder
#                     exercise a packaged Go extractor, editor import/export,
#                     real game font gate, same-locale installs and relocation.
#                     This does not replace clean full-game regeneration.
#   make check-cross  compile AND link the game for the platforms that cannot be
#                     tested on this machine (currently Windows x86_64), using
#                     the pinned Zig toolchain and the same SDL3 redistributable
#                     the bundle ships. Proves the build, not the run.
#
# Cleaning (these are a full reset, not part of the inner loop — `make clean`
# removes the generated C and build trees, so run `make dev` afterwards to get
# back to a buildable state):
#   make clean        remove every regenerable artifact (build trees, generated
#                     C, tool binaries, release bundles) — keeps the ROM, save
#                     files, source, and the downloaded dependency cache.
#   make clean-all    also remove the downloaded Zig/SDL cache (forces a
#                     re-download on the next `make release`).
#   make clean-release  remove only the packaged bundles + packaging build.

PACKAGING := installer/packaging
PLATFORMS := macos-arm64 macos-x86_64 linux-x86_64 linux-arm64 windows-x86_64 windows-arm64 steam-deck
DESKTOP ?= 1
ROM ?= ar.sfc

# Regenerable artifacts, grouped. Never lists the ROM, saves/*.srm, recordings,
# or authored source; only the specific generated sidecars inside saves/.
CLEAN_BUILD_DIRS := build build-release build-control build-terrain build-asan build-trace $(PACKAGING)/build snesrecomp-go/build installer/build
CLEAN_GENERATED  := src/gen recomp/funcs.h saves/gen_meta.json saves/rts_webs.txt saves/rts_webs.prev.txt
CLEAN_RELEASE    := release

.PHONY: dev release $(addprefix release-,$(PLATFORMS)) check-constants check-appimage check-cross check-localization-roms check-localization-workflow clean clean-all clean-release clean-packaging-mounts

check-constants:
	@sh tools/check_constants.sh

check-appimage:
	cmake -S $(PACKAGING)/appimage-check -B build-appimage-check
	cmake --build build-appimage-check --target check-appimage

# config.ini is gitignored -- it is the developer's live config, and the stock
# copy that ships is the packaging template. Seed it once so a fresh clone runs
# with the project's tuned base settings instead of the built-in fallbacks (a
# missing --config file is a silent no-op in ParseConfigFile). Never
# overwrites: this file is yours the moment it exists.
CONFIG_TEMPLATE := $(PACKAGING)/templates/config.ini

config.ini:
	@cp $(CONFIG_TEMPLATE) $@
	@echo "seeded $@ from $(CONFIG_TEMPLATE)"

dev: config.ini
	go -C installer run ./cmd/actraiser-builder native-source --root .. --rom $(ROM)
	@if [ -z "$$(ls src/gen/*.c 2>/dev/null)" ]; then \
	  echo "=== regenerating (src/gen is empty) ==="; \
	  go -C snesrecomp-go run ./cmd/snesbuild regen --root .. --rom $(ROM) --allow-stubs; \
	fi
	cmake --preset play
	cmake --build --preset play
	@echo "Built ./build-release/ActRaiserRecomp — run it with: ./build-release/ActRaiserRecomp $(ROM) --config config.ini"

RELEASE_OPTIONS = -DBUILDER_LEGACY_ARCHIVES=$(if $(filter 0,$(DESKTOP)),ON,OFF) -DBUILDER_KEEP_BUILD=$(if $(KEEP_BUILD),ON,OFF)

release:
	cmake "-DBUILDER_PLATFORMS=$(PLATFORMS)" $(RELEASE_OPTIONS) -P $(PACKAGING)/release.cmake

$(addprefix release-,$(PLATFORMS)): release-%:
	cmake -DBUILDER_PLATFORMS=$* $(RELEASE_OPTIONS) -P $(PACKAGING)/release.cmake

# Cross-target link check. `zig cc` carries libc headers and a linker for every
# target it supports, so the compile and the link are the real ones for that
# platform -- only the run is missing. That is enough to catch the whole class
# of breakage that is otherwise invisible from a Mac: #ifdef _WIN32 branches
# nothing has ever compiled, macro collisions with <windows.h>, and system
# libraries the link needs but nothing declares.
#
# Windows x86_64 is the only target listed because it is the only one with both
# an official SDL3 redistributable to link against and no other way to test it
# here. macOS is covered by building natively; Linux x86_64 is covered on the
# Steam Deck. To check any other triple by hand, stage or supply its SDL and run
#   snesbuild build --hermetic --target <triple> --sdl-include ... --sdl-lib ...
CROSS_TARGETS := x86_64-windows-gnu

check-cross:
	go -C installer run ./cmd/actraiser-builder native-source --root .. --rom $(ROM)
	@if [ -z "$$(ls src/gen/*.c 2>/dev/null)" ]; then \
	  echo "=== regenerating (src/gen is empty) ==="; \
	  go -C snesrecomp-go run ./cmd/snesbuild regen --root .. --rom $(ROM) --allow-stubs || exit 1; \
	fi
	go -C snesrecomp-go build -o build/snesbuild ./cmd/snesbuild
	@./snesrecomp-go/build/snesbuild toolchain fetch --root .
	@for t in $(CROSS_TARGETS); do \
	  echo "=== cross-checking $$t ==="; \
	  ./snesrecomp-go/build/snesbuild sdl stage --root . --target $$t \
	    --cache-dir $(PACKAGING)/cache || exit 1; \
	  ./snesrecomp-go/build/snesbuild build --hermetic --root . --target $$t || exit 1; \
	done
	@echo "Cross targets link cleanly: $(CROSS_TARGETS)"

# Optional localization acceptance that needs the five regional ROMs. These are
# never in source control, so the Go tests skip themselves without them; that
# skip is deliberately indistinguishable from a pass in `go test` output, which
# is exactly why this target exists. Run it before claiming five-ROM coverage.
ROM_ROOT ?= .
LOCALIZATION_ROMS := ar.sfc ar-eu.sfc ar-ger.sfc ar-fra.sfc ar-jp.sfc
LOCALIZATION_PROBE := build/actraiser_language_pack_runtime_test

check-localization-roms:
	@missing=""; for r in $(LOCALIZATION_ROMS); do \
	  [ -f "$(ROM_ROOT)/$$r" ] || missing="$$missing $$r"; \
	done; \
	if [ -n "$$missing" ]; then \
	  echo "missing regional ROMs in $(ROM_ROOT):$$missing"; \
	  echo "this gate cannot be reported as passed without them"; exit 1; \
	fi
	cmake --build build --target actraiser_language_pack_runtime_test actraiser_localization_art_test actraiser_localization_credits_test
	AR_LOCALIZATION_GUI_ROM_ROOT="$(abspath $(ROM_ROOT))" \
	AR_LOCALIZATION_BUILD_ROM="$(abspath $(ROM_ROOT)/ar.sfc)" \
	AR_AUTHOR_RUNTIME_PROBE="$(abspath $(LOCALIZATION_PROBE))" \
	AR_NATIVE_GRAPHICS_PROBE="$(abspath build/actraiser_localization_art_test)" \
	AR_CREDITS_RUNTIME_PROBE="$(abspath build/actraiser_localization_credits_test)" \
	  go -C installer test ./internal/builder ./internal/localization -count=1
	@echo "five-ROM localization acceptance ran with all $(words $(LOCALIZATION_ROMS)) ROMs and the C probe"

check-localization-workflow: check-localization-roms
	@test -n "$(ACTRAISER_BUILDER)" && test -x "$(ACTRAISER_BUILDER)" || \
	  { echo "set ACTRAISER_BUILDER to a freshly packaged actraiser-builder executable"; exit 1; }
	cmake --build build --target ActRaiserRecomp actraiser_input_replay_test
	AR_LOCALIZATION_GUI_ROM_ROOT="$(abspath $(ROM_ROOT))" \
	AR_LOCALIZATION_BUILD_ROM="$(abspath $(ROM_ROOT)/ar.sfc)" \
	AR_LOCALIZATION_BUILDER_PROBE="$(abspath $(ACTRAISER_BUILDER))" \
	AR_AUTHOR_RUNTIME_PROBE="$(abspath $(LOCALIZATION_PROBE))" \
	AR_AUTHOR_FONT_PROBE="$(abspath build/ActRaiserRecomp)" \
	AR_REPLAY_WRITER_PROBE="$(abspath build/actraiser_input_replay_test)" \
	  go -C installer test -race ./internal/builder \
	    -run 'TestLocalizationRelocatedBuilderGameWorkflow|TestHeadlessDebugStateRejectionExits|TestHeadlessReplayEndExitsBeforeSafetyCap' -count=1

clean-packaging-mounts:
	@/bin/sh "$(PACKAGING)/scripts/detach-macos-dmgs.sh" "$(abspath $(PACKAGING)/cache)"

clean: clean-packaging-mounts
	@removed=""; \
	for t in $(CLEAN_BUILD_DIRS) $(CLEAN_GENERATED) $(CLEAN_RELEASE); do \
	  if [ -e "$$t" ]; then echo "  rm $$t ($$(du -sh "$$t" 2>/dev/null | cut -f1))"; rm -rf "$$t"; removed=1; fi; \
	done; \
	[ -n "$$removed" ] || echo "  nothing to clean"; \
	echo "Kept: ROM, saves/*.srm, source, and $(PACKAGING)/cache (use 'make clean-all' to drop the download cache)."; \
	echo "Run 'make dev' to regenerate + rebuild a runnable local build."

clean-all: clean
	@if [ -e "$(PACKAGING)/cache" ]; then \
	  echo "  rm $(PACKAGING)/cache ($$(du -sh $(PACKAGING)/cache 2>/dev/null | cut -f1)) — Zig/SDL will re-download next release"; \
	  rm -rf "$(PACKAGING)/cache"; \
	fi

clean-release: clean-packaging-mounts
	rm -rf release $(PACKAGING)/build
