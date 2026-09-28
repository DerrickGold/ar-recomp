#ifndef AR_DIORAMA_H
#define AR_DIORAMA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "diorama_coverage.h"
#include "diorama_camera.h"
#include "diorama_planes.h"
#include "diorama_skybox_uv.h"
#include "present/presentation_outcome.h"
#include "render/render_device.h"

/* Decode deterministic named backdrop sources from immutable cart data. A
 * failed source remains unavailable and authored uses fall back to captured. */
bool Diorama_InitRomBackdrops(const uint8_t *rom_data, size_t rom_size);

/* Install the live-room, override, persistence and CGRAM hooks after loading
 * the manifest. The editor snapshots the palette only when its picker opens. */
void Diorama_InstallLayerEditor(void);
/* The room the draw loop is currently applying overrides to, for the layer
 * editor. False when no diorama room is running, in which case the outputs are
 * untouched -- so the editor reports a room exactly when authoring one would
 * have a visible effect. */
bool Diorama_LiveRoom(uint8_t *out_group, uint8_t *out_map,
                      uint8_t *out_section);
void Diorama_PublishLiveLayerSection(uint8_t map_group, uint8_t map_number,
                                     uint8_t section);

void Diorama_SeedCameraFromSettings(void);
void Diorama_AdjustCamera(float d_yaw, float d_pitch, float d_zoom);
bool Diorama_UpdateDynamicCamera(float elapsed_seconds, bool orbit_held);
void Diorama_ResetCamera(void);
bool Diorama_IsActiveThisFrame(void);
/* Whether the frame being drawn is a diorama frame: the frame draw latches
 * Diorama_IsActiveThisFrame into it once per frame, and turning diorama mode
 * off clears it. The enhancements, FrameSlot_Capture and the host loop read
 * it. Defined in diorama_host.c. */
extern bool g_diorama_frame_active;
void Diorama_OnModeChanged(void);

float Diorama_DragRadPerPx(void);
float Diorama_ZoomStep(void);
bool Diorama_IsDragging(void);
void Diorama_SetDragging(bool dragging);

/* Shared by focal framing and the rendered layer mesh. */
enum { kDioramaPlaneSubdivY = 6 };

/* Center the projected bounds of the focal mesh vertically. authentic_t0/t1
 * delimit the native playfield within a capture: when the whole mesh cannot fit,
 * keep that band visible (or center it if even the native band cannot fit).
 * Changes only screen Y, preserving perspective, depth and horizontal framing.
 * Invalid/unprojectable geometry leaves the matrix untouched. */
bool Diorama_CenterCameraVertically(
    float matrix[16], float aspect_x, float height_scale,
    float z_world, float rake, float bow,
    float authentic_t0, float authentic_t1);

enum { kDioramaObjectPriorityCount = 4 };

typedef struct DioramaPlaneProjection {
  bool valid;
  float u0, v0, u1, v1;
  float z_world;
  float rake;
  float bow;
  /* Optional continuation attached below this plane. Projecting BG-local
   * atmosphere through these exact fold parameters keeps it registered with
   * curved auxiliary geometry instead of extrapolating a flat billboard. */
  bool overflow_valid;
  float overflow_fold_t;
  float overflow_height;
  float overflow_overlap_t;
  float overflow_handoff_z;
  float overflow_front_z;
  float overflow_front_drop;
} DioramaPlaneProjection;

/* Resolved action-world projection for presentation-only overlays. The
 * compositor publishes the same camera, mesh dimensions, independent
 * source UV windows and per-room BG1/BG2/OBJ plane shapes used
 * by the captured planes; consumers therefore cannot duplicate auto-fit or
 * guess a parallel depth. */
typedef struct DioramaProjection {
  bool valid;
  float matrix[16];
  float aspect_x, height_scale;
  /* Texture column containing captured display x=0. Action-effect callers
   * speak in display-capture coordinates; the projection owns the hidden OBJ
   * resolve apron carried by every diorama layer surface. */
  int texture_x_origin;
  int texture_width, texture_height;
  int output_x, output_y;
  int output_width, output_height;
  DioramaPlaneProjection bg1_plane;
  DioramaPlaneProjection bg2_plane;
  DioramaPlaneProjection bg1_high_plane;
  DioramaPlaneProjection bg2_high_plane;
  DioramaPlaneProjection object_planes[kDioramaObjectPriorityCount];
} DioramaProjection;

/* Optional presentation hook inserted immediately after a drawable plane's
 * main mesh. It receives the same resolved projection the plane uses, making
 * BG-local enhancements part of painter order instead of a late world overlay. */
typedef void (*DioramaPlaneEffectFn)(void *userdata, int plane,
                                    const DioramaProjection *projection);

/* Pure eligibility contract shared by projection publication and drawing.
 * Resource booleans describe this frame's upload/content intersection. */
bool Diorama_PlaneEligible(int plane, bool visible, bool has_texture,
                           bool has_pixels, bool hud_flat, bool skybox_only);

/* Projection normally has the same current-pixel contract as drawing. A
 * current host effect is also current content for its authentic BG or OBJ
 * plane, even when that isolated hardware band has no winning pixels. */
bool Diorama_PlaneProjectable(int plane, bool visible, bool has_texture,
                              bool has_pixels, bool has_attached_effect,
                              bool hud_flat, bool skybox_only);

/* Keeps required OBJ priorities whose plane was requested and either had no
 * source pixels or uploaded those pixels successfully. This distinguishes an
 * intentionally empty band from an upload failure before pixels[] collapses
 * both to NULL at composite time. */
