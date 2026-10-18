#ifndef AR_EDITOR_RECEIVER_PREVIEW_H
#define AR_EDITOR_RECEIVER_PREVIEW_H

#include "action/action_effect_receivers.h"
#include "diorama/diorama.h"

/* Temporary editor stand-ins, never actors or saved project data. The player
 * packet contains the common OBJ bank, its three palettes, and idle picture.
 * Decode once with the production PPU; subsequent frames only tint the pixels. */
bool EditorReceiverPreview_Load(const uint8_t *bytes, size_t size);
bool EditorReceiverPreview_Ready(void);
void EditorReceiverPreview_Reset(void);

typedef struct EditorReceiverPreviewSettings {
  unsigned mask; /* bit 0: player, bit 1: same artwork as enemies/objects */
  int x[2], feet_y[2];
  bool flipped;
} EditorReceiverPreviewSettings;

/* Populate the ordinary OBJ-2 capture, letting the shared compositor own depth,
 * clipping, filtering and rim lighting. The caller owns the texture's lifetime.
 * Only changed rectangles upload; orbit/output changes reuse the same pixels. */
bool EditorReceiverPreview_Capture(ArRenderDevice *device,
    const EditorReceiverPreviewSettings *settings,
    const ActionReceiverLighting *lighting, int camera_x, int camera_y, int extra,
    const DioramaCapture *capture, ArRenderTexture *texture,
    const uint8_t **pixels, unsigned *uploads);

/* Invert the published OBJ projection for click/drag placement. Returns capture
 * coordinates, including the extended rows, with no parallel camera model. */
bool EditorReceiverPreview_Pick(const DioramaProjection *projection,
    float screen_x, float screen_y, ArRenderPointF *capture_point);

#endif
