#ifndef AR_EDITOR_OBJECT_PREVIEW_H
#define AR_EDITOR_OBJECT_PREVIEW_H
#include "diorama/diorama.h"
#include "diorama/diorama_object_snapshot.h"

/* Finite native captures, never an inferred whole-room actor inventory. */
typedef struct EditorObjectPreview {
  DioramaObjectSnapshot snapshot;
  uint32_t *pixels;
  ActionVoxelSpriteFrame props;
  ArRenderTexture atlas;
  bool atlas_dirty;
} EditorObjectPreview;
bool EditorObjectPreview_Load(EditorObjectPreview *, const uint8_t *, size_t,
                             unsigned group, unsigned room, unsigned terrain);
/* Consumes the already-decoded packet only on success. The caller has checked
 * camera-local section identity before committing this native frame. */
bool EditorObjectPreview_Adopt(EditorObjectPreview *, DioramaObjectSnapshot *);
void EditorObjectPreview_Reset(EditorObjectPreview *, ArRenderDevice *);
bool EditorObjectPreview_Capture(EditorObjectPreview *, ArRenderDevice *,
    DioramaCapture *, const DioramaView *, DioramaScene *, bool voxel,
    ArRenderTexture textures[kDioramaPlane_Count],
    const uint8_t *pixels[kDioramaPlane_Count], unsigned *uploads);
#endif
