#include "diorama_snapshot.h"
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(kDioramaPlane_Count == 13 && SR_PPU_SURFACE_MAX_WIDTH == 640 &&
               SR_PPU_SURFACE_MAX_HEIGHT == 352,
               "update the compositor snapshot version when capture planes/extents change");
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "compositor snapshots require IEEE binary32 float");

#define SNAPSHOT_MAGIC UINT32_C(0x49445241) /* ARDI */
static uint32_t Read32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
      (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void Write32(uint8_t *p, uint32_t n) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (i * 8));
}
static uint32_t Hash(const uint8_t *p, size_t size) {
  uint32_t h = UINT32_C(2166136261);
  for (size_t i = 0; i < size; i++)
    h = (h ^ (i >= 12 && i < 16 ? 0 : p[i])) * UINT32_C(16777619);
  return h;
}

/* One checked field codec for both directions; bools and small integers are
 * validated BEFORE assignment, so narrowing cannot hide an invalid packet. */
static bool Fields(DioramaSnapshot *s, uint8_t *header, bool decode) {
  size_t at = 16;
#define FIELD(TYPE, LO, HI, VALUE, TO_BITS, FROM_BITS) do { \
    if (at > kDioramaSnapshotHeaderBytes - 4) return false; \
    TYPE value = (VALUE); \
    uint32_t bits; \
    if (decode) { bits = Read32(header + at); FROM_BITS; } \
    else { TO_BITS; } \
    if (!isfinite((double)value) || (double)value < (double)(LO) || \
        (double)value > (double)(HI)) return false; \
    if (decode) (VALUE) = value; \
    else Write32(header + at, bits); \
    at += 4; \
  } while (0);
#define U(LO, HI, VALUE) FIELD(uint32_t, LO, HI, VALUE, bits = value, value = bits)
#define I(LO, HI, VALUE) \
    FIELD(int32_t, LO, HI, VALUE, bits = (uint32_t)value, memcpy(&value, &bits, 4))
#define F(LO, HI, VALUE) \
    FIELD(float, LO, HI, VALUE, memcpy(&bits, &value, 4), memcpy(&value, &bits, 4))
#define B(VALUE) U(0, 1, VALUE)
#define Q(VALUE) do { \
    uint32_t lo = (uint32_t)(VALUE), hi = (uint32_t)((VALUE) >> 32); \
    U(0, UINT32_MAX, lo) U(0, 65535, hi) \
    if (decode) (VALUE) = (uint64_t)hi << 32 | lo; \
  } while (0);
#include "diorama_snapshot_fields.inc"
#undef Q
#undef B
#undef F
#undef I
#undef U
#undef FIELD
  if (at > kDioramaSnapshotHeaderBytes) return false;
  if (decode)
    for (; at < kDioramaSnapshotHeaderBytes; at++)
      if (header[at]) return false;
  if (s->capture.width + s->capture.obj_apron * 2 > 640 ||
      s->capture.authentic_y0 + 224 > s->capture.height ||
      (s->plane_mask & (1u << SR_PPU_OVERLAY_BG4)) ||
      s->view.viewport.x + s->view.viewport.w > 4096 ||
      s->view.viewport.y + s->view.viewport.h > 4096) return false;
  if (!!s->skybox_width != !!s->skybox_height) return false;
  if (s->skybox_width && (s->skybox.width <= 0 ||
      s->skybox.width > s->skybox_width)) return false;
  if (s->skybox_width && (s->skybox.periodic
      ? s->skybox_width != 256 || s->skybox_height != 256
      : s->skybox_width != 640 || s->skybox_height != 352)) return false;
  uint32_t seen = 0;
  for (int i = 0; i < s->layer_count; i++) {
    uint32_t bit = 1u << s->layers[i].plane;
    if ((seen & bit) || s->layers[i].plane == SR_PPU_OVERLAY_BG4 ||
        s->layers[i].stack_copies > (s->layers[i].stack_solid
            ? kDioramaVoxelMax : kDioramaStackMax)) return false;
    seen |= bit;
  }
  for (int i = 0; i < s->spans.count; i++)
    if (s->spans.spans[i].y1 < s->spans.spans[i].y0 ||
        s->spans.spans[i].x1 < s->spans.spans[i].x0 ||
        (i && s->spans.spans[i].y0 < s->spans.spans[i-1].y1)) return false;
  return true;
}

