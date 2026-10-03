#ifndef AR_PRESENT_ACTION_EFFECTS_H
#define AR_PRESENT_ACTION_EFFECTS_H
/* Action-stage visual effects: mask uploads, ordered plane/actor draws, heat
 * refraction and resource reset. All scene inputs are captured FrameSlot data. */

#include "render/render_device.h"

typedef struct FrameSlot FrameSlot;
typedef struct DioramaProjection DioramaProjection;
typedef struct ActionEffectSourceBatch ActionEffectSourceBatch;
typedef struct ActionMoonlightOcclusion ActionMoonlightOcclusion;
typedef bool (*PresentActionSourceDraw)(ArRenderDevice *, const ActionEffectSourceBatch *,
    const DioramaProjection *, const ActionMoonlightOcclusion *, ArRenderBlendMode, float);

/* Optional BG1 scenery dimming, 0..1. Shared by flat masks and Diorama's
 * existing layer colors; requires this frame's validated environmental field. */
float PresentActionEffects_Bg1Dimming(const FrameSlot *slot);
/* World-space transition to the darker lower temple, or zero for uniform rooms. */
ArRenderRectF PresentActionEffects_Bg1DimmingRamp(const FrameSlot *slot);

/* Borrow validated winner-mask pixels only during FrameSlot upload. Return
 * bytes uploaded for the caller's traffic accounting; retained draws use the
 * owned GPU textures and never read these borrowed pixels again. */
uint64_t PresentActionEffects_UploadMask(
    ArRenderDevice *device, int plane, const FrameSlot *slot,
    const uint8_t *pixels, int pitch_bytes);

/* False means target state was lost and fatal reporting is already latched.
 * Stop drawing the scene; an omitted optional effect still returns true. */
bool PresentActionEffects_DrawFlatPlanes(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport);
void PresentActionEffects_Draw(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport,
    const DioramaProjection *diorama_projection);
void PresentActionEffects_DrawWithSource(
    ArRenderDevice *, const FrameSlot *, ArRenderRectI,
    const DioramaProjection *, PresentActionSourceDraw);

/* Stack-owned for the synchronous Diorama_Composite plane callback. */
typedef struct PresentActionPlaneEffectContext {
  ArRenderDevice *device;
  const FrameSlot *slot;
  ArRenderRectI viewport;
  PresentActionSourceDraw source_draw;
} PresentActionPlaneEffectContext;
void PresentActionEffects_DrawDioramaPlane(
    void *userdata, int plane, const DioramaProjection *diorama_projection);

/* Bracket the world scene; resolve heat before drawing HUD/host overlays.
 * Every successful Begin requires End or Cancel. The scene viewport becomes
 * target-local only while a heat target is bound. Disabled heat is a no-op. */
bool PresentActionHeat_Begin(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport);
ArRenderRectI PresentActionHeat_SceneViewport(ArRenderRectI output_viewport);
void PresentActionHeat_Cancel(ArRenderDevice *device);
void PresentActionHeat_End(
    ArRenderDevice *device, const FrameSlot *slot, ArRenderRectI viewport);

/* Invalidate before uploading/releasing a retained capture, including captures
 * with an unchanged timestamp (save-state loads and editor changes). */
void PresentActionEffects_InvalidateSourcePackets(void);

/* Called on render-device reset and shutdown, outside any active pass. */
void PresentActionEffects_Reset(ArRenderDevice *device);

#endif /* AR_PRESENT_ACTION_EFFECTS_H */
