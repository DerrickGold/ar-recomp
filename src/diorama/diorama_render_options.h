#ifndef AR_DIORAMA_RENDER_OPTIONS_H
#define AR_DIORAMA_RENDER_OPTIONS_H
/* Explicit compositor policy. Borrowed resources remain valid for one draw;
 * callers own decoding, settings, persistence and backend resource lifetime. */

#include "diorama_layer_order.h"
#include "render/presentation_options.h"
#include "render/render_device.h"

typedef ArRenderTexture (*DioramaSkyboxResolver)(
    void *userdata, ArRenderDevice *device, int source,
    bool transparent_fill_configured, uint32_t transparent_fill_argb,
    bool *state_restore_failed);

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
} DioramaRenderOptions;

/* Desktop adapter: snapshot the current settings and manifest for one draw. */
void Diorama_CaptureRenderOptions(DioramaRenderOptions *options);

#endif /* AR_DIORAMA_RENDER_OPTIONS_H */
