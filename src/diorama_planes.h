#pragma once
#include "snes/ppu.h"

/* Diorama plane indexing for g_diorama_layer_pixels[] and the plane texture
 * array. The engine-source primaries keep their PpuOverlaySource index and,
 * once the priority bands are bound, hold only the LOWEST priority rank of
 * their layer (BG1/BG2 = priority-0 tiles, OBJ = priority-0 sprites; BG3
 * stays whole — the HUD). The appended entries are the priority-band splits
 * (PpuBindOverlayPrioSurface) plus the backdrop slot the render wrapper
 * points at g_pixels. SDL-free so actraiser_rtl.c can bind bands by index. */
enum {
  kDioramaPlane_Backdrop = kPpuOverlaySource_Count,  /* residual main frame */
  kDioramaPlane_Bg1Hi,                               /* BG1 priority-1 tiles */
  kDioramaPlane_Bg2Hi,                               /* BG2 priority-1 tiles */
  kDioramaPlane_Obj1,                                /* sprites, priority 1 */
  kDioramaPlane_Obj2,                                /* sprites, priority 2 */
  kDioramaPlane_Obj3,                                /* sprites, priority 3 */
  kDioramaPlane_Count
};
