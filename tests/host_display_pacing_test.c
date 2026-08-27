#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "host_display.h"
#include "host_display_pacing.h"

static const uint64_t kEmulationFrameIntervalNs = 16639267ull;
static const int kMaximumCatchupFrames = 3;
static int s_failure_count;

#define CHECK(expression) do { \
  if (!(expression)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expression); \
    s_failure_count++; \
  } \
} while (0)

static HostDisplayPacingOptions Options(RefreshMode mode, int limit_fps,
                                        int nominal_refresh_hz,
                                        bool compositor_managed) {
  return (HostDisplayPacingOptions){
      .refresh_mode = mode,
      .frame_limit_fps = limit_fps,
      .nominal_refresh_hz = nominal_refresh_hz,
      .compositor_managed = compositor_managed,
      .vsync_active = mode == kRefreshMode_Vsync,
  };
}

static void TestExplicitFrameLimits(void) {
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Limit, 25, 60, false)) ==
        40000000ull);
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Limit, 0, 60, false)) ==
        1000000000ull);
}

static void TestInterpolationSourceCadence(void) {
  CHECK(HostDisplayPacing_SourceFrameIntervalNs(
            kEmulationFrameIntervalNs, true, true,
            kInterpolationSource_Test30) ==
        kEmulationFrameIntervalNs * 2u);
  CHECK(HostDisplayPacing_SourceFrameIntervalNs(
            kEmulationFrameIntervalNs, false, true,
            kInterpolationSource_Test30) == kEmulationFrameIntervalNs);
  CHECK(HostDisplayPacing_SourceFrameIntervalNs(
            kEmulationFrameIntervalNs, true, false,
            kInterpolationSource_Test30) == kEmulationFrameIntervalNs);
  CHECK(HostDisplayPacing_SourceFrameIntervalNs(
            kEmulationFrameIntervalNs, true, true,
            kInterpolationSource_Native) == kEmulationFrameIntervalNs);
}

static void TestOnlyExplicitLimitOwnsFrameLimitInterval(void) {
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Uncapped, 60, 60, false)) == 0);
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Unlimited, 60, 0, false)) == 0);
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Unlimited, 60, 90, true)) == 0);
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Vsync, 60, 60, false)) == 0);
}

static void TestAllowedFramesInFlightPolicy(void) {
  CHECK(HostDisplayPacing_AllowedFramesInFlight(kRefreshMode_Vsync) == 1);
  CHECK(HostDisplayPacing_AllowedFramesInFlight(kRefreshMode_Uncapped) == 2);
  CHECK(HostDisplayPacing_AllowedFramesInFlight(kRefreshMode_Limit) == 2);
  CHECK(HostDisplayPacing_AllowedFramesInFlight(kRefreshMode_Unlimited) == 2);
  CHECK(HostDisplayPacing_AllowedFramesInFlight(kRefreshMode_Count) == 1);
}

static void TestUiAndPausedIntervals(void) {
  const HostDisplayPacingOptions vsync_144 =
      Options(kRefreshMode_Vsync, 60, 144, false);
  CHECK(HostDisplayPacing_UiIntervalNs(
            vsync_144, kEmulationFrameIntervalNs) == 0);
  CHECK(HostDisplayPacing_PausedIntervalNs(
            vsync_144, kEmulationFrameIntervalNs) == 0);

  HostDisplayPacingOptions rejected_vsync = vsync_144;
  rejected_vsync.vsync_active = false;
  CHECK(HostDisplayPacing_UiIntervalNs(
            rejected_vsync, kEmulationFrameIntervalNs) ==
        kEmulationFrameIntervalNs);

  const HostDisplayPacingOptions uncapped_30 =
      Options(kRefreshMode_Uncapped, 60, 30, false);
  CHECK(HostDisplayPacing_UiIntervalNs(
            uncapped_30, kEmulationFrameIntervalNs) ==
        16666666ull);
  CHECK(HostDisplayPacing_PausedIntervalNs(
            uncapped_30, kEmulationFrameIntervalNs) ==
        16666666ull);

  const HostDisplayPacingOptions limited_120 =
      Options(kRefreshMode_Limit, 120, 60, false);
  CHECK(HostDisplayPacing_UiIntervalNs(
            limited_120, kEmulationFrameIntervalNs) ==
        8333333ull);
  CHECK(HostDisplayPacing_PausedIntervalNs(
            limited_120, kEmulationFrameIntervalNs) ==
        8333333ull);

  const HostDisplayPacingOptions unlimited =
      Options(kRefreshMode_Unlimited, 60, 60, false);
  CHECK(HostDisplayPacing_UiIntervalNs(
            unlimited, kEmulationFrameIntervalNs) == 0);
  CHECK(HostDisplayPacing_PausedIntervalNs(
            unlimited, kEmulationFrameIntervalNs) == 0);

  const HostDisplayPacingOptions uncapped_unknown =
      Options(kRefreshMode_Uncapped, 60, 0, false);
  CHECK(HostDisplayPacing_UiIntervalNs(
            uncapped_unknown, kEmulationFrameIntervalNs) ==
        kEmulationFrameIntervalNs / 2u);
  CHECK(HostDisplayPacing_GameIntervalNs(
            uncapped_unknown, kEmulationFrameIntervalNs) ==
        kEmulationFrameIntervalNs / 2u);

  const HostDisplayPacingOptions gamescope =
      Options(kRefreshMode_Uncapped, 60, 90, true);
  CHECK(HostDisplayPacing_UiIntervalNs(
            gamescope, kEmulationFrameIntervalNs) ==
        kEmulationFrameIntervalNs / 2u);
}

