#ifndef AR_SIM_RENDER_ATLAS_H
#define AR_SIM_RENDER_ATLAS_H
/* SimRenderAtlas: packs the SIM town's objects into one 512 x 512 atlas each
 * frame and publishes where each object landed. A failed build flags the
 * frame, which then falls back to the authentic image.
 * Phase: capture (reads the runner).
 * Tests: tests/ppu_render_pipeline_test.c */

#include <stdbool.h>
#include <stdint.h>

#include "snesrecomp/runner.h"
#include "snesrecomp/game/types.h"

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
bool SimRenderAtlas_Build(SrRunnerHandle *runner,
                          uint16 camera_x, uint16 camera_y);

#endif  /* AR_SIM_RENDER_ATLAS_H */
