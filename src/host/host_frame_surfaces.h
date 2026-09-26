#ifndef AR_HOST_FRAME_SURFACES_H
#define AR_HOST_FRAME_SURFACES_H
/* HostFrameSurfaces: the CPU-side ARGB surfaces the PPU renders each frame
 * into before the host uploads them to presentation textures. Each holds the
 * PPU's full render target (SR_PPU_SURFACE_MAX_WIDTH columns by
 * kHostDisplayFramebufferHeight rows) so the active geometry can change live
 * without reallocating storage: a frame uses only the leading g_snes_width*4
 * bytes of each row, and only 224 + g_ws_extra_top + g_ws_extra_bottom rows.
 * The frame draw binds them as PPU outputs and writes them; the host display,
 * HD replacements, the diorama upload and the developer captures read them.
 * Present-phase code never does: its frame arrives through the FrameSlot.
 * Phase: game (frame draw) writes; host reads; both on the main thread. */

#include <stdint.h>

#include "diorama/diorama_planes.h"
#include "host/host_display.h"

enum {
  kHostFrameSurfaceBytes =
      SR_PPU_SURFACE_MAX_WIDTH * 4 * kHostDisplayFramebufferHeight,
};

/* The composited frame the host presents. */
extern uint8_t g_pixels[kHostFrameSurfaceBytes];
/* Complete native PPU result captured beside g_pixels before host presentation
 * extractions remove layers. It stays at the active scanline width (no OBJ
 * apron) because comparison presents only a native 256x224 crop. */
extern uint8_t g_authentic_pixels[kHostFrameSurfaceBytes];
/* The HUD planes (BG3 and OBJ) split out for the widescreen HUD overlay. */
extern uint8_t g_hud_bg_pixels[kHostFrameSurfaceBytes];
extern uint8_t g_hud_obj_pixels[kHostFrameSurfaceBytes];
/* Flat-mode mask of pixels for which BG1 wins the priority resolve of its
 * owning PPU screen. This remains correct in Marahna/Viper rooms where BG1
 * and OBJ are TS-only inputs to the final colour-add composite. */
extern uint8_t g_action_bg1_mask_pixels[kHostFrameSurfaceBytes];
/* Flat-mode mask of pixels for which BG2 wins the complete PPU main-screen
 * priority resolve. A BG2-stage presentation effect is multiplied by this
 * before compositing, so later BG1/OBJ art retains authentic occlusion. */
extern uint8_t g_action_bg2_mask_pixels[kHostFrameSurfaceBytes];

/* Diorama per-plane capture buffers, indexed by kDioramaPlane_* (engine
 * sources = the priority-0 remainder of each layer, appended entries = the
 * priority-band splits; see diorama_planes.h). Dedicated set separate from
 * the HUD/HD overlay buffers (BG3/OBJ reuse those for the widescreen HUD
 * split, and HD replacements claim per-source capture slots — see §4.3).
 * Each is allocated on first diorama capture and released at shutdown. BG4 is
 * never drawn in Mode 1, so excluded; the backdrop slot stays NULL
 * (RenderDiorama points it at g_pixels). */
extern uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];

/* The plane's capture buffer, allocated zero-filled on first use; NULL when
 * that allocation fails. */
uint8_t *HostFrameSurfaces_DioramaPlane(int plane);
void HostFrameSurfaces_ReleaseDioramaPlanes(void);

#endif  /* AR_HOST_FRAME_SURFACES_H */
