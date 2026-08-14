#ifndef DIORAMA_H
#define DIORAMA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include "diorama_planes.h"
#include "diorama_skybox_uv.h"

/* The per-room ($18,$19) layer override table the editor edits and the draw
 * loop reads. Never NULL. Empty means "every room draws as built". */
struct DioramaLayerOrderTable *Diorama_LayerOverrides(void);

/* Decode deterministic named backdrop sources from immutable cart data. A
 * failed source remains unavailable and authored uses fall back to captured. */
bool Diorama_InitRomBackdrops(const uint8_t *rom_data, size_t rom_size);

/* Load / write `diorama-layers.ini` (beside settings.ini). Load is called once
 * at boot; save is the editor's "Export manifest". Absent file = no overrides. */
void Diorama_LoadLayerManifest(void);
bool Diorama_SaveLayerManifest(void);

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
void Diorama_GetDynamicCameraOrbit(float *yaw, float *pitch);
void Diorama_ResetCamera(void);
bool Diorama_IsActiveThisFrame(void);
void Diorama_OnModeChanged(void);

float Diorama_DragRadPerPx(void);
float Diorama_ZoomStep(void);
bool Diorama_IsDragging(void);
void Diorama_SetDragging(bool dragging);

/* Draw the diorama scene, split into an upload phase (SDL_UpdateTexture only)
 * and a composite phase (SDL_RenderGeometry + camera projection), per M5's
 * buffer-ownership design: the caller can release pixel-buffer writers right
 * after Diorama_Upload returns, without waiting for Composite's vsync-bound
 * present. textures[]/pixels[] are kDioramaPlane_Count-sized, indexed by
 * kDioramaPlane_*; the caller fills [kDioramaPlane_Backdrop] with the
 * residual main framebuffer. Planes with a NULL texture or pixels are
 * skipped. `plane_mask` is the caller's immutable request/content
 * intersection (M5 D3 — present-time code must not re-derive live settings
 * state). Returns a bit per successfully uploaded plane so the compositor
 * cannot resurface stale texture contents after an upload failure. */
/* `snes_width` is the FULL surface width including both aprons; `obj_apron` is
 * the per-side apron so planes that cannot hold apron content upload only their
 * display columns (see DioramaPlaneCanCarryApron). */
uint32_t Diorama_Upload(SDL_Texture *textures[], uint8_t *pixels[],
                        int snes_width, int snes_height, int obj_apron,
                        uint32_t plane_mask);

/* M7 (§6): per-BG-layer UV shift for present-time scroll interpolation.
 * Indexed by SNES BG number (0=BG1..3=BG4); Diorama_Composite maps each
 * layer's plane to its BG internally and applies bg_du/bg_dv[that BG] to
 * the layer's mesh UVs. active=false (or a NULL pointer) disables
 * interpolation entirely for that composite call — e.g. first frame, a
 * BG-mode change between captures, or turbo (§6.4 edge cases). */
typedef struct DioramaScrollDelta {
  bool active;
  float bg_du[4];
  float bg_dv[4];
} DioramaScrollDelta;

/* B4-split (followup doc): the camera pose Diorama_Composite renders with,
 * passed in by the caller instead of Composite reading producer-owned
 * g_diorama_cam directly. Free Cam mode: the caller passes the authored pose
 * (snapshotted through FrameSlot). Dynamic Cam mode: the caller passes the
 * presentation-owned render camera with damped sway—see present.c's
 * g_diorama_render_cam. fov_y isn't
 * part of this: it's a fixed camera constant (kDioramaFovY, diorama.c),
 * never authored per-mode. */
typedef struct DioramaCameraPose {
  float tilt_x;
  float tilt_y;
  float distance;
} DioramaCameraPose;

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
 * interpolated BG1/BG2 UV windows, and per-room BG1/BG2/OBJ plane shapes used
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
  DioramaPlaneProjection object_planes[kDioramaObjectPriorityCount];
} DioramaProjection;

/* Optional presentation hook inserted immediately after a drawable plane's
 * main mesh. It receives the same resolved projection the plane uses, making
 * BG-local enhancements part of painter order instead of a late world overlay. */
typedef void (*DioramaPlaneEffectFn)(void *userdata, int plane,
                                    const DioramaProjection *projection);

/* Optional replacement for one captured plane's complete geometry pass. It is
 * invoked in the plane's authentic painter position with the renderer still
 * in viewport-local coordinates. Returning true skips the captured plane,
 * including its whole-plane shadow/depth treatments; false draws it normally.
 * The action OBJ interpolator uses this to submit independently moving atlas
 * quads without flattening them into an intermediate pixel grid. */
typedef bool (*DioramaPlaneReplacementFn)(
    void *userdata, int plane, const DioramaProjection *projection,
    SDL_FColor shade, bool additive, bool casts_shadow);

/* Pure eligibility contract shared by projection publication and drawing.
 * Resource booleans describe this frame's upload/content intersection. */
bool Diorama_PlaneEligible(int plane, bool visible, bool has_texture,
                           bool has_pixels, bool hud_flat, bool skybox_only);

/* Projection normally has the same current-pixel contract as drawing. A
 * current OBJ-attached host effect is also current content for its authentic
 * priority plane, even when that isolated sprite band has no winning pixels.
 * The exception is deliberately rejected for every non-OBJ plane. */
