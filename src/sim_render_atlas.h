#ifndef SIM_RENDER_ATLAS_H
#define SIM_RENDER_ATLAS_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

typedef struct Ppu Ppu;

enum {
  kSimObjAtlasWidth = 512,
  kSimObjAtlasHeight = 512,
  kSimObjAtlasPitch = kSimObjAtlasWidth * 4,
};

/* Game-thread-owned until PresentUpload completes under the existing frame
 * handshake. The texture is intentionally presentation-owned elsewhere. */
extern uint32_t g_sim_obj_atlas_pixels[
    kSimObjAtlasWidth * kSimObjAtlasHeight];

/* Builds and atomically publishes atlas descriptors for the current semantic
 * producer. Failure is committed as an integrity flag so the frame selects
 * the complete authentic fallback instead of using a partial atlas. */
bool SimRenderAtlas_Build(Ppu *ppu, uint16 camera_x, uint16 camera_y);

#endif  /* SIM_RENDER_ATLAS_H */
