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

  /* A newly armed surface is not displayable merely because capture was
   * enabled. The click may already be released while a frozen redraw and GPU
   * upload complete; no authentic pixel is exposed before readiness. */
  RenderComparison_OnPress(1000, false);
  CHECK(!RenderComparison_IsTransitioning());
  CHECK(RenderComparison_IsAwaitingAuthenticFrame());
  CHECK(RenderComparison_FreezesGameplay());
  CHECK(RenderComparison_PresentView() == kRenderComparison_Enhanced);
  CHECK(RenderComparison_BaseView() == kRenderComparison_Authentic);
  CHECK(!RenderComparison_UsesAuthenticAudio());
  CHECK(RenderComparison_RequiresAuthenticFrame());
  RenderComparison_Tick(1050, false, false);
  CHECK(RenderComparison_IsAwaitingAuthenticFrame());
  RenderComparison_Tick(1100, false, true);
  CHECK(!RenderComparison_IsAwaitingAuthenticFrame());
  CHECK(RenderComparison_IsTransitioning());
  CHECK(RenderComparison_UsesAuthenticAudio());
  RenderComparison_Tick(1280, false, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_Authentic);
  CHECK(RenderComparison_TransitionFadeAlpha() == 255);
  RenderComparison_Tick(1460, false, true);
  CHECK(!RenderComparison_IsTransitioning());
  CHECK(RenderComparison_PresentView() == kRenderComparison_Authentic);

  RenderComparison_OnPress(2000, true);
  RenderComparison_Tick(2420, true, true);
  CHECK(RenderComparison_BaseView() == kRenderComparison_Enhanced);
  CHECK(RenderComparison_IsTransitioning());
  CHECK(!RenderComparison_UsesAuthenticAudio());
  RenderComparison_Tick(2780, true, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_SideBySide);
  CHECK(!RenderComparison_IsTransitioning());
  RenderComparison_Tick(2800, false, true);
  CHECK(RenderComparison_IsTransitioning());
  RenderComparison_Tick(3160, false, true);
  CHECK(RenderComparison_PresentView() == kRenderComparison_Enhanced);
  CHECK(!RenderComparison_RequiresAuthenticFrame());

  RenderComparison_OnPress(4000, false);
  CHECK(RenderComparison_IsAwaitingAuthenticFrame());
  RenderComparison_Tick(
      4000 + kRenderComparisonAuthenticWaitMilliseconds - 1, false, false);
  CHECK(!RenderComparison_AuthenticWaitExpired());
  RenderComparison_Tick(
      4000 + kRenderComparisonAuthenticWaitMilliseconds, false, false);
  CHECK(RenderComparison_AuthenticWaitExpired());
  CHECK(RenderComparison_FreezesGameplay());
  CHECK(RenderComparison_RequiresAuthenticFrame());

  if (s_failures) return 1;
  puts("render_comparison_test: PASS");
  return 0;
}
