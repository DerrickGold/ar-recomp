#include "render_comparison.h"

typedef struct RenderComparisonState {
  RenderComparisonView base_view;
  RenderComparisonView settled_view;
  RenderComparisonView transition_from;
  RenderComparisonView transition_to;
  uint64_t transition_started_ms;
  uint64_t now_ms;
  uint64_t pressed_ms;
  uint64_t authentic_wait_started_ms;
  RenderComparisonView awaiting_view;
  bool transitioning;
  bool awaiting_authentic_frame;
  bool pressed;
  bool hold_engaged;
  bool pip_latched;
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

static void RequestView(RenderComparisonView target, uint64_t now_ms,
                        bool authentic_frame_ready) {
  if (target == kRenderComparison_Enhanced || authentic_frame_ready) {
    BeginTransition(target, now_ms);
    return;
  }
  s_compare.awaiting_view = target;
  s_compare.authentic_wait_started_ms = now_ms;
  s_compare.awaiting_authentic_frame = true;
}

static void TogglePip(uint64_t now_ms, bool authentic_frame_ready) {
  s_compare.pip_latched = !s_compare.pip_latched;
  RequestView(s_compare.pip_latched
                  ? kRenderComparison_SideBySide
                  : s_compare.base_view,
              now_ms, authentic_frame_ready);
}

void RenderComparison_OnPress(uint64_t now_ms) {
  /* Input backends are allowed to be noisy; presentation state is not. Once
   * one press owns the comparison control, no further press callback may
   * mutate the base view until Tick observes the control physically released.
   * This is the final contract boundary beneath SDL repeat filtering and the
   * input-map edge latches. */
  if (s_compare.pressed || s_compare.awaiting_authentic_frame) return;
  s_compare.now_ms = now_ms;
  s_compare.pressed = true;
  s_compare.hold_engaged = false;
  s_compare.pressed_ms = now_ms;
}

void RenderComparison_Tick(uint64_t now_ms, bool control_held,
                           bool authentic_frame_ready) {
  s_compare.now_ms = now_ms;
  if (s_compare.transitioning &&
      now_ms - s_compare.transition_started_ms >=
          kRenderComparisonTransitionMilliseconds) {
    s_compare.settled_view = s_compare.transition_to;
    s_compare.transitioning = false;
  }

  if (s_compare.pressed && !control_held) {
    const bool was_hold = s_compare.hold_engaged ||
        now_ms - s_compare.pressed_ms >=
            kRenderComparisonHoldMilliseconds;
    s_compare.pressed = false;
    if (was_hold && !s_compare.hold_engaged) {
      /* A low host refresh rate may observe the release after the threshold
       * without ever ticking the still-held state. Resolve that as the same
       * long-hold intent, never as a click. */
      TogglePip(now_ms, authentic_frame_ready);
    } else if (!was_hold) {
      s_compare.pip_latched = false;
      s_compare.base_view =
          s_compare.base_view == kRenderComparison_Enhanced
              ? kRenderComparison_Authentic
              : kRenderComparison_Enhanced;
      RequestView(s_compare.base_view, now_ms, authentic_frame_ready);
    }
    /* A recognized hold already toggled the persistent PiP latch at the
     * threshold. Its release is intentionally a no-op. */
    s_compare.hold_engaged = false;
  }

  if (s_compare.awaiting_authentic_frame) {
    if (!authentic_frame_ready) return;
    const RenderComparisonView target = s_compare.awaiting_view;
    s_compare.awaiting_authentic_frame = false;
    BeginTransition(target, now_ms);
  }

  if (s_compare.pressed && control_held && !s_compare.hold_engaged &&
      now_ms - s_compare.pressed_ms >=
          kRenderComparisonHoldMilliseconds) {
    s_compare.hold_engaged = true;
    TogglePip(now_ms, authentic_frame_ready);
  }
}

RenderComparisonView RenderComparison_PresentView(void) {
  return VisibleView();
}

RenderComparisonView RenderComparison_BaseView(void) {
  return s_compare.base_view;
}

RenderComparisonView RenderComparison_TransitionTargetView(void) {
  return s_compare.transitioning
      ? s_compare.transition_to : s_compare.settled_view;
}

bool RenderComparison_IsTransitioning(void) {
  return s_compare.transitioning;
}

bool RenderComparison_IsAwaitingAuthenticFrame(void) {
  return s_compare.awaiting_authentic_frame;
}

bool RenderComparison_AuthenticWaitExpired(void) {
  return s_compare.awaiting_authentic_frame &&
      s_compare.now_ms - s_compare.authentic_wait_started_ms >=
          kRenderComparisonAuthenticWaitMilliseconds;
}

bool RenderComparison_FreezesGameplay(void) {
  return s_compare.awaiting_authentic_frame || s_compare.transitioning;
}

bool RenderComparison_RequiresAuthenticFrame(void) {
  if (s_compare.awaiting_authentic_frame || s_compare.transitioning)
    return true;
  return s_compare.settled_view != kRenderComparison_Enhanced ||
      s_compare.base_view != kRenderComparison_Enhanced ||
      s_compare.pip_latched;
}

uint8_t RenderComparison_TransitionFadeAlpha(void) {
  if (!s_compare.transitioning) return 0;
  const uint64_t elapsed =
      s_compare.now_ms - s_compare.transition_started_ms;
  if (elapsed >= kRenderComparisonTransitionMilliseconds) return 0;
  if (elapsed < kRenderComparisonTransitionFadeMilliseconds) {
    return (uint8_t)(
        (elapsed * 255u + kRenderComparisonTransitionFadeMilliseconds / 2u) /
        kRenderComparisonTransitionFadeMilliseconds);
  }
  const uint64_t fade_in_started =
      kRenderComparisonTransitionFadeMilliseconds +
      kRenderComparisonTransitionBlackMilliseconds;
  if (elapsed < fade_in_started) return 255;
  const uint64_t remaining =
      kRenderComparisonTransitionMilliseconds - elapsed;
  return (uint8_t)(
      (remaining * 255u + kRenderComparisonTransitionFadeMilliseconds / 2u) /
      kRenderComparisonTransitionFadeMilliseconds);
}

bool RenderComparison_UsesAuthenticAudio(void) {
  /* Audio is paused for the complete transition, so selecting its destination
   * now makes the handoff complete before the device resumes. */
  const RenderComparisonView view = s_compare.transitioning
      ? s_compare.transition_to : s_compare.settled_view;
  return view == kRenderComparison_Authentic;
}
