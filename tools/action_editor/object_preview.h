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
/* Indices and owner addresses identify this captured pose only. Saved rules
 * target the canonical source family (optionally parent/animation), not an
 * actor pool slot or a claimed whole-level placement. */
typedef struct EditorObjectPreviewItem {
  unsigned index;
  uint16_t owner,source,parent,animation;
  uint8_t priority,shape,mode,owner_tag;
  float capture_x,capture_y,width,height,offset,thickness,width_scale;
  bool can_extrude,active,visible,player;
} EditorObjectPreviewItem;
unsigned EditorObjectPreview_ItemCount(const EditorObjectPreview *);
bool EditorObjectPreview_Item(const EditorObjectPreview *,const DioramaScene *,
    unsigned index,EditorObjectPreviewItem *);
/* Exact native winner hit, in the imported capture's pixel coordinates.
 * Metadata-only actors have no owner mask and remain selectable in the list. */
bool EditorObjectPreview_ItemPixel(const EditorObjectPreview *,unsigned index,
    int capture_x,int capture_y);
/* Independent composition alpha, including pixels originally occluded by
 * another native object. Use this for the modeled front/rear face observer. */
bool EditorObjectPreview_ItemInk(const EditorObjectPreview *,unsigned index,
    int capture_x,int capture_y);
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