void DioramaSnapshot_Bind(DioramaSnapshot *s) {
  s->scene.render = &s->options;
  s->options.resolved_layers = s->layers;
  s->options.resolved_layer_count = s->layer_count;
  s->capture.textures = s->textures;
  s->capture.pixels = s->rgba;
  s->capture.plane_capture_offsets = s->offsets;
  s->capture.bg_transparent_fill_configured = s->fill_configured;
  s->capture.bg_transparent_fill_argb = s->fill_argb;
  s->capture.coverage_masks = s->has_coverage ? s->coverage : NULL;
  s->capture.bg2_valid_spans = s->has_spans ? &s->spans : NULL;
  s->capture.skybox = s->skybox_width ? &s->skybox : NULL;
  s->capture.bg2_revision = 1;
  s->skybox.revision = 1;
}

bool DioramaSnapshot_Describe(DioramaSnapshot *out,
    const DioramaCapture *c, const DioramaView *v, const DioramaScene *scene,
    uint32_t plane_mask, int skybox_width, int skybox_height) {
  if (!out || !c || !v || !scene || !scene->render || scene->backdrop || c->bg2_dynamic ||
      (c->skybox && c->skybox->dynamic)) return false;
  DioramaSnapshot s = {0};
  s.capture = *c;
  s.view = *v;
  s.scene = *scene;
  s.scene.plane_effect = NULL;
  s.scene.plane_effect_userdata = NULL;
  s.scene.effect_bg_plane_mask = 0;
  s.scene.effect_obj_priority_mask = 0;
  s.options = *scene->render;
  s.options.layers = NULL;
  s.options.resolve_skybox = NULL;
  s.options.skybox_userdata = NULL;
  s.options.waterfall_diagnostics = false;
  s.layer_count = Diorama_ResolveSceneLayers(scene, s.layers);
  s.plane_mask = plane_mask;
  s.skybox_width = skybox_width;
  s.skybox_height = skybox_height;
  if (c->skybox) s.skybox = *c->skybox;
  s.skybox.texture = ArRenderTexture_Invalid();
  s.has_coverage = c->coverage_masks != NULL;
  s.has_spans = c->bg2_valid_spans != NULL;
  if (s.has_spans) s.spans = *c->bg2_valid_spans;
  if (s.has_coverage) memcpy(s.coverage, c->coverage_masks, sizeof(s.coverage));
  if (c->plane_capture_offsets) memcpy(s.offsets, c->plane_capture_offsets, sizeof(s.offsets));
  if (c->bg_transparent_fill_configured)
    memcpy(s.fill_configured, c->bg_transparent_fill_configured, sizeof(s.fill_configured));
  if (c->bg_transparent_fill_argb)
    memcpy(s.fill_argb, c->bg_transparent_fill_argb, sizeof(s.fill_argb));
  uint8_t header[kDioramaSnapshotHeaderBytes] = {0};
  if (!Fields(&s, header, false)) return false;
  *out = s;
  DioramaSnapshot_Bind(out);
  return true;
}

static size_t ImageSize(const DioramaSnapshot *s, int image) {
  if (image == kDioramaPlane_Count)
    return (size_t)s->skybox_width * s->skybox_height * 4;
  return (s->plane_mask & (1u << image)) ? 640u * 352u * 4u : 0;
}

bool DioramaSnapshot_Encode(const DioramaSnapshot *snapshot,
    const DioramaSnapshotImage images[kDioramaSnapshotImageCount],
    uint8_t *packet, size_t capacity, size_t *size) {
  if (size) *size = 0;
  if (!snapshot || !images || !packet || !size) return false;
  DioramaSnapshot s = *snapshot;
  uint8_t header[kDioramaSnapshotHeaderBytes] = {0};
  if (!Fields(&s, header, false)) return false;
  size_t total = kDioramaSnapshotHeaderBytes;
  for (int i = 0; i < kDioramaSnapshotImageCount; i++) {
    size_t bytes = ImageSize(&s, i);
    if (bytes && (!images[i].argb || images[i].pitch <
        (size_t)(i == kDioramaPlane_Count ? s.skybox_width : 640) * 4 ||
        images[i].pitch > SIZE_MAX / 352)) return false;
    total += bytes;
  }
  if (total > capacity) return false;
  Write32(header, SNAPSHOT_MAGIC);
  Write32(header + 4, kDioramaSnapshotVersion);
  Write32(header + 8, (uint32_t)total);
  memcpy(packet, header, sizeof(header));
  size_t at = sizeof(header);
  for (int i = 0; i < kDioramaSnapshotImageCount; i++) {
    if (!ImageSize(&s, i)) continue;
    int w = i == kDioramaPlane_Count ? s.skybox_width : 640;
    int h = i == kDioramaPlane_Count ? s.skybox_height : 352;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      uint32_t p;
      memcpy(&p, images[i].argb + (size_t)y * images[i].pitch + x * 4, 4);
      packet[at++] = (uint8_t)(p >> 16);
      packet[at++] = (uint8_t)(p >> 8);
      packet[at++] = (uint8_t)p;
      packet[at++] = (uint8_t)(p >> 24);
    }
  }
  Write32(packet + 12, Hash(packet, total));
  *size = total;
  return true;
}

