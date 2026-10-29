#ifndef AR_DIORAMA_ASTRAL_RIFT_H
#define AR_DIORAMA_ASTRAL_RIFT_H
#include "diorama.h"

/* The existing final-battle preference now enhances captured native artwork.
 * It never replaces/omits capture planes or loads generated art. Shared by
 * the game and editor; flat presentation never calls Attach. */
bool DioramaAstralRift_Prepare(ArRenderDevice *device);
void DioramaAstralRift_Reset(ArRenderDevice *device);
bool DioramaAstralRift_Attach(bool enabled, ArRenderDevice *device, DioramaScene *scene);
/* Into the native background target, before radiance extraction and fighters. */
bool DioramaAstralRift_DrawDust(ArRenderDevice *device, const DioramaScene *scene,
    const DioramaCapture *capture, const DioramaProjection *projection);
#endif
