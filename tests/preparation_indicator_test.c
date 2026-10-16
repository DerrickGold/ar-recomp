#include "present/preparation_indicator.h"
#include "host/host_clock.h"
#include "support/test_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ROM-free: layout rules, draw gating and the plain-bar fallback. Decoding
 * real art needs the player's ROM, so only its rejection paths run here. */
static uint64_t clock_ms;
static int captures, ends, clears, presents, solid_rects, textures_drawn;
static bool target_bound;

uint64_t HostClock_Milliseconds(void) { return clock_ms; }
bool ArRenderDevice_CaptureTargetState(ArRenderDevice *device, ArRenderTargetState *state) {
  (void)device;
  ++captures;
  *state = (ArRenderTargetState){.target = {target_bound ? 7u : 0u}, .valid = true};
  return true;
}
bool ArRenderDevice_EndTarget(ArRenderDevice *device, const ArRenderTargetState *state) {
  (void)device;
  assert(state->valid);
  ++ends;
  return true;
}
bool ArRenderDevice_UseOutputCoordinates(ArRenderDevice *device) { return device != NULL; }
bool ArRenderDevice_GetOutputSize(ArRenderDevice *device, int *width, int *height) {
  (void)device;
  *width = 1920;
  *height = 1080;
  return true;
}
bool ArRenderDevice_Clear(ArRenderDevice *device, ArRenderColorF color) {
  (void)device;
  (void)color;
  ++clears;
  return true;
}
bool ArRenderDevice_DrawSolidRect(ArRenderDevice *device, const ArRenderRectF *rectangle,
                                  ArRenderColorF color, ArRenderBlendMode blend) {
  (void)device;
  (void)color;
  assert(rectangle->w > 0 && rectangle->h > 0 && blend == kArRenderBlendMode_Alpha);
  ++solid_rects;
  return true;
}
bool ArRenderDevice_DrawTexture(ArRenderDevice *device, ArRenderTexture texture,
                                const ArRenderRectF *source, const ArRenderRectF *destination) {
  (void)device;
  (void)texture;
  (void)source;
  (void)destination;
  ++textures_drawn;
  return true;
}
bool ArRenderDevice_CreateTexture(ArRenderDevice *device, const ArRenderTextureDesc *desc,
                                  ArRenderTexture *out) {
  (void)device;
  (void)desc;
  *out = (ArRenderTexture){1};
  return true;
}
bool ArRenderDevice_UpdateTexture(ArRenderDevice *device, ArRenderTexture texture,
                                  const ArRenderRectI *rect, const void *pixels, int pitch) {
  (void)device;
  (void)texture;
  (void)rect;
  (void)pixels;
  (void)pitch;
  return true;
}
void ArRenderDevice_DestroyTexture(ArRenderDevice *device, ArRenderTexture texture) {
  (void)device;
  (void)texture;
}
bool ArRenderDevice_Present(ArRenderDevice *device) {
  (void)device;
  ++presents;
  return true;
}

static int LostPills(const PreparationSprite *sprites) {
  int lost = 0;
  for (int i = 0; i < kPreparationBarPills; i++) lost += sprites[i].source.x > 0;
  return lost;
}

int main(void) {
  /* Layout: sixteen contiguous pills, a four-pill red window at each end of
   * the sweep, the angel fixed over the bar's centre, one cycle per 2 sweeps. */
  PreparationSprite sprites[kPreparationSpriteCapacity];
  assert(PreparationIndicator_Layout(0, 1080, 0, sprites) == 0);
  assert(PreparationIndicator_Layout(1920, 1080, 0, sprites) == kPreparationSpriteCapacity);
  const float pill = sprites[0].destination.w;
  assert(pill == kPreparationPillWidth * (1080 / 150));
  for (int i = 1; i < kPreparationBarPills; i++)
    assert(sprites[i].destination.x == sprites[0].destination.x + i * pill);
  assert(LostPills(sprites) == kPreparationLostPills && sprites[0].source.x > 0);
  const PreparationSprite angel = sprites[kPreparationBarPills];
  const float bar_centre = sprites[0].destination.x + kPreparationBarPills * pill / 2;
  assert(angel.destination.x + angel.destination.w / 3 == bar_centre);
  assert(angel.destination.y + angel.destination.h < sprites[0].destination.y);
  assert(PreparationIndicator_Layout(1920, 1080, kPreparationIndicatorSweepMs, sprites) ==
         kPreparationSpriteCapacity);
  assert(LostPills(sprites) == kPreparationLostPills &&
         sprites[kPreparationBarPills - 1].source.x > 0);
  assert(sprites[kPreparationBarPills].destination.x == angel.destination.x &&
         sprites[kPreparationBarPills].destination.y == angel.destination.y);
  PreparationSprite looped[kPreparationSpriteCapacity];
  PreparationIndicator_Layout(1920, 1080, 2 * kPreparationIndicatorSweepMs, looped);
  PreparationIndicator_Layout(1920, 1080, 0, sprites);
  assert(!memcmp(looped, sprites, sizeof(sprites)));

  /* Decode rejects what it cannot validate and keeps the plain bar. */
  assert(!PreparationIndicator_DecodeRom(NULL, 0));
  uint8_t *blank = calloc(1, 0x100000);
  assert(blank && !PreparationIndicator_DecodeRom(blank, 0x100000));
  assert(!PreparationIndicator_HasArt());
  free(blank);

  /* Gating: hidden before the delay, throttled after it, never over a bound
   * offscreen target, and the captured target state is always restored. */
  ArRenderDevice device = {0};
  unsetenv("AR_PREPARATION_INDICATOR");
  clock_ms = 1000;
  PreparationIndicator_Begin(&device);
  PreparationIndicator_Tick(&device);
  assert(presents == 0 && captures == 0);
  clock_ms += kPreparationIndicatorDelayMs;
  PreparationIndicator_Tick(&device);
  assert(presents == 1 && captures == 1 && ends == 1 && clears == 1);
  assert(solid_rects == 2 && textures_drawn == 0); /* plain bar without art */
  PreparationIndicator_Tick(&device);
  assert(presents == 1); /* throttled */
  clock_ms += kPreparationIndicatorIntervalMs;
  target_bound = true;
  PreparationIndicator_Tick(&device);
  assert(presents == 1 && captures == 2 && ends == 2);
  target_bound = false;
  PreparationIndicator_End(&device);
  assert(presents == 1);

  setenv("AR_PREPARATION_INDICATOR", "off", 1);
  PreparationIndicator_Begin(&device);
  clock_ms += 10 * kPreparationIndicatorDelayMs;
  PreparationIndicator_Tick(&device);
  assert(presents == 1);
  setenv("AR_PREPARATION_INDICATOR", "force", 1);
  PreparationIndicator_Begin(&device);
  PreparationIndicator_Tick(&device);
  assert(presents == 2); /* no delay when forced */
  unsetenv("AR_PREPARATION_INDICATOR");
  puts("preparation indicator: PASS");
}
