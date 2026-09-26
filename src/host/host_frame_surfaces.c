#include "host/host_frame_surfaces.h"

#include <stdlib.h>

#include "snesrecomp/runner.h"

_Static_assert(kHostDisplayFramebufferHeight >= SR_PPU_SURFACE_MAX_HEIGHT,
               "frame surfaces must hold every row the PPU can render");

uint8_t g_pixels[kHostFrameSurfaceBytes];
uint8_t g_authentic_pixels[kHostFrameSurfaceBytes];
uint8_t g_hud_bg_pixels[kHostFrameSurfaceBytes];
uint8_t g_hud_obj_pixels[kHostFrameSurfaceBytes];
uint8_t g_action_bg1_mask_pixels[kHostFrameSurfaceBytes];
uint8_t g_action_bg2_mask_pixels[kHostFrameSurfaceBytes];
uint8_t *g_diorama_layer_pixels[kDioramaPlane_Count];

uint8_t *HostFrameSurfaces_DioramaPlane(int plane) {
  if (!g_diorama_layer_pixels[plane])
    g_diorama_layer_pixels[plane] = calloc(1, kHostFrameSurfaceBytes);
  return g_diorama_layer_pixels[plane];
}

void HostFrameSurfaces_ReleaseDioramaPlanes(void) {
  for (int plane = 0; plane < kDioramaPlane_Count; plane++) {
    free(g_diorama_layer_pixels[plane]);
    g_diorama_layer_pixels[plane] = NULL;
  }
}
