#ifndef DIORAMA_PLANES_H
#define DIORAMA_PLANES_H
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

/* Can this plane ever hold pixels in the resolve apron?
 *
 * ONLY the OBJ planes. Everything else is filled exclusively by the scanline
 * path, which is bounded by the display margins and by kPpuExtraLeftRight and
 * therefore cannot reach an apron column; the apron pass
 * (ActRaiser_DioramaApronFinish) writes OBJ planes and nothing else. The
 * backdrop is the residual main framebuffer, whose apron the compositor never
 * touches either.
 *
 * Consequence, and the reason this predicate exists rather than being folded
 * into a comment: uploading the apron columns of a plane that can never fill
 * them is uploading known zeros. Measured at 0.79 MB per frame, ~47 MB/s at
 * 60fps, which was most of what the apron cost in steady state. */
static inline bool DioramaPlaneCanCarryApron(int plane) {
  return plane == kPpuOverlaySource_Obj || plane == kDioramaPlane_Obj1 ||
         plane == kDioramaPlane_Obj2 || plane == kDioramaPlane_Obj3;
}

#endif  /* DIORAMA_PLANES_H */
