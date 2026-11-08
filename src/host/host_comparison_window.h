#ifndef AR_HOST_COMPARISON_WINDOW_H
#define AR_HOST_COMPARISON_WINDOW_H
/* Owns the optional authentic comparison window on the main render thread.
 * Both windows share the GPU device; all settings changes require runner
 * ownership, just like the ordinary settings overlay. */

#include <stdbool.h>

union SDL_Event;
struct FrameSlot;

/* Reconcile visibility/fullscreen and prepare the output target. False means
 * use the ordinary inset (including unsupported/headless/failure cases). */
bool HostComparisonWindow_Prepare(bool active);
bool HostComparisonWindow_Draw(const struct FrameSlot *slot, bool blank);
/* OwnsEvent is read-only and safe while the producer runs. HandleEvent must
 * run after the producer yields, before ordinary input/window routing. */
bool HostComparisonWindow_OwnsEvent(const union SDL_Event *event);
bool HostComparisonWindow_HandleEvent(const union SDL_Event *event);
void HostComparisonWindow_Destroy(void);

#endif /* AR_HOST_COMPARISON_WINDOW_H */
