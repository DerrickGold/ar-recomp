# Root convenience targets for producing the distributable game bundles.
#
# `make release` cross-builds every platform's self-contained bundle and
# writes them (plus SHA-256 sidecars) into ./release/. Bundles are named
# actraiser-recomp-<platform>.{tar.xz,zip}. Requires Go and CMake; the C
# toolchain and SDL2 are downloaded and bundled by the packaging project, so
# no compiler/SDL install is needed to PRODUCE the bundles.
#
# The equivalent pure-CMake command (run from the packaging directory) is:
#   cd snesrecomp-go/packaging && cmake --workflow --preset release
# Individual platforms: `make release-macos-arm64`, etc.
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
PLATFORMS := macos-arm64 macos-x86_64 linux-x86_64 linux-arm64 windows-x86_64 windows-arm64
ROM ?= ar.sfc

# Regenerable artifacts, grouped. Never lists the ROM, saves/*.srm, recordings,
# or authored source; only the specific generated sidecars inside saves/.
CLEAN_BUILD_DIRS := build build-release build-asan build-trace $(PACKAGING)/build snesrecomp-go/build
CLEAN_GENERATED  := src/gen recomp/funcs.h saves/gen_meta.json saves/rts_webs.txt saves/rts_webs.prev.txt
CLEAN_RELEASE    := release

.PHONY: dev release $(addprefix release-,$(PLATFORMS)) clean clean-all clean-release

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

clean:
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

clean-release:
	rm -rf release $(PACKAGING)/build
