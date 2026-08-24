#ifndef AR_RENDER_COMPARISON_H
#define AR_RENDER_COMPARISON_H

#include <stdbool.h>
#include <stdint.h>

/* Session-only presentation selector. It deliberately lives outside Settings:
 * the player's enhanced configuration remains untouched and the default is
 * restored automatically on every process launch. */
typedef enum RenderComparisonView {
  kRenderComparison_Enhanced = 0,
  kRenderComparison_Authentic,
  kRenderComparison_SideBySide,
} RenderComparisonView;

enum {
  kRenderComparisonHoldMilliseconds = 420,
  kRenderComparisonTransitionMilliseconds = 360,
  /* A frozen redraw/upload should finish in a handful of frames. A generous
   * bound accommodates a device-reset redraw without freezing gameplay
   * forever when the native pass can no longer satisfy its contract. */
  kRenderComparisonAuthenticWaitMilliseconds = 2000,
};

void RenderComparison_Reset(void);
/* A comparison press cannot expose the authentic texture until a completed
 * native PPU pass with the current geometry has reached the GPU. When false,
 * the state machine freezes in an enhanced-only wait state. */
void RenderComparison_OnPress(uint64_t now_ms, bool authentic_frame_ready);
void RenderComparison_Tick(uint64_t now_ms, bool control_held,
                           bool authentic_frame_ready);

RenderComparisonView RenderComparison_PresentView(void);
RenderComparisonView RenderComparison_BaseView(void);
bool RenderComparison_IsTransitioning(void);
bool RenderComparison_IsAwaitingAuthenticFrame(void);
bool RenderComparison_AuthenticWaitExpired(void);
bool RenderComparison_FreezesGameplay(void);
/* True while any current/transition endpoint can display the authentic PPU
 * pass. Hosts combine this with configured bindings to arm capture early. */
bool RenderComparison_RequiresAuthenticFrame(void);
/* Triangular fade: transparent at the ends and opaque at the view swap. */
uint8_t RenderComparison_TransitionFadeAlpha(void);
/* Authentic is the only audio override. Side-by-side intentionally keeps the
 * player's enhanced audio, as does the enhanced half of a transition. */
bool RenderComparison_UsesAuthenticAudio(void);
const char *RenderComparison_ViewName(RenderComparisonView view);

#endif /* AR_RENDER_COMPARISON_H */
