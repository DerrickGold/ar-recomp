#ifndef AR_ACTRAISER_CAMERA_INTERNAL_H
#define AR_ACTRAISER_CAMERA_INTERNAL_H

#include "diorama/diorama_layer_order.h"

/* Narrow camera/capture seam. Keeping generated-bank declarations out of this
 * header lets the production camera be exercised in ROM-free builds. */
const DioramaRoomOverride *ActRaiser_CurrentVirtualLayerRoom(void);
bool ActRaiser_DioramaNeedsBgApron(const DioramaRoomOverride *room,
    unsigned bg, bool pixel_edits, bool world_available);
/* Last resolved travel, valid only for the current room and native width. */
bool ActRaiser_ActionCameraTravel(uint16_t *minimum, uint16_t *maximum);

#endif
