#include "render_comparison.h"

#include <stdio.h>

static int s_failures;
#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

int main(void) {
  RenderComparison_Reset();
  CHECK(RenderComparison_PresentView() == kRenderComparison_Enhanced);
  CHECK(!RenderComparison_IsTransitioning());
  CHECK(!RenderComparison_RequiresAuthenticFrame());

  /* Press alone has no presentation effect: intent is resolved from duration
   * on release or when the hold threshold is crossed. */
  RenderComparison_OnPress(1000);
  RenderComparison_OnPress(1001);  /* noisy duplicate press */
  CHECK(!RenderComparison_IsTransitioning());
  CHECK(!RenderComparison_IsAwaitingAuthenticFrame());
  CHECK(!RenderComparison_FreezesGameplay());
  CHECK(RenderComparison_PresentView() == kRenderComparison_Enhanced);
  CHECK(RenderComparison_BaseView() == kRenderComparison_Enhanced);
  CHECK(!RenderComparison_UsesAuthenticAudio());

  /* A short release commits the base-view swap. A newly armed surface is not
   * displayable merely because capture was enabled, so the state freezes only
   * after click intent is known and waits for the current native upload. */
  RenderComparison_Tick(1050, false, false);
  CHECK(RenderComparison_IsAwaitingAuthenticFrame());
  CHECK(RenderComparison_FreezesGameplay());
  CHECK(RenderComparison_BaseView() == kRenderComparison_Authentic);
  CHECK(RenderComparison_RequiresAuthenticFrame());
  RenderComparison_Tick(1100, false, true);
  CHECK(!RenderComparison_IsAwaitingAuthenticFrame());
  CHECK(RenderComparison_IsTransitioning());
  CHECK(RenderComparison_UsesAuthenticAudio());
  CHECK(RenderComparison_TransitionTargetView() ==
        kRenderComparison_Authentic);
  RenderComparison_Tick(1280, false, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_Enhanced);
  CHECK(RenderComparison_TransitionFadeAlpha() == 255);
  RenderComparison_Tick(1550, false, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_Authentic);
  CHECK(RenderComparison_TransitionFadeAlpha() == 255);
  RenderComparison_Tick(2000, false, true);
  CHECK(!RenderComparison_IsTransitioning());
  CHECK(RenderComparison_PresentView() == kRenderComparison_Authentic);

  RenderComparison_OnPress(2200);
  /* Even a broken/noisy backend that dispatches repeated press callbacks
   * cannot toggle again until Tick observes a physical release. */
  RenderComparison_OnPress(2201);
  CHECK(RenderComparison_BaseView() == kRenderComparison_Authentic);
  RenderComparison_Tick(2620, true, true);
  CHECK(RenderComparison_BaseView() == kRenderComparison_Authentic);
  CHECK(RenderComparison_IsTransitioning());
  CHECK(!RenderComparison_UsesAuthenticAudio());
  RenderComparison_Tick(3520, true, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_SideBySide);
  CHECK(!RenderComparison_IsTransitioning());
  RenderComparison_OnPress(3530);
  CHECK(RenderComparison_BaseView() == kRenderComparison_Authentic);
  CHECK(RenderComparison_PresentView() == kRenderComparison_SideBySide);
  RenderComparison_Tick(3540, false, true);
  CHECK(!RenderComparison_IsTransitioning());
  CHECK(RenderComparison_PresentView() == kRenderComparison_SideBySide);

  /* Release leaves PiP latched. A later short click clears it and performs the
   * ordinary authentic/enhanced swap exactly once. */
  RenderComparison_OnPress(3700);
  RenderComparison_Tick(3750, false, true);
  CHECK(RenderComparison_BaseView() == kRenderComparison_Enhanced);
  CHECK(RenderComparison_IsTransitioning());
  RenderComparison_Tick(4650, false, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_Enhanced);
  CHECK(!RenderComparison_RequiresAuthenticFrame());

  /* Long hold is itself a PiP toggle. Its release never changes the result,
   * and a second long hold toggles the latch back to the unchanged base. */
  RenderComparison_OnPress(4800);
  RenderComparison_Tick(5220, true, true);
  RenderComparison_Tick(6120, true, true);
  RenderComparison_Tick(6140, false, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_SideBySide);
  RenderComparison_OnPress(6300);
  RenderComparison_Tick(6720, true, true);
  RenderComparison_Tick(7620, true, true);
  RenderComparison_Tick(7640, false, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_Enhanced);
  CHECK(RenderComparison_BaseView() == kRenderComparison_Enhanced);

  RenderComparison_OnPress(8000);
  RenderComparison_Tick(8050, false, false);
  CHECK(RenderComparison_IsAwaitingAuthenticFrame());
  RenderComparison_Tick(
      8050 + kRenderComparisonAuthenticWaitMilliseconds - 1, false, false);
  CHECK(!RenderComparison_AuthenticWaitExpired());
  RenderComparison_Tick(
      8050 + kRenderComparisonAuthenticWaitMilliseconds, false, false);
  CHECK(RenderComparison_AuthenticWaitExpired());
  CHECK(RenderComparison_FreezesGameplay());
  CHECK(RenderComparison_RequiresAuthenticFrame());

  if (s_failures) return 1;
  puts("render_comparison_test: PASS");
  return 0;
}
