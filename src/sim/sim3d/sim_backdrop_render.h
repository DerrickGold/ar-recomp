#ifndef AR_SIM_BACKDROP_RENDER_H
#define AR_SIM_BACKDROP_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "render/render_types.h"

enum {
  kSimBackdropRenderMaxVertices = 6,
  kSimBackdropRenderMaxIndices = 12,
};

/* Backend-neutral inputs for the shared SIM-town/world-navigation sky. The
 * optional matrix supplies the real ground horizon; a NULL or off-screen
 * horizon selects the authored synthetic anchor. */
typedef struct SimBackdropRenderInput {
  uint32_t backdrop_argb;
  int strength_pct;
  int horizon_pct;
  ArRenderRectI viewport;
  const float *matrix;
} SimBackdropRenderInput;

typedef struct SimBackdropRenderBatch {
  ArRenderVertex2D vertices[kSimBackdropRenderMaxVertices];
  int32_t indices[kSimBackdropRenderMaxIndices];
  int vertex_count;
  int index_count;
} SimBackdropRenderBatch;

/* Builds at most two gradient quads. Invalid inputs fail closed with an empty
 * batch; successful output is ready for one untextured opaque device draw. */
bool SimBackdropRender_Build(const SimBackdropRenderInput *input,
                             SimBackdropRenderBatch *batch);

#endif  /* AR_SIM_BACKDROP_RENDER_H */
