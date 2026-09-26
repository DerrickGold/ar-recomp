#ifndef AR_SCENE_ASSET_DUMP_H
#define AR_SCENE_ASSET_DUMP_H
/* SceneAssetDump: writes a point-in-time package of the resident SNES scene
 * (PPU state, VRAM, CGRAM, OAM, WRAM) into a new directory while emulation
 * is paused, for offline asset work.
 * Phase: developer tools (emulation paused).
 * Tests: tests/scene_asset_dump_test.c */

#include <stdbool.h>
#include <stdint.h>

#include "snesrecomp/runner.h"

typedef struct SceneAssetDumpSource {
  SrPpuStateSnapshot ppu;
  SrBorrowedU16Span vram;
  SrBorrowedU16Span cgram;
  SrBorrowedU16Span oam;
  SrBorrowedSpan high_oam;
  SrBorrowedSpan wram;
} SceneAssetDumpSource;

/* Write a complete, point-in-time asset package from resident SNES state.
 * `directory` is created by this function and must have an existing parent.
 * Every span is borrowed from one runner generation. The caller must invoke
 * this synchronously while emulation is paused. */
bool SceneAssetDump_Write(const char *directory,
                          const SceneAssetDumpSource *source,
                          int host_frame);

bool WritePng(const char *path, const uint8_t *rgba, int width, int height);

#endif