uint8_t Diorama_FilterObjEffectProjectionMask(
    uint8_t required_priorities, uint32_t requested_planes,
    uint32_t content_planes, uint32_t uploaded_planes);

/* Keeps required BG transforms when their capture was intentionally empty,
 * but removes a content-bearing plane whose texture upload actually failed. */
uint32_t Diorama_FilterBgEffectProjectionMask(
    uint32_t required_planes, uint32_t requested_planes,
    uint32_t content_planes, uint32_t uploaded_planes);

/* Maps a captured framebuffer point onto its authentic action OBJ priority
 * plane. scale_x/scale_y are the projected lengths of one captured pixel. */
bool Diorama_ProjectCapturedPoint(const DioramaProjection *projection,
                                  float capture_x, float capture_y,
                                  unsigned obj_priority, ArRenderPointF *point,
                                  float *scale_x, float *scale_y);

/* Same mapping, using the resolved BG1-low plane. Environmental lights such
 * as wall torches stay registered to raked/bowed room geometry instead of
 * floating at an arbitrary OBJ depth. */
bool Diorama_ProjectCapturedBg1Point(const DioramaProjection *projection,
                                     float capture_x, float capture_y,
                                     ArRenderPointF *point,
                                     float *scale_x, float *scale_y);

/* Same mapping for BG1's priority-1 tile band. */
bool Diorama_ProjectCapturedBg1HighPoint(
    const DioramaProjection *projection,
    float capture_x, float capture_y, ArRenderPointF *point,
    float *scale_x, float *scale_y);

/* Same mapping for BG2's priority-1 water/foreground band. */
bool Diorama_ProjectCapturedBg2HighPoint(
    const DioramaProjection *projection,
    float capture_x, float capture_y, ArRenderPointF *point,
    float *scale_x, float *scale_y);

/* Same mapping, using the resolved BG2-low backdrop plane. Waterfall accents
 * follow the independently-authored backdrop rake/depth instead of borrowing
 * BG1's playfield shape. */
bool Diorama_ProjectCapturedBg2Point(const DioramaProjection *projection,
                                     float capture_x, float capture_y,
                                     ArRenderPointF *point,
                                     float *scale_x, float *scale_y);

typedef struct DioramaSkyboxView {
  ArRenderTexture texture;
  uint64_t revision;
  int width;
  bool dynamic;
} DioramaSkyboxView;

/* Borrowed for this draw. Width excludes the hidden apron; authentic_y0 is
 * the first native row inside the vertically expanded capture. A non-NULL
 * pixels entry certifies current content, not merely an allocated texture.
 * Valid BG2 spans bound skybox sampling and waterfall attachment, never the
 * ordinary world-registered layer UVs. */
typedef struct DioramaCapture {
  int width, height, authentic_y0, obj_apron;
  const ArRenderTexture *textures;
  const uint8_t *const *pixels;
  const bool *bg_transparent_fill_configured;
  const uint32_t *bg_transparent_fill_argb;
  const DioramaCoverageMask *coverage_masks;
  const DioramaBgValidSpanPlan *bg2_valid_spans;
  const DioramaSkyboxView *skybox;
  /* Revision identifies captured pixels; dynamic marks interpolated pixels
   * that cannot reuse an immutable skybox prefilter result. */
  uint64_t bg2_revision;
  bool bg2_dynamic;
} DioramaCapture;

typedef struct DioramaView {
  DioramaCameraPose camera;
  /* Applied after auto-fit resolves camera.distance's zero sentinel. */
  float distance_scale;
  bool center_camera_vertically;
  int pixel_aspect;
  bool ignore_aspect_ratio;
  int visible_width;
  ArRenderRectI viewport;
} DioramaView;

typedef struct DioramaScene {
  uint8_t map_group, map_number, layer_section;
  uint32_t additive_plane_mask;
  /* 0 leaves authored color intact; 1 darkens BG1 low/high/far to black.
   * Applied to existing face/depth geometry, without changing layer alpha. */
  float bg1_dimming;
  /* Current effects retain projection on an intentionally empty plane. */
  uint8_t effect_obj_priority_mask;
  uint32_t effect_bg_plane_mask;
  DioramaPlaneEffectFn plane_effect;
  void *plane_effect_userdata;
} DioramaScene;

/* Draws in viewport-local coordinates and restores full output on every exit.
 * Enter without a custom GPU state bound; the compositor owns all effects it
 * binds. Complete/OptionalOmitted produce a usable scene; CoreFailure means
 * the caller must stop rather than publish a partial frame. */
PresentationOutcome Diorama_Composite(ArRenderDevice *device,
                                      const DioramaCapture *capture,
                                      const DioramaView *view,
                                      const DioramaScene *scene,
                                      DioramaProjection *out_projection);

/* Drops backend-owned targets/effects after a render-device reset so they are
 * lazily recreated against the current device. */
void Diorama_ResetRendererResources(ArRenderDevice *device);

/* Releases backend-owned supersample and optional GPU-effect resources.
 * Call before destroying the render device. */
void Diorama_Shutdown(ArRenderDevice *device);

void Diorama_FlushSettingsIfDirty(void);

/* Apply live settings here, beside the subsystem they configure. */
struct SettingDesc;
void Diorama_ApplySetting(const struct SettingDesc *desc);

#endif  /* AR_DIORAMA_H */
