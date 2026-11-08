#include "diorama/diorama_trunk_guard_upload.h"
#include "action/action_trunk.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Backend { unsigned creates, updates, destroys, fail_update; uint32_t *pixels[4]; } Backend;
static bool Create(void *context, const ArRenderTextureDesc *desc, ArRenderTexture *out) {
  Backend *b = context;
  assert(desc->width == 640 && desc->height == 128 && desc->filter == kArRenderFilter_Nearest);
  unsigned id = ++b->creates; assert(id < 4);
  b->pixels[id] = calloc(kDioramaTrunkGuardPixels, 4); assert(b->pixels[id]);
  *out = (ArRenderTexture){id}; return true;
}
static void Destroy(void *context, ArRenderTexture texture) {
  Backend *b = context; assert(texture.value < 4 && b->pixels[texture.value]);
  free(b->pixels[texture.value]); b->pixels[texture.value] = NULL; ++b->destroys;
}
static bool Update(void *context, ArRenderTexture texture, const ArRenderRectI *r,
    const void *pixels, int pitch) {
  Backend *b = context; ++b->updates;
  if (b->fail_update == b->updates) return false;
  assert(r && r->x >= 0 && r->y >= 0 && r->x + r->w <= 640 && r->y + r->h <= 128);
  for (int y = 0; y < r->h; ++y) memcpy(b->pixels[texture.value] + (r->y + y) * 640 + r->x,
      (const uint8_t *)pixels + y * pitch, (size_t)r->w * 4);
  return true;
}
/* Native split semantics are tested independently against all six regional
 * scenes. This ownership test makes each output identifiable without ROM or
 * linking the compiler; the production guard helper still handles clipping. */
void ActionTrunk_Split(const ActionTrunkAssets *assets, int ox, int oy, int width,
    int height, int pitch, const uint32_t *const input[3], const size_t input_pitch[3],
    uint32_t *const output[3], unsigned brightness) {
  (void)assets; (void)ox; (void)oy;
  for (unsigned b = 0; b < 3; ++b) for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      output[b][y * pitch + x] = input[0][y * input_pitch[0] + x] ^ (b << 16) ^ brightness;
}
int main(void) {
  Backend backend = {0};
  const ArRenderBackendOps ops = {.struct_size = sizeof(ops), .create_texture = Create,
      .destroy_texture = Destroy, .update_texture = Update};
  ArRenderDevice device = {.ops = &ops, .context = &backend};
  static ActionTrunkAssets assets; assets.height = 512; assets.revision = 7;
  uint32_t *raw = malloc(kDioramaTrunkGuardPixels * 4); assert(raw);
  for (unsigned i = 0; i < kDioramaTrunkGuardPixels; ++i) raw[i] = 0xff123456u;
  DioramaTrunkGuardSource source = {.pixels = raw, .pitch_pixels = 640,
      .pixel_count = kDioramaTrunkGuardPixels, .capture_generation = 11};
  assert(DioramaTrunkGuard_Resolve(0, 64, 640, 224,
      (DioramaTrunkGuardBounds){0, 0, 640, 512}, &source.layout));
  DioramaTrunkGuardUpload cache = {0};
  assert(DioramaTrunkGuardUpload_Update(&cache, &device, &assets, &source, 15));
  assert(backend.creates == 3 && backend.updates == 3 && cache.view.capture_generation == 11);
  assert(cache.view.source_revision == 7 && backend.pixels[1][0] == (0xff123456u ^ 15));
  /* Re-presented generations do no split/upload; identical next captures also
   * avoid uploads through exact mirrors while updating source publication. */
  assert(DioramaTrunkGuardUpload_Update(&cache, &device, &assets, &source, 15));
  ++source.capture_generation;
  assert(DioramaTrunkGuardUpload_Update(&cache, &device, &assets, &source, 15));
  assert(backend.updates == 3 && cache.view.capture_generation == 12);
  raw[0] = 0xffabcdefu; ++source.capture_generation;
  backend.fail_update = backend.updates + 2;
  assert(!DioramaTrunkGuardUpload_Update(&cache, &device, &assets, &source, 15));
  assert(!cache.view.capture_generation); /* Never expose a mixed-generation trio. */
  backend.fail_update = 0;
  assert(DioramaTrunkGuardUpload_Update(&cache, &device, &assets, &source, 15));
  for (unsigned b = 0; b < 3; ++b)
    assert(backend.pixels[b + 1][0] == (raw[0] ^ (b << 16) ^ 15));
  ++assets.revision;
  assert(DioramaTrunkGuardUpload_Update(&cache, &device, &assets, &source, 3));
  assert(cache.view.source_revision == assets.revision && backend.pixels[1][0] == (raw[0] ^ 3));
  const unsigned created = backend.creates;
  assert(DioramaTrunkGuardUpload_Update(&cache, &device, NULL, NULL, 0));
  assert(!cache.view.capture_generation && backend.creates == created);
  ++source.capture_generation; source.layout.strips[0].valid.height--;
  assert(!DioramaTrunkGuardUpload_Update(&cache, &device, &assets, &source, 15));
  assert(!cache.view.capture_generation);
  DioramaTrunkGuardUpload_Reset(&cache, &device);
  assert(backend.destroys == 3 && !cache.pixels[0] && !cache.view.textures[0].value);
  free(raw); puts("native tree guard upload ownership: pass"); return 0;
}