bool Diorama_PlaneProjectable(int plane, bool visible, bool has_texture,
                              bool has_pixels, bool has_obj_effect,
                              bool hud_flat, bool skybox_only);

/* Keeps required OBJ priorities whose plane was requested and either had no
 * source pixels or uploaded those pixels successfully. This distinguishes an
 * intentionally empty band from an upload failure before pixels[] collapses
 * both to NULL at composite time. */
uint8_t Diorama_FilterObjEffectProjectionMask(
    uint8_t required_priorities, uint32_t requested_planes,
    uint32_t content_planes, uint32_t uploaded_planes);

/* Maps a captured framebuffer point onto its authentic action OBJ priority
 * plane. scale_x/scale_y are the projected lengths of one captured pixel. */
bool Diorama_ProjectCapturedPoint(const DioramaProjection *projection,
                                  float capture_x, float capture_y,
                                  unsigned obj_priority, SDL_FPoint *point,
                                  float *scale_x, float *scale_y);

/* Same mapping, using the resolved BG1-low plane. Environmental lights such
 * as wall torches stay registered to raked/bowed room geometry instead of
 * floating at an arbitrary OBJ depth. */
bool Diorama_ProjectCapturedBg1Point(const DioramaProjection *projection,
                                     float capture_x, float capture_y,
                                     SDL_FPoint *point,
                                     float *scale_x, float *scale_y);

/* Same mapping, using the resolved BG2-low backdrop plane. Waterfall accents
 * follow the independently-authored backdrop rake/depth instead of borrowing
 * BG1's playfield shape. */
bool Diorama_ProjectCapturedBg2Point(const DioramaProjection *projection,
                                     float capture_x, float capture_y,
                                     SDL_FPoint *point,
                                     float *scale_x, float *scale_y);

/* B4-kick (followup doc): boost's "zoom-punch" multiplies the RESOLVED
 * distance (1.0 = no change) rather than offsetting DioramaCameraPose's
 * distance field directly — cam_pose->distance is often the 0 "auto-fit"
 * sentinel (M5's dead-zone design, see Diorama_Composite's cam.distance
 * handling), and an additive world-unit offset on top of 0 would still
 * resolve to <=0 and re-trigger auto-fit, silently eating the punch. A
 * multiplier applied AFTER auto-fit/dead-zone resolution composes correctly
 * either way, which is also why this is its own parameter rather than a
 * 4th DioramaCameraPose field — that struct is reused verbatim for
 * FrameSlot's settings snapshots (main.c), which have no kick state at all.
 *
 * `viewport` is the aspect-fit game rectangle in physical output pixels. The
 * compositor renders in viewport-local coordinates and restores the full
 * renderer viewport before returning.
 *
 * bg2_valid_spans (Fix B/BH6): exact capture-row and texture-column regions of
 * BG2's rendered content. The skybox uses every row span for screen-space
 * crop/stretch; an attached BG2 continuation additionally uses their drawable
 * row bound as its sampling handoff. They are deliberately NOT used to narrow
 * ordinary layer UVs: those quads are world-registered against BG1, and doing
 * so would desync them. */
/* authentic_y0: texture row where authentic screen y=0 begins inside the
 * vertically expanded capture.
 *
 * obj_apron: columns of RESOLVE apron the bound surfaces carry per side. The
 * displayed span is [obj_apron, obj_apron+snes_width) -- snes_width stays the
 * DISPLAY width, so the mesh, aspect_x and the camera fit are unaffected by
 * the apron and apron 0 reproduces the pre-apron geometry exactly. Only the UV
 * window and the supersample source rect move.
 *
 * effect_obj_priority_mask: authentic OBJ bands required by current captured
 * world-overlay effects. Those actors are current projection content even if
 * their isolated hardware band has no final winning pixels; this cannot make
 * a BG plane projectable or bypass a hidden OBJ layer/missing texture. */
bool Diorama_Composite(SDL_Renderer *renderer, int snes_width, int snes_height,
                       int authentic_y0,
                       int obj_apron,
                       int active_pixel_aspect, bool ignore_aspect_ratio,
                       int visible_width, SDL_Rect viewport,
                       SDL_Texture *textures[],
                       uint8_t *pixels[],
                       const DioramaScrollDelta *scroll_delta,
                       bool obj_vector_interpolation,
                       const DioramaCameraPose *cam_pose,
                       float distance_scale,
                       uint32_t additive_plane_mask,
                       uint8_t effect_obj_priority_mask,
                       uint8_t map_group, uint8_t map_number,
                       uint8_t layer_section,
                       const DioramaBgValidSpanPlan *bg2_valid_spans,
                       DioramaPlaneReplacementFn plane_replacement,
                       void *plane_replacement_userdata,
                       DioramaPlaneEffectFn plane_effect,
                       void *plane_effect_userdata,
                       DioramaProjection *out_projection);

/* Drops renderer-owned targets/effects after SDL_EVENT_RENDER_DEVICE_RESET so
 * they are lazily recreated against the current device. */
void Diorama_ResetRendererResources(SDL_Renderer *renderer);

/* Releases renderer-owned supersample and optional GPU-effect resources.
 * Call before destroying the renderer. */
void Diorama_Shutdown(SDL_Renderer *renderer);

void Diorama_FlushSettingsIfDirty(void);

#endif  /* DIORAMA_H */
