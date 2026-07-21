#ifndef SCENE_ASSET_DUMP_H
#define SCENE_ASSET_DUMP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Ppu Ppu;

/* Write a complete, point-in-time asset package from resident SNES state.
 * `directory` is created by this function and must have an existing parent.
 * The caller should invoke this while emulation is paused so PPU/WRAM state is
 * coherent for the entire synchronous export. */
bool SceneAssetDump_Write(const char *directory, const Ppu *ppu,
                          const uint8_t *wram, int host_frame);

bool WritePng(const char *path, const uint8_t *rgba, int width, int height);

#endif