static void TestGamePresentAntiSpinFloor(void) {
  CHECK(HostDisplayPacing_GameIntervalNs(
            Options(kRefreshMode_Limit, 25, 60, false),
            kEmulationFrameIntervalNs) ==
        40000000ull);
  CHECK(HostDisplayPacing_GameIntervalNs(
            Options(kRefreshMode_Uncapped, 60, 60, false),
            kEmulationFrameIntervalNs) ==
        8333333ull);
  CHECK(HostDisplayPacing_GameIntervalNs(
            Options(kRefreshMode_Vsync, 60, 60, false),
            kEmulationFrameIntervalNs) == 0);
  CHECK(HostDisplayPacing_GameIntervalNs(
            Options(kRefreshMode_Unlimited, 60, 60, false),
            kEmulationFrameIntervalNs) == 0);
}

static void TestCompletedPresentRate(void) {
  HostDisplayFpsCounter counter = {0};
  const uint64_t start_ns = 1000000000ull;
  HostDisplayPacing_RecordPresent(&counter, start_ns);
  CHECK(HostDisplayPacing_FramesPerSecond(&counter) == 0.0);
  for (int interval = 1; interval <= 125; interval++)
    HostDisplayPacing_RecordPresent(
        &counter, start_ns + (uint64_t)interval * 4000000ull);
  const double fps = HostDisplayPacing_FramesPerSecond(&counter);
  CHECK(fps > 249.99 && fps < 250.01);

  /* A reset/non-monotonic platform clock restarts the sampling window without
   * publishing an invalid negative/overflowed rate. */
  HostDisplayPacing_RecordPresent(&counter, start_ns);
  CHECK(HostDisplayPacing_FramesPerSecond(&counter) == fps);

  HostDisplayPacing_ResetFpsCounter(&counter);
  CHECK(HostDisplayPacing_FramesPerSecond(&counter) == 0.0);
  CHECK(!counter.initialized);
}

static void TestRepresentPolicy(void) {
  static const RefreshMode modes[] = {
    kRefreshMode_Vsync,
    kRefreshMode_Unlimited,
    kRefreshMode_Limit,
    kRefreshMode_Uncapped,
  };
  for (size_t index = 0; index < sizeof(modes) / sizeof(modes[0]); index++) {
    CHECK(HostDisplayPacing_ShouldRepresentFrame(modes[index], false));
    CHECK(!HostDisplayPacing_ShouldRepresentFrame(modes[index], true));
  }
  CHECK(!HostDisplayPacing_ShouldRepresentFrame(kRefreshMode_Count, false));
}

static void TestEmulatedFramePresentModes(void) {
  CHECK(HostDisplay_EmulatedFramePresentMode(false, false) ==
        kHostDisplayPresent_GameTick);
  CHECK(HostDisplay_EmulatedFramePresentMode(false, true) ==
        kHostDisplayPresent_GameTick);
  CHECK(HostDisplay_EmulatedFramePresentMode(true, false) ==
        kHostDisplayPresent_None);
  CHECK(HostDisplay_EmulatedFramePresentMode(true, true) ==
        kHostDisplayPresent_HeadlessVideo);
}

static void TestSub60LimitsRetainElapsedTime(void) {
  static const int limits[] = {59, 30, 25, 20, 15};
  const uint64_t ordinary_cap_ns =
      kEmulationFrameIntervalNs * kMaximumCatchupFrames;

  for (size_t index = 0; index < sizeof(limits) / sizeof(limits[0]);
       index++) {
    const HostDisplayPacingOptions options =
        Options(kRefreshMode_Limit, limits[index], 60, false);
    const uint64_t limited_interval_ns =
        HostDisplayPacing_FrameLimitIntervalNs(options);
    const uint64_t catchup_cap_ns =
        HostDisplayPacing_CatchupCapNs(
            options, kEmulationFrameIntervalNs, kMaximumCatchupFrames);

    CHECK(catchup_cap_ns >= ordinary_cap_ns);
    CHECK(catchup_cap_ns >=
          limited_interval_ns + kEmulationFrameIntervalNs);
  }

  /* Display-relative presentation policy must never enlarge the emulation
   * accumulator. Even an absurd 1 Hz nominal display stays presentation-only. */
  const HostDisplayPacingOptions uncapped =
      Options(kRefreshMode_Uncapped, 60, 1, false);
  CHECK(HostDisplayPacing_CatchupCapNs(
            uncapped, kEmulationFrameIntervalNs, kMaximumCatchupFrames) ==
        ordinary_cap_ns);
  const HostDisplayPacingOptions unlimited =
      Options(kRefreshMode_Unlimited, 60, 1, false);
  CHECK(HostDisplayPacing_CatchupCapNs(
            unlimited, kEmulationFrameIntervalNs, kMaximumCatchupFrames) ==
        ordinary_cap_ns);
}

