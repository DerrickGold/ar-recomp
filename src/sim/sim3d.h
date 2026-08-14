#ifndef SIM3D_H
#define SIM3D_H

#include "sim_background_voxel_quality.h"

#include <stdbool.h>
#include <stdint.h>

#include "sim3d_planes.h"
#include "sim_render_metadata.h"
#include "types.h"

typedef struct Ppu Ppu;

typedef struct Sim3DCaptureRequest {
  bool town;
  bool master_enabled;
  bool picker_active;
  bool renderer_ready;
  /* Current-frame semantic OBJ data and its GPU consumer are both required
   * before raw OBJ capture may be suppressed. Either false keeps the four
   * priority planes as an exact fallback. */
  bool billboard_atlas_ready;
  bool billboard_renderer_ready;
  bool diorama_active;
  /* A capture-diagnostics UI is on screen this frame (the scene inspector
   * panel reports the composed-frame hash). Supplied by the host rather than
   * read here so this module stays free of the settings layer; when nothing is
   * displaying them, the diagnostic-only pixel passes are skipped. */
  bool inspector_active;
  SimRenderFeatureMask requested_features;
  uint32_t diagnostic_layer_mask;
  int width, height;
} Sim3DCaptureRequest;

enum {
  kSim3DMaxWidth = 512,  /* kPpuBufWidth, asserted in sim3d.c */
  kSim3DMaxHeight = 240,
};

/* Raw capture buffers are game-thread-written until PresentUpload completes.
 * Each plane is allocated lazily on first demand and never reallocated. */
extern uint8_t *g_sim3d_layer_pixels[kSim3DPlane_Count];
extern uint32_t g_sim3d_flat_pixels[kSim3DMaxWidth * kSim3DMaxHeight];
extern uint32_t g_sim3d_difference_pixels[kSim3DMaxWidth * kSim3DMaxHeight];

/* OBJ priority bands are interleaved with BG planes in SNES painter order;
 * callers must never derive these plane indices arithmetically. */
int Sim3D_ObjPlaneForPriority(int priority);

/* Plane textures sampled by the resolved presentation profile. Flat mode
 * samples only g_sim3d_flat_pixels; projected billboard mode replaces the
 * four raw OBJ planes with the packed object atlas. */
uint32_t Sim3D_PlaneTextureUploadMask(
    SimRenderFeatureMask effective_features, uint32_t captured_plane_mask);

/* Called before per-frame overlay policies. True means the preceding SIM
 * capture owned the PPU bindings and the frontend must restore its defaults. */
bool Sim3D_BeginFrame(void);

/* Called after ordinary HUD/HD/diorama policy has declared its captures, but
 * before scanout. The known town-HUD BG3/OBJ captures are safely superseded by
 * the complete SIM planes; every other owner fails closed. SIM captures are
 * observational: the authentic PPU framebuffer is never removed or modified. */
bool Sim3D_PrepareCapture(Ppu *ppu, const Sim3DCaptureRequest *request);

/* Rebuilds the pitch-zero image after scanout only when the flat stage or a
 * diagnostic consumer needs it. Diagnostic modes also compare it against the
 * same-frame authentic framebuffer; projected ordinary play skips both
 * full-frame passes. */
void Sim3D_FinishCapture(uint8_t *authentic_pixels,
                         int authentic_pitch, uint16_t game_frame);

SimRenderFeatureMask Sim3D_ImplementedFeatures(void);

/* Host-side camera controls for the enhanced simulation-town view. Render
 * textures remain host-owned, so availability accepts their current readiness
 * rather than reaching into main.c. In Dynamic mode, zoom edits the persisted
 * baseline while orbit is a transient offset that decays after release. */
bool Sim3DCamera_ControlsAvailable(bool textures_ready);
void Sim3DCamera_Adjust(float yaw_delta, float pitch_delta, float zoom_delta);
bool Sim3DCamera_UpdateDynamic(float elapsed_seconds, bool orbit_held);
void Sim3DCamera_GetDynamicOrbit(float *yaw, float *pitch);
void Sim3DCamera_Reset(void);
bool Sim3DCamera_IsDragging(void);
void Sim3DCamera_SetDragging(bool dragging);
void Sim3DCamera_FlushSettingsIfDirty(void);

/* Console line on every enhanced<->authentic transition in a town, so a
 * one-frame flicker names its own cause without a trace pass. */
void Sim3D_LogViewTransition(const SimFrameData *frame);
/* Resolved presentation tuning for one frame. Grouped rather than passed as
 * positional arguments because every visual stage adds another knob, and a
 * frame must never mix values read at different times. */
typedef struct Sim3DTuning {
  int pitch_mrad;
  int yaw_mrad;
  int distance_x100;
  int height_scale_x100;
  int voxel_detail;
  int shadow_opacity_pct;
  int height_pop_pct;
  int light_azimuth_deg;
  int light_elevation_deg;
  int shadow_softness_pct;
  int rim_strength_pct;
  int underlay_haze_pct;
  int cloud_opacity_pct;
  int cloud_falloff_px;
  int cloud_inset_px;
  int cull_lead_px;
  int cull_haze_pct;
  int cull_dim_pct;
  int cull_haze_lead_px;
  int cull_corner_px;
  int underlay_defocus_pct;
  int cloud_altitude_px;
  int cloud_drift_pct;
  int world_navigation_lighting;
  int world_navigation_clouds;
  int world_navigation_backdrop;
  int world_navigation_haze;
  int cull_lift_inset;
  int backdrop_strength_pct;
  int backdrop_horizon_pct;
  /* Sprite-drawable margins either side of the authentic window, from
   * ActRaiser_SimSpriteMargins. Passed in rather than queried here so sim3d.c
   * stays linkable without the widescreen sprite unit, which the focused
   * tests do not build. */
  int sprite_margin_left, sprite_margin_right;
  int sprite_margin_top, sprite_margin_bottom;
} Sim3DTuning;

void Sim3D_AnnotateFrame(SimFrameData *frame, const Sim3DTuning *tuning);
/* Re-renders the whole town's ground from the resident WRAM tilemap plus
 * live VRAM/CGRAM. Call once per game frame, after Sim3D_AnnotateFrame; a
 * frame without a usable separated capture is skipped. */
void Sim3D_RenderTownCanvas(const SimFrameData *frame, const uint8 *wram,
                            const Ppu *ppu);
/* Pure painter-order reference used by focused tests. `plane_mask==0` means
 * every plane; otherwise bit P controls Sim3DPlane P. */
void Sim3D_ComposeFlatPixels(
    uint32_t *dst, int width, int height, int pitch,
    uint32_t backdrop_argb, int live_x0, int live_x1,
    uint8_t *const planes[kSim3DPlane_Count], uint32_t plane_mask,
    int full_width_rows);

/* The colour the authentic renderer shows for any unrendered pixel: cgram[0]
 * expanded to 8 bits per channel through the PPU's brightness table. Shared so
 * the widescreen margin-gap fill (actraiser_ws_gap.h) and the sim-3D flat
 * composite cannot disagree about it. */
uint32_t ActRaiser_BackdropArgb(const Ppu *ppu);
/* World navigation composites its complete host scene at full intensity, then
 * applies INIDISP master brightness once. This form prevents the backdrop
 * from being darkened once during capture and again during composition. */
uint32_t ActRaiser_BackdropArgbFullBrightness(const Ppu *ppu);

#endif  /* SIM3D_H */
