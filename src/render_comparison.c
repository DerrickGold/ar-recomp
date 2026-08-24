#include "render_comparison.h"

typedef struct RenderComparisonState {
  RenderComparisonView base_view;
  RenderComparisonView settled_view;
  RenderComparisonView transition_from;
  RenderComparisonView transition_to;
  uint64_t transition_started_ms;
  uint64_t now_ms;
  uint64_t pressed_ms;
  bool transitioning;
  bool awaiting_authentic_frame;
  bool pressed;
  bool hold_engaged;
} RenderComparisonState;

static RenderComparisonState s_compare;

const char *RenderComparison_ViewName(RenderComparisonView view) {
  switch (view) {
    case kRenderComparison_Authentic: return "AUTHENTIC";
    case kRenderComparison_SideBySide: return "COMPARE";
    case kRenderComparison_Enhanced:
    default: return "ENHANCED";
  }
}

void RenderComparison_Reset(void) {
  s_compare = (RenderComparisonState){
    .base_view = kRenderComparison_Enhanced,
    .settled_view = kRenderComparison_Enhanced,
    .transition_from = kRenderComparison_Enhanced,
    .transition_to = kRenderComparison_Enhanced,
  };
}

static RenderComparisonView VisibleView(void) {
  if (!s_compare.transitioning)
    return s_compare.settled_view;
  const uint64_t elapsed = s_compare.now_ms - s_compare.transition_started_ms;
  return elapsed < kRenderComparisonTransitionMilliseconds / 2
      ? s_compare.transition_from : s_compare.transition_to;
}

static void BeginTransition(RenderComparisonView target, uint64_t now_ms) {
  s_compare.now_ms = now_ms;
  const RenderComparisonView from = VisibleView();
  if (!s_compare.transitioning && from == target) return;
  s_compare.transition_from = from;
  s_compare.transition_to = target;
  s_compare.transition_started_ms = now_ms;
  s_compare.transitioning = true;
}

void RenderComparison_OnPress(uint64_t now_ms, bool authentic_frame_ready) {
  if (s_compare.awaiting_authentic_frame) return;
  s_compare.now_ms = now_ms;
  s_compare.pressed = true;
  s_compare.hold_engaged = false;
  s_compare.pressed_ms = now_ms;
  s_compare.base_view = s_compare.base_view == kRenderComparison_Enhanced
      ? kRenderComparison_Authentic : kRenderComparison_Enhanced;
  /* A click takes effect immediately only when the independently rendered
   * frame has completed and uploaded. Otherwise the host freezes gameplay and
   * asks for one redraw while the enhanced view remains visible. */
  if (authentic_frame_ready)
    BeginTransition(s_compare.base_view, now_ms);
  else
    s_compare.awaiting_authentic_frame = true;
}

void RenderComparison_Tick(uint64_t now_ms, bool control_held,
                           bool authentic_frame_ready) {
  s_compare.now_ms = now_ms;
  if (s_compare.awaiting_authentic_frame) {
    if (!control_held) s_compare.pressed = false;
    if (!authentic_frame_ready) return;
    s_compare.awaiting_authentic_frame = false;
    BeginTransition(s_compare.base_view, now_ms);
  }
  if (s_compare.transitioning &&
      now_ms - s_compare.transition_started_ms >=
          kRenderComparisonTransitionMilliseconds) {
    s_compare.settled_view = s_compare.transition_to;
    s_compare.transitioning = false;
  }

  if (!s_compare.pressed) return;
  if (!control_held) {
    s_compare.pressed = false;
    if (s_compare.hold_engaged)
      BeginTransition(s_compare.base_view, now_ms);
    return;
  }
  if (!s_compare.hold_engaged &&
      now_ms - s_compare.pressed_ms >=
          kRenderComparisonHoldMilliseconds) {
    s_compare.hold_engaged = true;
    BeginTransition(kRenderComparison_SideBySide, now_ms);
  }
}

RenderComparisonView RenderComparison_PresentView(void) {
  return VisibleView();
}

RenderComparisonView RenderComparison_BaseView(void) {
  return s_compare.base_view;
}

bool RenderComparison_IsTransitioning(void) {
  return s_compare.transitioning;
}

bool RenderComparison_IsAwaitingAuthenticFrame(void) {
  return s_compare.awaiting_authentic_frame;
}

bool RenderComparison_AuthenticWaitExpired(void) {
  return s_compare.awaiting_authentic_frame &&
      s_compare.now_ms - s_compare.pressed_ms >=
          kRenderComparisonAuthenticWaitMilliseconds;
}

bool RenderComparison_FreezesGameplay(void) {
  return s_compare.awaiting_authentic_frame || s_compare.transitioning;
}

bool RenderComparison_RequiresAuthenticFrame(void) {
  if (s_compare.awaiting_authentic_frame || s_compare.transitioning)
    return true;
  return s_compare.settled_view != kRenderComparison_Enhanced ||
      s_compare.base_view != kRenderComparison_Enhanced;
}

uint8_t RenderComparison_TransitionFadeAlpha(void) {
  if (!s_compare.transitioning) return 0;
  uint64_t elapsed = s_compare.now_ms - s_compare.transition_started_ms;
  if (elapsed >= kRenderComparisonTransitionMilliseconds) return 0;
  const uint64_t half = kRenderComparisonTransitionMilliseconds / 2;
  uint64_t distance = elapsed <= half ? elapsed
                                      : kRenderComparisonTransitionMilliseconds - elapsed;
  return (uint8_t)((distance * 255u + half / 2u) / half);
}

bool RenderComparison_UsesAuthenticAudio(void) {
  /* Audio is paused for the complete transition, so selecting its destination
   * now makes the handoff complete before the device resumes. */
  const RenderComparisonView view = s_compare.transitioning
      ? s_compare.transition_to : s_compare.settled_view;
  return view == kRenderComparison_Authentic;
}