/* W4-5: window-client coordinate -> renderer-output pixel.
 *
 * The property that matters is that the result is ALWAYS a valid index into the
 * output extent, for every input the window can produce and at every ratio.
 * Round-to-nearest alone satisfies that when magnifying but overshoots by one on
 * the final row/column once the output is half the window or smaller — the exact
 * ratio a 2x render-resolution reduction produces. */
static void TestWindowAxisToOutput(void) {
  /* Identity: every position maps to itself. */
  for (int position = 0; position < 8; position++)
    CHECK(HostDisplayPacing_WindowAxisToOutput(position, 8, 8) == position);

  /* The last valid window pixel must map in range at every ratio, including the
   * minifying ones where the unclamped formula returns `extent`. */
  const struct { int window; int output; } ratios[] = {
    { 1280, 2560 },   /* 2x high-DPI magnify */
    { 1280, 1920 },   /* 1.5x magnify */
    { 1920, 1280 },   /* 1.5x minify — never overshot even unclamped */
    { 2560, 1280 },   /* 2x minify — the overshooting case */
    { 3840, 1080 },   /* 3.56x minify */
    { 2560,  640 },   /* 4x minify */
    {    1,    1 },   /* degenerate single pixel */
    {    7,    3 },   /* non-integer, odd sizes */
  };
  for (size_t index = 0; index < sizeof ratios / sizeof *ratios; index++) {
    const int window = ratios[index].window;
    const int output = ratios[index].output;
    for (int position = 0; position < window; position++) {
      const int mapped =
          HostDisplayPacing_WindowAxisToOutput(position, window, output);
      CHECK(mapped >= 0);
      CHECK(mapped <= output - 1);
    }
    /* Monotone: a larger window position never maps to a smaller output pixel. */
    int previous = -1;
    for (int position = 0; position < window; position++) {
      const int mapped =
          HostDisplayPacing_WindowAxisToOutput(position, window, output);
      CHECK(mapped >= previous);
      previous = mapped;
    }
    /* The first window pixel lands on the first output pixel. The LAST one is
     * deliberately not asserted to be output-1: when magnifying, one window
     * pixel spans several output pixels and round-to-nearest picks the first of
     * them (window 1279 of 1280 -> 2558 of 2560, covering 2558..2559), which is
     * correct. What must hold is only that it is in range and is the largest
     * value produced — both already asserted above. */
    CHECK(HostDisplayPacing_WindowAxisToOutput(0, window, output) == 0);
    CHECK(HostDisplayPacing_WindowAxisToOutput(window - 1, window, output) >=
          HostDisplayPacing_WindowAxisToOutput(window - 2 > 0 ? window - 2 : 0,
                                               window, output));
    /* Minifying, the last window pixel DOES reach the last output pixel — and
     * this is the case the unclamped formula overshot. */
    if (output <= window)
      CHECK(HostDisplayPacing_WindowAxisToOutput(window - 1, window, output) ==
            output - 1);
  }

  /* Out-of-window inputs are clamped, not propagated: SDL_mouse.h warns that
   * mouse coordinates may fall outside the window entirely. */
  CHECK(HostDisplayPacing_WindowAxisToOutput(-50, 1280, 720) == 0);
  CHECK(HostDisplayPacing_WindowAxisToOutput(99999, 1280, 720) == 719);

  /* A non-positive extent yields 0 rather than dividing by zero. */
  CHECK(HostDisplayPacing_WindowAxisToOutput(10, 0, 720) == 0);
  CHECK(HostDisplayPacing_WindowAxisToOutput(10, 1280, 0) == 0);
  CHECK(HostDisplayPacing_WindowAxisToOutput(10, -4, -4) == 0);
}

int main(void) {
  TestInterpolationSourceCadence();
  TestExplicitFrameLimits();
  TestOnlyExplicitLimitOwnsFrameLimitInterval();
  TestAllowedFramesInFlightPolicy();
  TestUiAndPausedIntervals();
  TestGamePresentAntiSpinFloor();
  TestCompletedPresentRate();
  TestRepresentPolicy();
  TestEmulatedFramePresentModes();
  TestSub60LimitsRetainElapsedTime();
  TestWindowAxisToOutput();
  if (s_failure_count) {
    fprintf(stderr, "host_display_pacing_test: %d failure(s)\n",
            s_failure_count);
    return 1;
  }
  puts("host_display_pacing_test: all tests passed");
  return 0;
}
