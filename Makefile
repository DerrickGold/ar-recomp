# Root convenience targets for producing the distributable game bundles.
#
# `make release` cross-builds every platform's self-contained bundle and
# writes them (plus SHA-256 sidecars) into ./release/. Bundles are named
# actraiser-recomp-<platform>.{tar.xz,zip}. Requires Go and CMake; the C
# toolchain and supported SDL3 redistributables are downloaded and bundled by
# the packaging project, so no compiler/SDL install is needed to PRODUCE the
# bundles.
#
# The equivalent pure-CMake command (run from the packaging directory) is:
#   cd snesrecomp-go/packaging && cmake --workflow --preset release
# Individual platforms: `make release-macos-arm64`, `make release-steam-deck`,
# etc.
#
# Each platform's CMake build tree (which holds a freshly extracted ~180 MB Zig
# toolchain) is removed as soon as that bundle is staged into release/, so the
# large intermediate build data does not accumulate. The download cache
# (snesrecomp-go/packaging/cache) is kept so re-runs need no re-download. Pass
# KEEP_BUILD=1 to retain the per-platform build trees for debugging.
#
# Local development:
#   make dev          bootstrap a runnable optimized build from a clean or fresh
#                     tree in one step: regenerate the C (only if missing),
#                     configure, and build the `play` preset. Use this after a
#                     `make clean` or a fresh clone. Override the ROM with
#                     `make dev ROM=path.sfc`.
#   For the normal inner loop (after editing src/ or runtime C) just run
#   `cmake --build --preset play` directly — no regen or reconfigure needed.
#
#   make check-constants  reject high-risk duplicate literals in authored code.
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

PACKAGING := snesrecomp-go/packaging
PLATFORMS := macos-arm64 macos-x86_64 linux-x86_64 linux-arm64 windows-x86_64 windows-arm64 steam-deck
ROM ?= ar.sfc

# Regenerable artifacts, grouped. Never lists the ROM, saves/*.srm, recordings,
# or authored source; only the specific generated sidecars inside saves/.
CLEAN_BUILD_DIRS := build build-release build-asan build-trace $(PACKAGING)/build snesrecomp-go/build
CLEAN_GENERATED  := src/gen recomp/funcs.h saves/gen_meta.json saves/rts_webs.txt saves/rts_webs.prev.txt
CLEAN_RELEASE    := release

.PHONY: dev release $(addprefix release-,$(PLATFORMS)) check-constants check-cross clean clean-all clean-release clean-packaging-mounts

check-constants:
	@sh tools/check_constants.sh

dev:
	@if [ -z "$$(ls src/gen/*.c 2>/dev/null)" ]; then \
	  echo "=== regenerating (src/gen is empty) ==="; \
	  go -C snesrecomp-go run ./cmd/snesbuild regen --root .. --rom $(ROM) --allow-stubs; \
	fi
	@[ -f build-release/CMakeCache.txt ] || cmake --preset play
	cmake --build --preset play
	@echo "Built ./build-release/ActRaiserRecomp — run it with: ./build-release/ActRaiserRecomp $(ROM) --config config.ini"

release:
	@for p in $(PLATFORMS); do \
	  echo "=== packaging $$p ==="; \
	  ( cd $(PACKAGING) && cmake --workflow --preset package-$$p ) || exit 1; \
	  [ -n "$(KEEP_BUILD)" ] || rm -rf $(PACKAGING)/build/$$p; \
	done
	@rm -rf release/_CPack_Packages
	@[ -n "$(KEEP_BUILD)" ] || rm -rf $(PACKAGING)/build
	@echo "Bundles written to $(CURDIR)/release/"

$(addprefix release-,$(PLATFORMS)): release-%:
	( cd $(PACKAGING) && cmake --workflow --preset package-$* )
	@rm -rf release/_CPack_Packages
	@[ -n "$(KEEP_BUILD)" ] || rm -rf $(PACKAGING)/build/$*
	@echo "Bundle written to $(CURDIR)/release/"

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