bool DioramaSnapshot_Decode(const uint8_t *packet, size_t size, DioramaSnapshot *out) {
  if (!packet || !out || size < kDioramaSnapshotHeaderBytes ||
      size > kDioramaSnapshotCapacity || Read32(packet) != SNAPSHOT_MAGIC ||
      Read32(packet+4) != kDioramaSnapshotVersion || Read32(packet+8) != size ||
      Read32(packet+12) != Hash(packet, size)) return false;
  uint8_t header[kDioramaSnapshotHeaderBytes];
  memcpy(header, packet, sizeof(header));
  DioramaSnapshot s = {0};
  if (!Fields(&s, header, true)) return false;
  size_t at = sizeof(header);
  for (int i = 0; i < kDioramaSnapshotImageCount; i++) {
    size_t bytes = ImageSize(&s, i);
    if (bytes > size - at) return false;
    if (bytes) s.rgba[i] = packet + at;
    at += bytes;
  }
  if (at != size) return false;
  *out = s;
  DioramaSnapshot_Bind(out);
  return true;
}

void DioramaSnapshot_ReleaseTextures(DioramaSnapshot *s, ArRenderDevice *device) {
  for (int i = 0; i < kDioramaPlane_Count; i++) {
    ArRenderDevice_DestroyTexture(device, s->textures[i]);
    s->textures[i] = ArRenderTexture_Invalid();
  }
  ArRenderDevice_DestroyTexture(device, s->skybox.texture);
  s->skybox.texture = ArRenderTexture_Invalid();
}

bool DioramaSnapshot_Upload(DioramaSnapshot *s, ArRenderDevice *device) {
  if (!s || !ArRenderDevice_IsReady(device) ||
      ArRenderTexture_IsValid(s->skybox.texture)) return false;
  for (int i = 0; i < kDioramaPlane_Count; i++)
    if (ArRenderTexture_IsValid(s->textures[i])) return false;
  uint32_t *pixels = malloc(640 * 352 * sizeof(uint32_t));
  if (!pixels) return false;
  bool ok = true;
  for (int i = 0; i < kDioramaSnapshotImageCount && ok; i++) {
    if (!s->rgba[i]) continue;
    int w = i == kDioramaPlane_Count ? s->skybox_width : 640;
    int h = i == kDioramaPlane_Count ? s->skybox_height : 352;
    const ArRenderTextureDesc desc = {w, h, kArRenderPixelFormat_Argb8888,
      kArRenderTextureUsage_Static, i == kDioramaPlane_Count
          ? kArRenderFilter_Linear : kArRenderFilter_Nearest,
      i == kDioramaPlane_Count || i == kDioramaPlane_Backdrop
          ? kArRenderBlendMode_Opaque : kArRenderBlendMode_Alpha};
    ArRenderTexture *t = i == kDioramaPlane_Count ? &s->skybox.texture : &s->textures[i];
    for (int j = 0; j < w * h; j++) {
      const uint8_t *p = s->rgba[i] + j * 4;
      pixels[j] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 |
          (uint32_t)p[1] << 8 | p[2];
    }
    ok = ArRenderDevice_CreateTexture(device, &desc, t) &&
        ArRenderDevice_UpdateTexture(device, *t, NULL, pixels, w * 4);
  }
  free(pixels);
  if (!ok) DioramaSnapshot_ReleaseTextures(s, device);
  return ok;
}
