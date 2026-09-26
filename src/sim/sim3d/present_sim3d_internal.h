#ifndef AR_PRESENT_SIM3D_INTERNAL_H
#define AR_PRESENT_SIM3D_INTERNAL_H
/* Private town compositor helpers. Shared town/globe atmosphere is declared
 * separately in present_sim3d_environment.h; feature entry points use
 * present_sim3d.h. Inputs are captured values, never live game state. */

#include "present/present.h"
#include "render/render_types.h"

int InsertSimGroundCoordinate(float *coordinates, int count, int capacity,
                              float coordinate);
void DrawSimBackdrop(const FrameSlot *slot, ArRenderRectI viewport,
                     const float matrix[16]);
#endif
