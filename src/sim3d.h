#ifndef SIM3D_H
#define SIM3D_H

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
  bool diorama_active;
  SimRenderFeatureMask requested_features;
  uint32_t diagnostic_layer_mask;
  int width, height;
} Sim3DCaptureRequest;

enum {
  kSim3DMaxWidth = 448,  /* kPpuBufWidth, asserted in sim3d.c */
  kSim3DMaxHeight = 240,
};

/* Raw capture buffers are game-thread-written until PresentUpload completes.
 * They are allocated once on first supported capture and never reallocated. */
extern uint8_t *g_sim3d_layer_pixels[kSim3DPlane_Count];
extern uint32_t g_sim3d_flat_pixels[kSim3DMaxWidth * kSim3DMaxHeight];
extern uint32_t g_sim3d_difference_pixels[kSim3DMaxWidth * kSim3DMaxHeight];

/* OBJ priority bands are interleaved with BG planes in SNES painter order;
 * callers must never derive these plane indices arithmetically. */
int Sim3D_ObjPlaneForPriority(int priority);

/* Called before per-frame overlay policies. True means the preceding SIM
 * capture owned the PPU bindings and the frontend must restore its defaults. */
bool Sim3D_BeginFrame(void);

/* Called after ordinary HUD/HD/diorama policy has declared its captures, but
 * before scanout. The known town-HUD BG3/OBJ captures are safely superseded by
 * the complete SIM planes; every other owner fails closed. SIM captures are
 * observational: the authentic PPU framebuffer is never removed or modified. */
bool Sim3D_PrepareCapture(Ppu *ppu, const Sim3DCaptureRequest *request);

/* Rebuilds the pitch-zero image after scanout and compares it against the
 * same-frame authentic framebuffer. A mismatch fails closed for this frame. */
void Sim3D_FinishCapture(uint8_t *authentic_pixels,
                         int authentic_pitch, uint16_t game_frame);

SimRenderFeatureMask Sim3D_ImplementedFeatures(void);
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
  int cull_lift_inset;
  int backdrop_strength_pct;
  int backdrop_horizon_pct;
  /* Sprite-drawable margins either side of the authentic window, from
   * ActRaiser_SimSpriteMargins. Passed in rather than queried here so sim3d.c
   * stays linkable without the widescreen sprite unit, which the focused
   * tests do not build. */
  int sprite_margin_left, sprite_margin_right;
} Sim3DTuning;

void Sim3D_AnnotateFrame(SimFrameData *frame, const Sim3DTuning *tuning);
/* Re-renders the whole town's ground from the resident WRAM tilemap plus
 * live VRAM/CGRAM. Call once per game frame, after Sim3D_AnnotateFrame; a
 * frame that failed the fidelity gate is skipped. */
void Sim3D_RenderTownCanvas(const SimFrameData *frame, const uint8 *wram,
                            const Ppu *ppu);
/* Pure painter-order reference used by focused tests. `plane_mask==0` means
 * every plane; otherwise bit P controls Sim3DPlane P. */
void Sim3D_ComposeFlatPixels(
    uint32_t *dst, int width, int height, int pitch,
    uint32_t backdrop_argb, int live_x0, int live_x1,
    uint8_t *const planes[kSim3DPlane_Count], uint32_t plane_mask,
    int full_width_rows);

#endif  /* SIM3D_H */
