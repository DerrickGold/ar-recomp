#include <stdio.h>
#include <string.h>
#include <math.h>
#include "diorama_scroll_math.h"

static int failures;
#define CHECK(e) do { if(!(e)){ fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#e); failures++; } } while(0)
static int near(float a, float b){ return fabsf(a-b) < 1e-4f; }

int main(void) {
  FrameSlot curr; DioramaScrollSnapshot prev;
  memset(&curr, 0, sizeof(curr)); memset(&prev, 0, sizeof(prev));
  curr.snes_width = 256; curr.snes_height = 224;
  curr.diorama_active = prev.diorama_active = true;
  prev.timestamp_ns = 1000000000ULL;
  curr.timestamp_ns = 1000000000ULL + 16600000ULL;   /* +16.6ms */
  prev.bg1_camera_x = 0;  curr.bg1_camera_x = 10;     /* +10 px */

  /* t ~= 0.5: now halfway into the 16.6ms span past curr. */
  uint64_t now = curr.timestamp_ns + 8300000ULL;
  DioramaScrollDelta d = ComputeDioramaScrollDeltaAt(&curr, &prev, now);
  CHECK(d.active);
  CHECK(near(d.bg_du[0], (0.5f * 10.0f) / 256.0f));   /* ~0.01953 */
  CHECK(d.bg_du[0] > 0.0f);
  /* Refuted-claim guard: do NOT assert bg_du[1]==0 (BG2 has a WRAM camera).
   * BG3 (index 2) and BG4 (index 3) DO stay zero. */
  CHECK(d.bg_du[2] == 0.0f && d.bg_dv[2] == 0.0f);
  CHECK(d.bg_du[3] == 0.0f && d.bg_dv[3] == 0.0f);
  /* bg_du[1]==0 here only because bg2_camera_x is equal in both snapshots: */
  CHECK(d.bg_du[1] == 0.0f);

  /* Turbo suppresses interpolation. */
  curr.turbo_active = true;
  CHECK(!ComputeDioramaScrollDeltaAt(&curr, &prev, now).active);
  curr.turbo_active = false;

  /* span >= 50ms suppresses. */
  FrameSlot slow = curr; slow.timestamp_ns = prev.timestamp_ns + 60000000ULL;
  CHECK(!ComputeDioramaScrollDeltaAt(&slow, &prev, slow.timestamp_ns + 1).active);

  /* R16: prev==NULL must return inactive AND leave the internal tick-span
   * average untouched. main.c relies on exactly this to keep paused/menu
   * keep-alive presents out of the velocity estimate: they carry identical
   * camera data under a fresh capture timestamp, so if their (much shorter)
   * host-UI interval reached span_ema, `t` would saturate and the first
   * frames after unpausing would over-extrapolate a whole tick of motion.
   * Baseline the delta, flood the function with NULL-prev calls at panel
   * rate, then re-run the SAME pair: an identical result proves no leak. */
  DioramaScrollDelta before = ComputeDioramaScrollDeltaAt(&curr, &prev, now);
  CHECK(before.active);
  for (int i = 0; i < 60; i++) {
    /* A keep-alive present: fresh timestamp ~6.9ms apart (144Hz), same camera. */
    FrameSlot idle = curr;
    idle.timestamp_ns = curr.timestamp_ns + (uint64_t)(i + 1) * 6944444ULL;
    CHECK(!ComputeDioramaScrollDeltaAt(&idle, NULL, idle.timestamp_ns).active);
  }
  DioramaScrollDelta after = ComputeDioramaScrollDeltaAt(&curr, &prev, now);
  CHECK(after.active);
  CHECK(near(after.bg_du[0], before.bg_du[0]));
  CHECK(near(after.bg_dv[0], before.bg_dv[0]));

  if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
  puts("diorama_scroll_math_test: PASS");
  return 0;
}
