#ifndef AR_DIORAMA_RENDER_OPTIONS_H
#define AR_DIORAMA_RENDER_OPTIONS_H
/* Explicit compositor policy. Borrowed resources remain valid for one draw;
 * callers own decoding, settings, persistence and backend resource lifetime. */

#include "diorama_layer_order.h"
#include "diorama_skybox_source.h"
#include "render/presentation_options.h"
#include "render/render_device.h"

typedef ArRenderTexture (*DioramaSkyboxResolver)(
    void *userdata, ArRenderDevice *device, int source,
    bool transparent_fill_configured, uint32_t transparent_fill_argb,
    bool *state_restore_failed);

typedef bool (*DioramaSkyboxDraw)(ArRenderDevice *, ArRenderTexture,
    const DioramaSkyboxSourceDraw *);
/* Optional native scope for consecutive effect-only callbacks. Begin records
 * no work; End submits before SDL rendering or target/viewport restoration.
 * Every successful Begin must be paired with End, including callback failure. */
typedef bool (*DioramaSourceEffectBatch)(ArRenderDevice *, bool begin);

/* Editor-only observation of the actual submitted surfaces. Native gameplay
 * never supplies a callback. Coordinates identify native map pixels, including
 * perspective-correct interpolation along caps and curved faces. */
typedef enum DioramaVoxelFaceKind {
  kDioramaVoxelFace_Front, kDioramaVoxelFace_Top, kDioramaVoxelFace_Side,
  kDioramaVoxelFace_Backing, kDioramaVoxelFace_Object,
  kDioramaVoxelFace_Left, kDioramaVoxelFace_Right, kDioramaVoxelFace_Bottom
} DioramaVoxelFaceKind;
typedef struct DioramaVoxelPickFace {
  bool authored;
  uint8_t art_index; /* Authored independent artwork page; ignored otherwise. */
  float art_uv[8]; /* Native 16px donor coordinates for alpha-aware picking. */
  int16_t art_source_x, art_source_y; /* Automatic cap's exact native donor. */
  uint8_t art_source_size; /* Zero means no square donor is represented. */
  float screen_xy[8], world_xy[8], depth[4], clip_w[4];
  int kind, priority;
  int plane; /* Native capture plane, or -1 for independently owned artwork. */
  int cell_x, cell_y; /* -1 uses interpolated world coordinates. */
  uint16_t owner; /* Captured object identity, valid only for this frame. */
  bool sample_alpha; /* Object cap/profile: world_xy maps independent source ink. */
} DioramaVoxelPickFace;
typedef void (*DioramaVoxelObserveFace)(void *userdata,
    const DioramaVoxelPickFace *face);

/* Transient editor inspection only; zero preserves the production draw. */
typedef enum DioramaVoxelInspection {
  kDioramaVoxelInspect_All,
  kDioramaVoxelInspect_Backing,
  kDioramaVoxelInspect_Cutaway
} DioramaVoxelInspection;

typedef struct DioramaRenderOptions {
  uint32_t visible_planes;
  DioramaSkyMode skybox;
  float depth_shade; /* 0..1 */
  bool hud_flat, shoebox, margin_fix;
  bool shadow_blur, rim_light, depth_of_field, edge_aa;
  bool stack_grouping, sparse_coverage, skybox_prefilter, priority_surface;
  bool waterfall_diagnostics; /* Optional desktop logging; no draw behavior changes. */
  /* NULL selects the production default layer order/shapes. */
  const DioramaLayerOrderTable *layers;
  /* Optional already-resolved capture/replay list. The caller validates and
   * owns this immutable array; live authoring normally uses layers above. */
  const DioramaResolvedLayer *resolved_layers;
  int resolved_layer_count;
  /* NULL leaves named art unavailable, retaining the captured-BG fallback. */
  DioramaSkyboxResolver resolve_skybox;
  void *skybox_userdata;
  /* Optional native draw for generated capture motion. NULL retains
   * the portable renderer used by the editor and reference comparisons. */
  DioramaSkyboxDraw draw_resident_skybox;
  DioramaSourceEffectBatch source_effect_batch;
  DioramaVoxelObserveFace voxel_observe_face;
  void *voxel_observer_userdata;
  DioramaVoxelInspection voxel_inspection;
} DioramaRenderOptions;

/* Desktop adapter: snapshot the current settings and manifest for one draw. */
void Diorama_CaptureRenderOptions(DioramaRenderOptions *options);

#endif /* AR_DIORAMA_RENDER_OPTIONS_H */
