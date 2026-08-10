#ifndef DIORAMA_H
#define DIORAMA_H
#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>
#include "diorama_planes.h"
#include "diorama_skybox_uv.h"

/* The per-room ($18,$19) layer override table the editor edits and the draw
 * loop reads. Never NULL. Empty means "every room draws as built". */
struct DioramaLayerOrderTable *Diorama_LayerOverrides(void);

/* Load / write `diorama-layers.ini` (beside settings.ini). Load is called once
 * at boot; save is the editor's "Export manifest". Absent file = no overrides. */
void Diorama_LoadLayerManifest(void);
bool Diorama_SaveLayerManifest(void);

/* The room the draw loop is currently applying overrides to, for the layer
 * editor. False when no diorama room is running, in which case the outputs are
 * untouched -- so the editor reports a room exactly when authoring one would
 * have a visible effect. */
bool Diorama_LiveRoom(uint8_t *out_group, uint8_t *out_map);

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
 * passed in by the caller instead of Composite reading the game-thread-owned
 * g_diorama_cam directly (the pre-existing D6 exception this checkpoint
 * closes). Free Cam mode: the caller passes the authored/persisted pose
 * (snapshotted through FrameSlot). Dynamic Cam mode: the caller passes the
 * present-thread's own render camera (baseline pose, later checkpoints add
 * damped sway on top) — see present.c's g_diorama_render_cam. fov_y isn't
 * part of this: it's a fixed camera constant (kDioramaFovY, diorama.c),
 * never authored per-mode. */
typedef struct DioramaCameraPose {
  float tilt_x;
  float tilt_y;
  float distance;
} DioramaCameraPose;

enum { kDioramaObjectPriorityCount = 4 };

typedef struct DioramaObjectPlaneProjection {
  bool valid;
  float z_world;
  float rake;
  float bow;
} DioramaObjectPlaneProjection;

/* Resolved action-OBJ projection for presentation-only overlays. The
 * compositor publishes the same camera, mesh dimensions, interpolated BG1 UV
 * window, and per-room OBJ plane shapes used by the captured sprite planes;
 * consumers therefore cannot duplicate auto-fit or guess a parallel depth. */
typedef struct DioramaProjection {
  bool valid;
  float matrix[16];
  float object_u0, object_v0, object_u1, object_v1;
  float aspect_x, height_scale;
  int texture_width, texture_height;
  int output_x, output_y;
  int output_width, output_height;
  DioramaObjectPlaneProjection object_planes[kDioramaObjectPriorityCount];
} DioramaProjection;

/* Maps a captured framebuffer point onto its authentic action OBJ priority
 * plane. scale_x/scale_y are the projected lengths of one captured pixel. */
bool Diorama_ProjectCapturedPoint(const DioramaProjection *projection,
                                  float capture_x, float capture_y,
                                  unsigned obj_priority, SDL_FPoint *point,
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
 * BG2's rendered content. Only the skybox uses them — it is screen-space, so
 * each row band can crop/stretch independently. They are deliberately NOT
 * applied to the per-layer loop: those quads are world-registered against BG1,
 * and narrowing their UV would desync them. */
/* authentic_y0: texture row where authentic screen y=0 begins inside the
 * vertically expanded capture.
 *
 * obj_apron: columns of RESOLVE apron the bound surfaces carry per side. The
 * displayed span is [obj_apron, obj_apron+snes_width) -- snes_width stays the
 * DISPLAY width, so the mesh, aspect_x and the camera fit are unaffected by
 * the apron and apron 0 reproduces the pre-apron geometry exactly. Only the UV
 * window and the supersample source rect move. */
bool Diorama_Composite(SDL_Renderer *renderer, int snes_width, int snes_height,
                       int authentic_y0,
                       int obj_apron,
                       int active_pixel_aspect, bool ignore_aspect_ratio,
                       int visible_width, SDL_Rect viewport,
                       SDL_Texture *textures[],
                       uint8_t *pixels[],
                       const DioramaScrollDelta *scroll_delta,
                       const DioramaCameraPose *cam_pose,
                       float distance_scale,
                       const DioramaBgValidSpanPlan *bg2_valid_spans,
                       DioramaProjection *out_projection);

/* Drops renderer-owned targets/effects after SDL_EVENT_RENDER_DEVICE_RESET so
 * they are lazily recreated against the current device. */
void Diorama_ResetRendererResources(SDL_Renderer *renderer);

/* Releases renderer-owned supersample and optional GPU-effect resources.
 * Call before destroying the renderer. */
void Diorama_Shutdown(SDL_Renderer *renderer);

void Diorama_FlushSettingsIfDirty(void);

#endif  /* DIORAMA_H */
