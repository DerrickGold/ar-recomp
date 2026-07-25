#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
                                        int host_refresh_hz,
                                        bool compositor_managed) {
  return (HostDisplayPacingOptions){
      .refresh_mode = mode,
      .frame_limit_fps = limit_fps,
      .host_refresh_hz = host_refresh_hz,
      .compositor_managed = compositor_managed,
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

static void TestUnlimitedAndVsyncPolicies(void) {
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Unlimited, 60, 60, false)) ==
        8333333ull);
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Unlimited, 60, 0, false)) == 0);
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Unlimited, 60, 90, true)) == 0);
  CHECK(HostDisplayPacing_FrameLimitIntervalNs(
            Options(kRefreshMode_Vsync, 60, 60, false)) == 0);
}

static void TestUiAndPausedIntervals(void) {
  const HostDisplayPacingOptions vsync_144 =
      Options(kRefreshMode_Vsync, 60, 144, false);
  CHECK(HostDisplayPacing_UiIntervalNs(
            vsync_144, kEmulationFrameIntervalNs) ==
        3472222ull);
  CHECK(HostDisplayPacing_PausedIntervalNs(
            vsync_144, kEmulationFrameIntervalNs) ==
        kEmulationFrameIntervalNs);

  const HostDisplayPacingOptions unlimited_30 =
      Options(kRefreshMode_Unlimited, 60, 30, false);
  CHECK(HostDisplayPacing_UiIntervalNs(
            unlimited_30, kEmulationFrameIntervalNs) ==
        33333333ull);
  CHECK(HostDisplayPacing_PausedIntervalNs(
            unlimited_30, kEmulationFrameIntervalNs) ==
        33333333ull);

  const HostDisplayPacingOptions gamescope =
      Options(kRefreshMode_Unlimited, 60, 90, true);
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
            Options(kRefreshMode_Unlimited, 60, 60, false),
            kEmulationFrameIntervalNs) ==
        8333333ull);
  CHECK(HostDisplayPacing_GameIntervalNs(
            Options(kRefreshMode_Vsync, 60, 60, false),
            kEmulationFrameIntervalNs) ==
        8333333ull);
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
}

int main(void) {
  TestExplicitFrameLimits();
  TestUnlimitedAndVsyncPolicies();
  TestUiAndPausedIntervals();
  TestGamePresentAntiSpinFloor();
  TestSub60LimitsRetainElapsedTime();
  if (s_failure_count) {
    fprintf(stderr, "host_display_pacing_test: %d failure(s)\n",
            s_failure_count);
    return 1;
  }
  puts("host_display_pacing_test: all tests passed");
  return 0;
}
