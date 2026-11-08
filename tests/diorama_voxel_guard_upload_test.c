#include "diorama/diorama_voxel_guard_upload.h"
#include "action/action_voxel.h"
#include "action/action_room_scene.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Backend { unsigned creates, updates, destroys, fail_update; uint32_t *pixels[4]; } Backend;
static unsigned split_calls;
static uint64_t revision = 7;
static const uint32_t *expected_low;
uint64_t ActionVoxel_Revision(const ActionVoxelGeometry *g) { assert(g); return revision; }
/* An independent lane marker isolates upload/ownership from the separately
 * tested production split. It catches union, reversed-output and crop errors. */
bool ActionVoxel_Split(const ActionVoxelGeometry *g, int ox, int oy, int width, int height,
    size_t pitch, const ActionRoomSceneFrameState *frame, const uint32_t *low,
    size_t low_pitch, uint32_t *rear, uint32_t *front) {
  assert(g && ox == 0 && oy == 305 && width == 16 && height == 47);
  assert(low == expected_low && pitch == 640 && low_pitch == 640);
  ++split_calls;
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    rear[y * pitch + x] = low[y * low_pitch + x] ^ frame->brightness;
    front[y * pitch + x] = low[y * low_pitch + x] ^ 0x00010000u ^ frame->fixed_color;
  }
  return true;
}
static bool Create(void *context, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  Backend *b = context; assert(desc->width == 640 && desc->height == 80);
  assert(desc->filter == kArRenderFilter_Nearest);
  unsigned id = ++b->creates; assert(id < 4);
  b->pixels[id] = calloc(kDioramaVoxelGuardTexturePixels, 4); assert(b->pixels[id]);
  *out = (ArRenderTexture){id}; return true;
}
static void Destroy(void *context, ArRenderTexture texture) {
  Backend *b = context; assert(texture.value < 4 && b->pixels[texture.value]);
  free(b->pixels[texture.value]); b->pixels[texture.value] = NULL; ++b->destroys;
}
static bool Update(void *context, ArRenderTexture texture, const ArRenderRectI *r,
    const void *pixels, int pitch) {
  Backend *b = context; if (++b->updates == b->fail_update) return false;
  assert(r && r->x >= 0 && r->y >= 0 && r->x + r->w <= 640 && r->y + r->h <= 80);
  for (int y = 0; y < r->h; ++y) memcpy(b->pixels[texture.value] + (r->y + y) * 640 + r->x,
      (const uint8_t *)pixels + y * pitch, (size_t)r->w * 4);
  return true;
}
int main(void) {
  Backend backend = {0};
  const ArRenderBackendOps ops = {.struct_size = sizeof(ops), .create_texture = Create,
    .destroy_texture = Destroy, .update_texture = Update};
  ArRenderDevice device = {.ops = &ops, .context = &backend};
  const ActionVoxelGeometry *geometry = (const ActionVoxelGeometry *)&revision;
  static uint32_t raw[3][kDioramaVoxelGuardPixels], prefix_pixels[3][640 * 16];
  DioramaVoxelGuardSource source = {.pixels = {raw[0], raw[1], raw[2]},
    .pitch_pixels = 640, .pixel_count = kDioramaVoxelGuardPixels, .capture_generation = 11};
  assert(DioramaVoxelGuard_Resolve(-8, -47, 32, 352,
      (DioramaTrunkGuardBounds){0, 0, 16, 352}, &source.layout));
  assert(source.layout.lower.valid.x == 8 && source.layout.lower.valid.height == 47);
  expected_low = raw[0] + 8;
  DioramaVoxelGuardPrefix prefix = {.pixels = {prefix_pixels[0], prefix_pixels[1], prefix_pixels[2]},
    .pitch_pixels = {640, 640, 640}, .width = 32, .capture_generation = 11};
  for (unsigned b = 0; b < 3; ++b) {
    for (unsigned i = 0; i < kDioramaVoxelGuardPixels; ++i) raw[b][i] = 0xff123400u + (b << 16) + (i & 255);
    for (unsigned i = 0; i < 640 * 16; ++i) prefix_pixels[b][i] = 0xff543200u + (b << 16) + (i & 255);
  }
  DioramaVoxelGuardUpload cache = {0}; ActionRoomSceneFrameState frame = {.brightness = 15};
  assert(DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  assert(split_calls == 1 && backend.creates == 3 && backend.updates == 3);
  for (unsigned b = 0; b < 3; ++b) for (unsigned y = 0; y < 80; ++y) for (unsigned x = 0; x < 640; ++x) {
    uint32_t expected = 0;
    if (y < 16 && x < 32) expected = prefix_pixels[b][y * 640 + x];
    else if (y >= 16 && y < 63 && x >= 8 && x < 24) {
      unsigned at = (y - 16) * 640 + x;
      expected = b == 1 ? raw[1][at] : raw[0][at] ^ (b == 0 ? 15u : 0x10000u);
    }
    assert(backend.pixels[b + 1][y * 640 + x] == expected);
  }
  assert(DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  assert(split_calls == 1 && backend.updates == 3); /* Orbit-only repaint. */
  ++source.capture_generation; prefix.capture_generation = source.capture_generation;
  raw[2][8] ^= 0xffffu; /* Native Far is not derived Voxel foreground. */
  assert(DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  assert(split_calls == 2 && backend.updates == 3);
  ++revision; frame.brightness = 3; frame.fixed_color = 123;
  assert(DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  assert(cache.view.source_revision == revision && split_calls == 3);
  ++source.capture_generation; prefix.capture_generation = source.capture_generation;
  raw[0][8] ^= 0x55; raw[1][8] ^= 0x66; backend.fail_update = backend.updates + 2;
  assert(!DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  assert(!cache.view.capture_generation);
  backend.fail_update = 0;
  assert(DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  --prefix.capture_generation;
  assert(!DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  assert(!cache.view.capture_generation);
  assert(DioramaVoxelGuardUpload_Update(&cache, &device, NULL, NULL, NULL, NULL));
  prefix.capture_generation = source.capture_generation;
  source.layout.lower.valid.height++;
  assert(!DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  assert(!cache.view.capture_generation);
  source.layout.lower.valid.height--;
  prefix.pitch_pixels[1] = 31;
  assert(!DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  prefix.pitch_pixels[1] = 640;
  source.pixels[2] = NULL;
  assert(!DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  source.pixels[2] = raw[2];
  assert(DioramaVoxelGuardUpload_Update(&cache, &device, geometry, &source, &frame, &prefix));
  assert(cache.view.capture_generation == source.capture_generation);
  DioramaVoxelGuardUpload_Reset(&cache, &device); assert(backend.destroys == 3);
  puts("voxel guard: exact bands, finite crop, primary overlap, cache, invalidation and atomic publication pass");
  return 0;
}
