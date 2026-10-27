#ifndef AR_ACTRAISER_CAMERA_INTERNAL_H
#define AR_ACTRAISER_CAMERA_INTERNAL_H

#include "diorama/diorama_layer_order.h"
#include "actraiser/actraiser_action_camera.h"

/* Narrow camera/capture seam. Keeping generated-bank declarations out of this
 * header lets the production camera be exercised in ROM-free builds. */
const DioramaRoomOverride *ActRaiser_CurrentVirtualLayerRoom(void);
bool ActRaiser_DioramaNeedsBgApron(const DioramaRoomOverride *room,
    unsigned bg, bool pixel_edits, bool world_available);

#endif
