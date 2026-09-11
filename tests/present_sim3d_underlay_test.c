#include "present_sim3d_underlay.h"
#include "sim/sim_world_map.h"
#include "sim/sim3d_performance.h"

#include <assert.h>
#include <stdio.h>

static unsigned creates[2], uploads[2], downsamples, bakes, live;
static bool fail_create[2], fail_upload[2], fail_bake, fail_downsample;
static uint32_t marker = 1;

void Sim3DPerformance_AddUpload(uint64_t bytes) { assert(bytes); }
const char *ArRenderDevice_LastError(const ArRenderDevice *device) {
  (void)device; return "injected failure";
}
bool ArRenderDevice_CreateTexture(ArRenderDevice *device, const ArRenderTextureDesc *desc,
    ArRenderTexture *out) {
  (void)device;
  bool blur = desc->width != kSimWorldMapPixels;
  assert(desc->width == desc->height && desc->width == (blur ? 256 : 1024));
  assert(desc->filter == (blur ? kArRenderFilter_Linear : kArRenderFilter_Nearest));
  creates[blur]++;
  *out = (ArRenderTexture){fail_create[blur] ? 0 : 1+blur};
  if (out->value) live++;
  return out->value != 0;
}
void ArRenderDevice_DestroyTexture(ArRenderDevice *device, ArRenderTexture texture) {
  (void)device;
  if (texture.value) { assert(live); live--; }
}
bool ArRenderDevice_UpdateTexture(ArRenderDevice *device, ArRenderTexture texture,
    const ArRenderRectI *rect, const void *pixels, int pitch) {
  (void)device; assert(!rect && pixels);
  const bool blur = texture.value == 2;
  assert(texture.value == 1 || blur);
  assert(pitch == (blur ? 256 : 1024) * (int)sizeof(uint32_t));
  assert(*(const uint32_t *)pixels == marker);
  uploads[blur]++;
  return !fail_upload[blur];
}
const uint32_t *SimWorldMap_BakedPixels(void) { bakes++; return fail_bake ? NULL : &marker; }
bool SimWorldMap_Downsample(uint32_t *pixels, int pitch, int divisor) {
  assert(pitch == 256 && divisor == 4);
  downsamples++;
  pixels[0] = marker;
  return !fail_downsample;
}
static void Check(ArRenderDevice *device, uint32_t serial, bool blur, bool sharp_ok, bool blur_ok) {
  SimUnderlayTextures result = PresentSim3DUnderlay_Prepare(device, serial, blur);
  assert(ArRenderTexture_IsValid(result.sharp) == sharp_ok);
  assert(ArRenderTexture_IsValid(result.blurred) == blur_ok);
}
static void Reset(ArRenderDevice *device) {
  PresentSim3DUnderlay_ResetResources(device);
  assert(!live);
  creates[0] = creates[1] = uploads[0] = uploads[1] = downsamples = bakes = 0;
  fail_create[0] = fail_create[1] = fail_upload[0] = fail_upload[1] = false;
  fail_bake = fail_downsample = false;
}
int main(void) {
  ArRenderDevice device = {0};
  Check(&device, 0, true, false, false);
  assert(!creates[0] && !creates[1]);
  Check(&device, 1, false, true, false);
  Check(&device, 1, false, true, false);
  assert(creates[0] == 1 && uploads[0] == 1 && bakes == 1);
  assert(!creates[1] && !downsamples && !uploads[1]);
  marker = 2;
  Check(&device, 2, false, true, false);
  assert(uploads[0] == 2 && !downsamples);
  Check(&device, 2, true, true, true); /* Enable after sharp is already current. */
  Check(&device, 2, true, true, true);
  assert(uploads[0] == 2 && creates[1] == 1 && uploads[1] == 1 && downsamples == 1);
  marker = 3;
  Check(&device, 3, false, true, false); /* Retain, but never expose a stale blur. */
  assert(uploads[1] == 1 && downsamples == 1);
  Check(&device, 3, true, true, true);
  assert(uploads[0] == 3 && uploads[1] == 2 && downsamples == 2);
  marker = 4;
  Check(&device, 4, true, true, true);
  assert(uploads[0] == 4 && uploads[1] == 3);

  fail_upload[0] = true;
  marker = 5;
  Check(&device, 5, true, false, false);
  assert(uploads[0] == 5 && uploads[1] == 3);
  marker = 4;
  Check(&device, 4, true, false, false); /* Partial failure invalidates old serial too. */
  assert(uploads[0] == 6);
  fail_upload[0] = false;
  Check(&device, 4, true, true, true);
  assert(uploads[0] == 7 && uploads[1] == 3);

  fail_upload[1] = true;
  marker = 6;
  Check(&device, 6, true, true, false);
  Check(&device, 6, true, true, false);
  assert(uploads[0] == 8 && uploads[1] == 4 && live == 1);
  Reset(&device);
  Check(&device, 6, true, true, true); /* Reset permits recovery. */
  assert(live == 2);
  Reset(&device);

  fail_create[0] = true;
  Check(&device, 1, true, false, false);
  Check(&device, 2, true, false, false);
  assert(creates[0] == 1 && !bakes && !creates[1]);
  Reset(&device);
  fail_create[1] = true;
  Check(&device, 1, true, true, false);
  Check(&device, 2, true, true, false);
  assert(creates[1] == 1 && !downsamples && uploads[0] == 2);
  Reset(&device);
  fail_bake = true;
  Check(&device, 1, true, false, false);
  assert(!uploads[0] && !creates[1]);
  fail_bake = false;
  fail_downsample = true;
  Check(&device, 1, true, true, false);
  Check(&device, 1, true, true, false);
  assert(downsamples == 1 && !uploads[1] && live == 1);
  Reset(&device);
  puts("present_sim3d_underlay_test: PASS");
  return 0;
}
