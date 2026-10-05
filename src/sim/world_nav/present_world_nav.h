#ifndef AR_PRESENT_WORLD_NAV_H
#define AR_PRESENT_WORLD_NAV_H
/* World-map navigation presentation and its resource lifetime. Sky Palace can
 * reuse the globe backdrop without composing the navigation screen's UI. */

#include "present/presentation_outcome.h"
#include "render/render_types.h"
struct FrameSlot;
/* Borrow projected BG1/OBJ artwork for this draw's water reflection and rim.
 * The action presenter owns the target; no borrowed handle is retained here. */
typedef struct DeathHeimCompletionArt {
  ArRenderTexture texture;
  ArRenderPointF waterline;
} DeathHeimCompletionArt;

PresentationOutcome PresentWorldNavigation3D(const struct FrameSlot *slot);
/* Draw into the caller's local viewport without changing output target or
 * viewport. A core failure aborts the scene instead of exposing native pixels. */
PresentationOutcome PresentWorldNavigationBackdrop(const struct FrameSlot *slot,
                                                   ArRenderRectI viewport);
PresentationOutcome PresentDeathHeimCompletionBackdrop(const struct FrameSlot *slot,
                                                       ArRenderRectI viewport,
                                                       ArRenderPointF player,
                                                       DeathHeimCompletionArt art);
PresentationOutcome PresentDeathHeimCompletionForeground(const struct FrameSlot *slot,
                                                         ArRenderRectI viewport,
                                                         ArRenderPointF player,
                                                         DeathHeimCompletionArt art);
void PresentWorldNav_ResetResources(void);
/* Prepare fixed ending cloud artwork at boot/reset without reading game state.
 * Failure retains lazy preparation and ordinary backdrop fallback. */
bool PresentWorldNav_PrepareCompletionResources(void);
#endif
