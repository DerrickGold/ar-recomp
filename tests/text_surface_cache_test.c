#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localization/text_rasterizer.h"
#include "localization/text_boundaries.h"
#include "render/render_device.h"
#include "render/text_surface_cache.h"
#include "host/font_resources.h"
#include "render/ui_text_renderer.h"

static int g_failures;

#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #condition); \
    ++g_failures; \
  } \
} while (0)

typedef struct FakeRenderBackend {
  uintptr_t next_texture;
  int creates;
  int destroys;
  int uploads;
  bool fail_create;
  bool fail_upload;
  ArRenderTextureDesc last_descriptor;
  uint32_t uploaded_pixels[64 * 32];
  int uploaded_pitch;
  int draws;
  ArRenderRectF last_destination;
} FakeRenderBackend;

static bool CreateTexture(void *context, const ArRenderTextureDesc *descriptor,
                          ArRenderTexture *texture) {
  FakeRenderBackend *backend = (FakeRenderBackend *)context;
  ++backend->creates;
  backend->last_descriptor = *descriptor;
  if (backend->fail_create) return false;
  *texture = (ArRenderTexture){++backend->next_texture};
  return true;
}

static void DestroyTexture(void *context, ArRenderTexture texture) {
  FakeRenderBackend *backend = (FakeRenderBackend *)context;
  if (ArRenderTexture_IsValid(texture)) ++backend->destroys;
}

static bool UpdateTexture(void *context, ArRenderTexture texture,
                          const ArRenderRectI *destination,
                          const void *pixels, int pitch_bytes) {
  FakeRenderBackend *backend = (FakeRenderBackend *)context;
  (void)texture;
  (void)destination;
  if (!pixels || pitch_bytes <= 0) return false;
  ++backend->uploads;
  backend->uploaded_pitch = pitch_bytes;
  if (backend->last_descriptor.format == kArRenderPixelFormat_Rgba8888 &&
      backend->last_descriptor.width <= 64 &&
      backend->last_descriptor.height <= 32) {
    for (int y = 0; y < backend->last_descriptor.height; ++y)
      memcpy(&backend->uploaded_pixels[y * 64],
             (const uint8_t *)pixels + (size_t)y * (size_t)pitch_bytes,
             (size_t)backend->last_descriptor.width * sizeof(uint32_t));
  }
  return !backend->fail_upload;
}

static bool AlwaysRenderTarget(void *context, ArRenderTexture target) {
  (void)context;
  (void)target;
  return true;
}

static bool AlwaysOutputCoordinates(void *context) {
  (void)context;
  return true;
}

static bool OutputSize(void *context, int *width, int *height) {
  (void)context;
  *width = 1280;
  *height = 720;
  return true;
}

static bool AlwaysRect(void *context, const ArRenderRectI *rectangle) {
  (void)context;
  (void)rectangle;
  return true;
}

static bool AlwaysClear(void *context, ArRenderColorF color) {
  (void)context;
  (void)color;
  return true;
}

static bool AlwaysDrawTexture(void *context, ArRenderTexture texture,
                              const ArRenderRectF *source,
                              const ArRenderRectF *destination,
                              const ArRenderDrawState *state) {
  FakeRenderBackend *backend = context;
  ++backend->draws;
  backend->last_destination = *destination;
  (void)texture;
  (void)source;
  (void)destination;
  (void)state;
  return true;
}

static bool AlwaysDrawGeometry(void *context, ArRenderTexture texture,
                               const ArRenderVertex2D *vertices,
                               int vertex_count, const int32_t *indices,
                               int index_count,
                               const ArRenderDrawState *state) {
  (void)context;
  (void)texture;
  (void)vertices;
  (void)vertex_count;
  (void)indices;
  (void)index_count;
  (void)state;
  return true;
}

static bool AlwaysPresent(void *context) {
  (void)context;
  return true;
}

static const char *LastError(void *context) {
  (void)context;
  return "fake render error";
}

static const ArRenderBackendOps kRenderOps = {
  .struct_size = sizeof(ArRenderBackendOps),
  .create_texture = CreateTexture,
  .destroy_texture = DestroyTexture,
  .update_texture = UpdateTexture,
  .set_render_target = AlwaysRenderTarget,
  .use_output_coordinates = AlwaysOutputCoordinates,
  .get_output_size = OutputSize,
  .set_viewport = AlwaysRect,
  .set_clip_rect = AlwaysRect,
  .clear = AlwaysClear,
  .draw_texture = AlwaysDrawTexture,
  .draw_geometry = AlwaysDrawGeometry,
  .present = AlwaysPresent,
  .last_error = LastError,
};

typedef struct FakeRasterizer {
  uint32_t pixels[64 * 16];
  uint32_t owners[64 * 16];
  ArTextRevealCluster owned_clusters[2];
  bool with_owners;
  bool invalid_owner;
  ArTextRevealCluster cluster;
  int calls;
  int releases;
  bool fail;
  ArTextRasterFailure fail_kind;
  bool invalid_bitmap;
  bool invalid_cluster_bounds;
  bool invalid_cluster_order;
  bool invalid_cluster_utf8_boundary;
  ArTextRasterRequest last_request;
  int line_advance;
  ArRenderPixelFormat format;
  int pitch_bytes;
} FakeRasterizer;

static bool Rasterize(void *context, const ArTextRasterRequest *request,
                      ArTextBitmap *bitmap, ArTextRasterFailure *failure,
                      char *error, size_t error_capacity) {
  FakeRasterizer *fake = (FakeRasterizer *)context;
  ++fake->calls;
  fake->last_request = *request;
  if (fake->fail) {
    *failure = fake->fail_kind ? fake->fail_kind
                               : kArTextRasterFailure_Deterministic;
    if (error && error_capacity)
      snprintf(error, error_capacity, "fake raster failure");
    return false;
  }
  const int width = fake->invalid_bitmap
      ? request->maximum_width + 1 : (int)request->utf8_bytes * 2;
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 64; ++x)
      fake->pixels[y * 64 + x] =
          UINT32_C(0xff000000) | (uint32_t)(y << 8) | (uint32_t)x;
  fake->cluster = (ArTextRevealCluster){
    .end_utf8_byte = request->utf8_bytes,
    .line_index = 0,
    .x = fake->invalid_cluster_bounds ? INT32_MAX : 0,
    .width = width,
    .height = 16,
  };
  *bitmap = (ArTextBitmap){
    .struct_size = sizeof(*bitmap),
    .abi_version = AR_TEXT_BITMAP_ABI_VERSION,
    .pixels = fake->pixels,
    .width = width,
    .height = 16,
    .pitch_bytes = fake->pitch_bytes ? fake->pitch_bytes : 64 * (int)sizeof(uint32_t),
    .format = fake->pitch_bytes ? fake->format : kArRenderPixelFormat_Rgba8888,
    .ascent = 11,
    .descent = 3,
    .line_advance = fake->line_advance ? fake->line_advance : 16,
    .reveal_clusters = &fake->cluster,
    .reveal_cluster_count = 1,
    .token = 1,
  };
  if (fake->with_owners) {
    fake->owned_clusters[0] = fake->cluster;
    fake->owned_clusters[0].width = width / 2;
    fake->owned_clusters[0].end_utf8_byte = request->utf8_bytes / 2;
    fake->owned_clusters[1] = fake->cluster;
    fake->owned_clusters[1].x = width / 2;
    fake->owned_clusters[1].width = width - width / 2;
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < width; ++x) {
        /* Disjoint irregular ink, with owner 1 spilling over its typographic
         * box as a shadow/overhang would. */
        const bool ink = (x + y) % 3 != 0;
        fake->pixels[y * 64 + x] = ink ? 0xffffff80 : 0;
        fake->owners[y * width + x] = !ink ? 0 : x < width / 2 + y % 2 ? 1 : 2;
      }
    bitmap->reveal_clusters = fake->owned_clusters;
    bitmap->reveal_cluster_count = 2;
    bitmap->pixel_owners = fake->owners;
    if (fake->invalid_owner) fake->owners[0] = 3;
  } else if (fake->invalid_cluster_order) {
    fake->owned_clusters[0] = fake->cluster;
    fake->owned_clusters[0].end_utf8_byte = request->utf8_bytes;
    fake->owned_clusters[0].width = width / 2;
    fake->owned_clusters[1] = fake->cluster;
    fake->owned_clusters[1].end_utf8_byte = request->utf8_bytes - 1u;
    fake->owned_clusters[1].x = width / 2;
    fake->owned_clusters[1].width = width - width / 2;
    bitmap->reveal_clusters = fake->owned_clusters;
    bitmap->reveal_cluster_count = 2;
  } else if (fake->invalid_cluster_utf8_boundary) {
    fake->cluster.end_utf8_byte = 1;
  }
  return true;
}

static void ReleaseBitmap(void *context, ArTextBitmap *bitmap) {
  FakeRasterizer *fake = (FakeRasterizer *)context;
  ++fake->releases;
  if (bitmap) memset(bitmap, 0, sizeof(*bitmap));
}

static const ArTextRasterizerOps kRasterOps = {
  .struct_size = sizeof(ArTextRasterizerOps),
  .abi_version = AR_TEXT_RASTERIZER_ABI_VERSION,
  .rasterize = Rasterize,
  .release_bitmap = ReleaseBitmap,
};

static ArTextRasterRequest Request(const char *text) {
  return (ArTextRasterRequest){
    .struct_size = sizeof(ArTextRasterRequest),
    .abi_version = AR_TEXT_RASTER_REQUEST_ABI_VERSION,
    .utf8 = text,
    .utf8_bytes = strlen(text),
    .font_stack_id = "actraiser-default",
    .font_stack_id_bytes = strlen("actraiser-default"),
    .source_revision = 7,
    .font_revision = 11,
    .style_id = 2,
    .flags = kArTextRasterFlag_WrapWords |
             kArTextRasterFlag_PreserveHardBreaks |
             kArTextRasterFlag_IncludeRevealClusters,
    .direction = kArTextDirection_LeftToRight,
    .alignment = kArTextHorizontalAlignment_Leading,
    .font_pixels = 24,
    .minimum_font_pixels = 24,
    .maximum_width = 256,
    .maximum_height = 64,
    .filter = kArRenderFilter_Linear,
    .language_bcp47 = "fr",
    .language_bcp47_bytes = 2,
  };
}

static void TestAbiValidation(void) {
  ArTextRasterizer rasterizer = {0};
  FakeRasterizer fake = {0};
  ArTextRasterizerOps bad = kRasterOps;
  bad.abi_version++;
  CHECK(!ArTextRasterizer_Init(&rasterizer, &bad, &fake, 1));
  CHECK(!ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 0));
  CHECK(ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 3));

  ArTextRasterRequest request = Request("Texte");
  CHECK(ArTextRasterRequest_IsValid(&request));
  uint8_t preferred[AR_TEXT_BOUNDARY_BYTES(16)] = {0};
  request = Request("A B");
  const ArTextCacheKey ordinary =
      ArTextSurfaceCache_MakeKey(&rasterizer, &request);
  request.preferred_line_breaks = preferred;
  request.preferred_line_break_capacity = 16;
  request.preferred_line_break_source_offset = 3;
  CHECK(ArTextRasterRequest_IsValid(&request));
  CHECK(ArTextCacheKey_Equals(
      ordinary, ArTextSurfaceCache_MakeKey(&rasterizer, &request)));
  ArTextBoundary_Set(preferred, 4, true);
  CHECK(!ArTextCacheKey_Equals(
      ordinary, ArTextSurfaceCache_MakeKey(&rasterizer, &request)));
  request.flags = 0;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request.flags = kArTextRasterFlag_WrapWords |
                  kArTextRasterFlag_PreserveHardBreaks;
  request.preferred_line_break_capacity = 5;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request = Request("A B");
  request.preferred_line_break_source_offset = 3;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request = Request("Texte");
  ArTextBidiSpan span = {0,5,kArTextDirection_Auto};
  request.bidi_spans = &span; request.bidi_span_count = 1;
  CHECK(ArTextRasterRequest_IsValid(&request));
  const ArTextCacheKey isolated = ArTextSurfaceCache_MakeKey(&rasterizer,&request);
  span.direction = kArTextDirection_LeftToRight;
  CHECK(!ArTextCacheKey_Equals(isolated,ArTextSurfaceCache_MakeKey(&rasterizer,&request)));
  request.bidi_span_count = kArTextMaximumBidiSpans + 1;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request.bidi_span_count = 1; request.bidi_spans = NULL;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request.bidi_spans = &span; span.end = 0;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request = Request("Élise"); span.start = 1; span.end = 3;
  request.bidi_spans = &span; request.bidi_span_count = 1;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request = Request("Texte");
  request.utf8_bytes++;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request = Request("Texte");
  request.struct_size = 1;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  static const char kInvalidUtf8[] = {(char)0xc0, (char)0xaf};
  request = Request("Texte");
  request.utf8 = kInvalidUtf8;
  request.utf8_bytes = sizeof(kInvalidUtf8);
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request = Request("Texte");
  request.language_bcp47 = "fr_CA";
  request.language_bcp47_bytes = 5;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request = Request("Texte");
  request.pixelation = kArTextPixelation_LowResolution;
  request.pixelation_size = 1;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request.pixelation_size = 2;
  CHECK(ArTextRasterRequest_IsValid(&request));
  request.pixelation = (ArTextPixelation)99;
  CHECK(!ArTextRasterRequest_IsValid(&request));

  request = Request("Texte");
  request.minimum_font_pixels = 25;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request.minimum_font_pixels = 0;
  CHECK(!ArTextRasterRequest_IsValid(&request));

  request = Request("Texte");
  fake.invalid_bitmap = true;
  ArTextBitmap bitmap;
  char error[256];
  CHECK(!ArTextRasterizer_Rasterize(
      &rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(fake.calls == 1 && fake.releases == 1);
  CHECK(strstr(error, "invalid bitmap") != NULL);
  fake.invalid_bitmap = false;
  fake.invalid_cluster_bounds = true;
  CHECK(!ArTextRasterizer_Rasterize(
      &rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(fake.calls == 2 && fake.releases == 2);
  CHECK(strstr(error, "invalid bitmap") != NULL);
  fake.invalid_cluster_bounds = false;
  fake.invalid_cluster_order = true;
  request = Request("ab");
  CHECK(!ArTextRasterizer_Rasterize(
      &rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(fake.calls == 3 && fake.releases == 3);
  CHECK(strstr(error, "invalid bitmap") != NULL);
  fake.invalid_cluster_order = false;
  fake.invalid_cluster_utf8_boundary = true;
  request = Request("\xC3\xA9x");
  CHECK(!ArTextRasterizer_Rasterize(
      &rasterizer, &request, &bitmap, NULL, error, sizeof(error)));
  CHECK(fake.calls == 4 && fake.releases == 4);
  CHECK(strstr(error, "invalid bitmap") != NULL);
  ArTextRasterizer_Reset(&rasterizer);
}

static void TestCacheHitsMissesAndFailureAtomicity(void) {
  FakeRenderBackend render = {0};
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &kRenderOps, &render,
      (ArRenderCapabilities){
        .maximum_texture_width = 512,
        .maximum_texture_height = 512,
      }));
  FakeRasterizer fake = {0};
  ArTextRasterizer rasterizer;
  CHECK(ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 19));
  ArTextSurfaceCache cache;
  CHECK(ArTextSurfaceCache_Init(&cache, 2));

  ArTextRasterRequest first_request = Request("Menu");
  ArTextSurface first;
  char error[256];
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &first_request,
      &first, error, sizeof(error)));
  CHECK(ArRenderTexture_IsValid(first.texture));
  CHECK(first.width == 8 && first.height == 16);
  CHECK(first.ascent == 11 && first.line_advance == 16);
  CHECK(first.reveal_cluster_count == 1);
  CHECK(first.reveal_clusters != &fake.cluster);
  CHECK(first.reveal_clusters[0].end_utf8_byte == strlen("Menu"));
  CHECK(first.ink_bounds.x == 1 && first.ink_bounds.w == 7);
  CHECK(first.ink_bounds.y == 0 && first.ink_bounds.h == 16);
  CHECK(first.cluster_ink_bounds != NULL);
  CHECK(!memcmp(&first.ink_bounds, &first.cluster_ink_bounds[0],
                sizeof(first.ink_bounds)));
  CHECK(fake.calls == 1 && fake.releases == 1);
  CHECK(render.creates == 1 && render.uploads == 1);
  CHECK(render.last_descriptor.usage == kArRenderTextureUsage_Static);
  CHECK(render.last_descriptor.filter == kArRenderFilter_Linear);

  ArTextSurface same;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &first_request,
      &same, error, sizeof(error)));
  CHECK(ArRenderTexture_Equals(first.texture, same.texture));
  CHECK(first.cluster_ink_bounds == same.cluster_ink_bounds);
  CHECK(fake.calls == 1 && render.uploads == 1);

  ArTextRasterRequest second_request = Request("Status");
  ArTextSurface second;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &second_request,
      &second, error, sizeof(error)));
  CHECK(fake.calls == 2 && render.uploads == 2);
  CHECK(!ArRenderTexture_Equals(first.texture, second.texture));

  ArTextRasterRequest third_request = Request("Offerings");
  ArTextSurface third;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &third_request,
      &third, error, sizeof(error)));
  CHECK(render.destroys == 1);
  CHECK(cache.stats.evictions == 1);

  /* A failed replacement does not evict another usable entry. */
  const int destroys_before_failure = render.destroys;
  fake.fail = true;
  ArTextRasterRequest failed_request = Request("Failure");
  ArTextSurface failed;
  CHECK(!ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &failed_request,
      &failed, error, sizeof(error)));
  CHECK(strstr(error, "fake raster failure") != NULL);
  CHECK(render.destroys == destroys_before_failure);
  fake.fail = false;

  /* Static menu acquisition after many frames is still a pure hit. */
  const int calls_before_repeat = fake.calls;
  const int uploads_before_repeat = render.uploads;
  for (int frame = 0; frame < 120; ++frame) {
    CHECK(ArTextSurfaceCache_Acquire(
        &cache, &device, &rasterizer, &third_request,
        &same, error, sizeof(error)));
  }
  CHECK(fake.calls == calls_before_repeat);
  CHECK(render.uploads == uploads_before_repeat);

  const ArTextSurfaceCacheStats *stats = ArTextSurfaceCache_GetStats(&cache);
  CHECK(stats && stats->lookups == 125);
  CHECK(stats && stats->hits == 121);
  CHECK(stats && stats->misses == 4);
  CHECK(stats && stats->rasterize_calls == 4);
  CHECK(stats && stats->upload_calls == 3);
  CHECK(stats && stats->failures == 1);

  for (unsigned i = 0; i < 120; ++i) {
    CHECK(!ArTextSurfaceCache_Acquire(
        &cache, &device, &rasterizer, &failed_request,
        &failed, error, sizeof(error)));
    CHECK(strstr(error, "fake raster failure") != NULL);
  }
  CHECK(fake.calls == calls_before_repeat);
  CHECK(stats->negative_hits == 120);
  /* Retry after any relevant layout/content/font change, without needing a
   * restart, while successful entries remain independent of failed work. */
  failed_request.maximum_width += 10;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &failed_request,
      &failed, error, sizeof(error)));
  CHECK(fake.calls == calls_before_repeat + 1);

  ArTextRasterRequest font_changed = third_request;
  ++font_changed.font_revision;
  const ArTextCacheKey old_key = ArTextSurfaceCache_MakeKey(
      &rasterizer, &third_request);
  const ArTextCacheKey new_key = ArTextSurfaceCache_MakeKey(
      &rasterizer, &font_changed);
  CHECK(!ArTextCacheKey_Equals(old_key, new_key));

  ArTextRasterRequest fit_changed = third_request;
  fit_changed.minimum_font_pixels = 20;
  CHECK(!ArTextCacheKey_Equals(
      old_key, ArTextSurfaceCache_MakeKey(&rasterizer, &fit_changed)));

  ArTextRasterRequest language_changed = third_request;
  language_changed.language_bcp47 = "de";
  language_changed.language_bcp47_bytes = 2;
  const ArTextCacheKey language_key = ArTextSurfaceCache_MakeKey(
      &rasterizer, &language_changed);
  CHECK(!ArTextCacheKey_Equals(old_key, language_key));

  ArTextRasterRequest ink_changed = third_request;
  ink_changed.band_rgb = 0xffce00;
  CHECK(!ArTextCacheKey_Equals(old_key, ArTextSurfaceCache_MakeKey(&rasterizer, &ink_changed)));
  ink_changed = third_request;
  ink_changed.body_rgb = 0xffff9c;
  CHECK(!ArTextCacheKey_Equals(old_key, ArTextSurfaceCache_MakeKey(&rasterizer, &ink_changed)));
  ink_changed = third_request;
  ink_changed.flags |= kArTextRasterFlag_Italic;
  CHECK(!ArTextCacheKey_Equals(old_key, ArTextSurfaceCache_MakeKey(&rasterizer, &ink_changed)));
  ink_changed = third_request;
  ink_changed.shadow_enabled = true;
  const ArTextCacheKey shadow_key = ArTextSurfaceCache_MakeKey(&rasterizer, &ink_changed);
  CHECK(!ArTextCacheKey_Equals(old_key, shadow_key));
  ink_changed.shadow_rgb = 0x222233;
  CHECK(!ArTextCacheKey_Equals(shadow_key, ArTextSurfaceCache_MakeKey(&rasterizer, &ink_changed)));
  ink_changed = third_request;
  ink_changed.shadow_shape = kArTextShadow_Keyline;
  CHECK(!ArTextCacheKey_Equals(old_key, ArTextSurfaceCache_MakeKey(&rasterizer, &ink_changed)));
  ink_changed = third_request;
  ink_changed.flags |= kArTextRasterFlag_SlantAsciiNumerals;
  CHECK(!ArTextCacheKey_Equals(old_key, ArTextSurfaceCache_MakeKey(&rasterizer, &ink_changed)));

  ArTextSurfaceCache_Destroy(&cache, &device);
  CHECK(render.destroys == render.creates);
  ArTextRasterizer_Reset(&rasterizer);
  ArRenderDevice_Reset(&device);
}

/* A momentary allocation/resource failure must not become this key's permanent
 * answer: the player should not have to change locale, font or window size to
 * get their text back. A deterministic failure still costs one raster. */
static void TestTransientFailureRecovers(void) {
  FakeRenderBackend render = {0};
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &kRenderOps, &render,
      (ArRenderCapabilities){
        .maximum_texture_width = 512,
        .maximum_texture_height = 512,
      }));
  FakeRasterizer fake = {0};
  ArTextRasterizer rasterizer;
  CHECK(ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 23));
  ArTextSurfaceCache cache;
  CHECK(ArTextSurfaceCache_Init(&cache, 4));
  char error[256];

  ArTextRasterRequest request = Request("Offerings");
  ArTextSurface surface;
  fake.fail = true;
  fake.fail_kind = kArTextRasterFailure_Retryable;
  CHECK(!ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &request, &surface, error, sizeof(error)));
  CHECK(fake.calls == 1);

  /* The identical request recovers on the next lookup once the backend does,
   * with no settings, font or window change. */
  fake.fail = false;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &request, &surface, error, sizeof(error)));
  CHECK(fake.calls == 2 && cache.stats.negative_hits == 0);

  /* A backend that stays broken is asked again with growing backoff, not once
   * per frame, and the remembered answer is served in between. */
  ArTextRasterRequest broken = Request("Status");
  fake.fail = true;
  const int calls_before = fake.calls;
  for (int frame = 0; frame < 200; ++frame) {
    CHECK(!ArTextSurfaceCache_Acquire(
        &cache, &device, &rasterizer, &broken, &surface, error,
        sizeof(error)));
    CHECK(strstr(error, "fake raster failure") != NULL);
  }
  const int retries = fake.calls - calls_before;
  CHECK(retries > 1 && retries < 20);
  CHECK(cache.stats.negative_hits == (uint64_t)(200 - retries));

  /* One key's transient failures must not crowd another key's answer out of
   * the bounded negative cache. */
  fake.fail_kind = kArTextRasterFailure_Deterministic;
  ArTextRasterRequest impossible = Request("Impossible");
  CHECK(!ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &impossible, &surface, error,
      sizeof(error)));
  const int calls_after_impossible = fake.calls;
  for (int frame = 0; frame < 50; ++frame) {
    CHECK(!ArTextSurfaceCache_Acquire(
        &cache, &device, &rasterizer, &impossible, &surface, error,
        sizeof(error)));
  }
  CHECK(fake.calls == calls_after_impossible);

  /* Any successful rasterization is evidence the backend recovered, so the
   * keys still waiting out a backoff are asked again at once; the
   * deterministic answer stands. */
  fake.fail = false;
  ArTextRasterRequest other = Request("Sky Palace");
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &other, &surface, error, sizeof(error)));
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &broken, &surface, error, sizeof(error)));
  const int calls_after_recovery = fake.calls;
  CHECK(!ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &impossible, &surface, error,
      sizeof(error)));
  CHECK(fake.calls == calls_after_recovery);

  ArTextSurfaceCache_Destroy(&cache, &device);
  ArTextRasterizer_Reset(&rasterizer);
  ArRenderDevice_Reset(&device);
}

/* An entry count is not a memory budget, and an impossible field must be
 * refused before any font work rather than after a full-size raster. */
static void TestByteBudgetAndOversizeRejection(void) {
  FakeRenderBackend render = {0};
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &kRenderOps, &render,
      (ArRenderCapabilities){
        .maximum_texture_width = 8192,
        .maximum_texture_height = 8192,
      }));
  FakeRasterizer fake = {0};
  ArTextRasterizer rasterizer;
  CHECK(ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 23));
  ArTextSurfaceCache cache;
  CHECK(ArTextSurfaceCache_Init(&cache, 8));
  char error[256];
  ArTextSurface surface;

  /* Bounds whose bitmap could never be worth keeping are refused with no
   * rasterization at all. */
  ArTextRasterRequest huge = Request("Impossible");
  huge.maximum_width = 60000;
  huge.maximum_height = 60000;
  CHECK(!ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &huge, &surface, error, sizeof(error)));
  CHECK(fake.calls == 0 && render.uploads == 0);
  CHECK(cache.stats.oversize_rejects == 1);
  CHECK(strstr(error, "per-request texture ceiling") != NULL);

  /* Ordinary entries report what they own, and the peak is not forgotten. */
  static const char *const kTexts[] = {"Aitos", "Bloodpool", "Fillmore",
                                       "Kasandora", "Marahna", "Northwall"};
  ArTextRasterRequest first = Request(kTexts[0]);
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &first, &surface, error, sizeof(error)));
  const uint64_t one = cache.stats.texture_bytes;
  CHECK(one == (uint64_t)surface.width * surface.height * 4);
  CHECK(cache.stats.peak_texture_bytes == one);

  /* A budget of two entries' worth keeps the cache at that size, evicting the
   * least recently used, while the peak still records the high-water mark. */
  ArTextSurfaceCache_SetByteBudget(&cache, one * 2u);
  for (size_t i = 1; i < sizeof(kTexts) / sizeof(kTexts[0]); ++i) {
    ArTextRasterRequest request = Request(kTexts[i]);
    CHECK(ArTextSurfaceCache_Acquire(
        &cache, &device, &rasterizer, &request, &surface, error,
        sizeof(error)));
  }
  CHECK(cache.stats.texture_bytes <= one * 2u);
  CHECK(cache.stats.peak_texture_bytes >= cache.stats.texture_bytes);
  CHECK(cache.stats.evictions > 0);
  CHECK(render.destroys == (int)cache.stats.evictions);

  /* A frame being prepared never loses a texture it already holds, however
   * tight the budget: pinned entries are skipped and the budget is exceeded
   * rather than breaking the frame. */
  ArTextSurfaceCache_SetByteBudget(&cache, 1);
  ArTextSurfaceCache_BeginFrame(&cache);
  ArTextSurface held[4];
  for (int i = 0; i < 4; ++i) {
    ArTextRasterRequest request = Request(kTexts[i]);
    CHECK(ArTextSurfaceCache_Acquire(
        &cache, &device, &rasterizer, &request, &held[i], error,
        sizeof(error)));
  }
  for (int i = 0; i < 4; ++i) {
    bool live = false;
    for (size_t entry = 0; entry < cache.capacity; ++entry)
      if (cache.entries[entry].valid &&
          ArRenderTexture_Equals(cache.entries[entry].surface.texture,
                                 held[i].texture))
        live = true;
    CHECK(live);
  }

  ArTextSurfaceCache_Destroy(&cache, &device);
  CHECK(render.destroys == render.creates);
  ArTextRasterizer_Reset(&rasterizer);
  ArRenderDevice_Reset(&device);
}

static void TestOwnershipAcrossStatsResetAndEviction(void) {
  static const struct { ArRenderPixelFormat format; int bytes; } formats[] = {
    {kArRenderPixelFormat_Argb8888, 4}, {kArRenderPixelFormat_Abgr8888, 4},
    {kArRenderPixelFormat_Rgba8888, 4}, {kArRenderPixelFormat_Rgb565, 2},
    {kArRenderPixelFormat_Rgba4444, 2}, {kArRenderPixelFormat_A8, 1},
  };
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
    FakeRenderBackend render = {0};
    ArRenderDevice device;
    CHECK(ArRenderDevice_Init(&device, &kRenderOps, &render,
                             (ArRenderCapabilities){0}));
    FakeRasterizer fake = {.format = formats[i].format,
                           .pitch_bytes = 64 * formats[i].bytes};
    ArTextRasterizer rasterizer;
    CHECK(ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 23));
    ArTextSurfaceCache cache;
    CHECK(ArTextSurfaceCache_Init(&cache, 2));
    ArTextSurface first, second;
    ArTextRasterRequest a = Request("Menu"), b = Request("Longer");
    char error[256];
    /* Even without a frame pin, a successful acquisition must own a live
     * texture when the budget is smaller than a single entry. */
    ArTextSurfaceCache_SetByteBudget(&cache, 1);
    CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &a,
                                     &first, error, sizeof(error)));
    const uint64_t first_bytes = 8u * 16u * (unsigned)formats[i].bytes;
    CHECK(render.destroys == 0);
    CHECK(cache.entries[0].valid);
    CHECK(ArRenderTexture_Equals(cache.entries[0].surface.texture, first.texture));
    CHECK(cache.stats.texture_bytes == first_bytes);
    CHECK(render.last_descriptor.format == formats[i].format);

    ArTextSurfaceCache_ResetStats(&cache);
    CHECK(cache.stats.lookups == 0 && cache.stats.upload_calls == 0);
    CHECK(cache.stats.texture_bytes == first_bytes);
    CHECK(cache.stats.peak_texture_bytes == first_bytes);
    CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &a,
                                     &second, error, sizeof(error)));
    CHECK(ArRenderTexture_Equals(first.texture, second.texture));
    CHECK(fake.calls == 1 && cache.stats.hits == 1);
    CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &b,
                                     &second, error, sizeof(error)));
    const uint64_t second_bytes = 12u * 16u * (unsigned)formats[i].bytes;
    CHECK(render.destroys == 1);
    CHECK(cache.entries[1].valid);
    CHECK(ArRenderTexture_Equals(cache.entries[1].surface.texture, second.texture));
    CHECK(cache.stats.texture_bytes == second_bytes);
    CHECK(cache.stats.peak_texture_bytes == first_bytes + second_bytes);
    ArTextSurfaceCache_ResetStats(&cache);
    CHECK(cache.stats.texture_bytes == second_bytes);
    CHECK(cache.stats.peak_texture_bytes == second_bytes);
    ArTextSurfaceCache_Destroy(&cache, &device);
    CHECK(render.destroys == render.creates);
    ArTextRasterizer_Reset(&rasterizer);
    ArRenderDevice_Reset(&device);
  }
}

static void TestFramePinsSurviveEntryPressure(void) {
  FakeRenderBackend render = {0};
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &kRenderOps, &render,
                           (ArRenderCapabilities){0}));
  FakeRasterizer fake = {0};
  ArTextRasterizer rasterizer;
  CHECK(ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 23));
  ArTextSurfaceCache cache;
  CHECK(ArTextSurfaceCache_Init(&cache, 1));
  ArTextSurface first, second;
  ArTextRasterRequest a = Request("Menu"), b = Request("Other");
  char error[256];
  ArTextSurfaceCache_SetByteBudget(&cache, 1);
  ArTextSurfaceCache_BeginFrame(&cache);
  CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &a,
                                   &first, error, sizeof(error)));
  CHECK(!ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &b,
                                    &second, error, sizeof(error)));
  CHECK(strstr(error, "all in use") != NULL);
  CHECK(!ArRenderTexture_IsValid(second.texture));
  CHECK(fake.calls == 1 && render.destroys == 0);
  CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &a,
                                   &second, error, sizeof(error)));
  CHECK(ArRenderTexture_Equals(first.texture, second.texture));
  /* The next frame can retry the identical request: capacity failures must
   * neither poison the negative cache nor retain the previous frame's pins. */
  ArTextSurfaceCache_BeginFrame(&cache);
  CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &b,
                                   &second, error, sizeof(error)));
  CHECK(fake.calls == 2 && render.destroys == 1);
  ArTextSurfaceCache_EndFrame(&cache);
  CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &a,
                                   &first, error, sizeof(error)));
  CHECK(fake.calls == 3 && render.destroys == 2);
  ArTextSurfaceCache_Destroy(&cache, &device);
  CHECK(render.destroys == render.creates);
  ArTextRasterizer_Reset(&rasterizer);
  ArRenderDevice_Reset(&device);
}

static void TestPixelationTreatments(void) {
  FakeRenderBackend render = {0};
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &kRenderOps, &render,
      (ArRenderCapabilities){
        .maximum_texture_width = 512,
        .maximum_texture_height = 512,
      }));
  FakeRasterizer fake = {0};
  ArTextRasterizer rasterizer;
  CHECK(ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 23));
  ArTextSurfaceCache cache;
  CHECK(ArTextSurfaceCache_Init(&cache, 4));
  char error[256];

  ArTextRasterRequest low = Request("ABCD");
  low.font_pixels = low.minimum_font_pixels = 23;
  low.maximum_width = 255;
  low.pixelation = kArTextPixelation_LowResolution;
  low.pixelation_size = 2;
  ArTextSurface low_surface;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &low,
      &low_surface, error, sizeof(error)));
  CHECK(fake.last_request.font_pixels == 11);
  CHECK(fake.last_request.minimum_font_pixels == 11);
  CHECK(fake.last_request.maximum_width == 127);
  CHECK(fake.last_request.maximum_height == 32);
  CHECK(fake.last_request.pixelation == kArTextPixelation_None);
  CHECK(low_surface.width == 16 && low_surface.height == 32);
  CHECK(low_surface.ascent == 22 && low_surface.line_advance == 32);
  CHECK(low_surface.reveal_clusters[0].width == 16);
  CHECK(low_surface.reveal_clusters[0].height == 32);
  CHECK(low_surface.ink_bounds.x == 2 && low_surface.ink_bounds.w == 14);
  CHECK(low_surface.ink_bounds.h == 32);
  CHECK(low_surface.cluster_ink_bounds[0].x == 2);
  CHECK(render.uploaded_pixels[0] == render.uploaded_pixels[1]);
  CHECK(render.uploaded_pixels[0] == render.uploaded_pixels[64]);
  CHECK(render.uploaded_pixels[2] != render.uploaded_pixels[0]);

  ArTextRasterRequest mosaic = Request("ABCD");
  mosaic.pixelation = kArTextPixelation_Mosaic;
  mosaic.pixelation_size = 2;
  ArTextSurface mosaic_surface;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &mosaic,
      &mosaic_surface, error, sizeof(error)));
  CHECK(fake.last_request.font_pixels == 24);
  CHECK(fake.last_request.maximum_width == 256);
  CHECK(mosaic_surface.width == 8 && mosaic_surface.height == 16);
  /* Sampling x=1 fills the first block: final ink differs from source ink. */
  CHECK(mosaic_surface.ink_bounds.x == 0 && mosaic_surface.ink_bounds.w == 8);
  CHECK(mosaic_surface.cluster_ink_bounds[0].w == 8);
  CHECK(render.uploaded_pixels[0] == render.uploaded_pixels[1]);
  CHECK(render.uploaded_pixels[0] == render.uploaded_pixels[64]);
  CHECK(render.uploaded_pixels[2] != render.uploaded_pixels[0]);

  const ArTextCacheKey low_key = ArTextSurfaceCache_MakeKey(
      &rasterizer, &low);
  const ArTextCacheKey mosaic_key = ArTextSurfaceCache_MakeKey(
      &rasterizer, &mosaic);
  CHECK(!ArTextCacheKey_Equals(low_key, mosaic_key));
  mosaic.pixelation_size = 3;
  CHECK(!ArTextCacheKey_Equals(
      mosaic_key, ArTextSurfaceCache_MakeKey(&rasterizer, &mosaic)));

  fake.line_advance = 8;
  ++mosaic.source_revision;
  mosaic.pixelation_size = 4;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &mosaic,
      &mosaic_surface, error, sizeof(error)));
  CHECK(render.uploaded_pixels[0] != render.uploaded_pixels[1]);
  CHECK(render.uploaded_pixels[0] != render.uploaded_pixels[64]);
  ++low.source_revision;
  low.font_pixels = low.minimum_font_pixels = 10;
  low.pixelation_size = 4;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &low,
      &low_surface, error, sizeof(error)));
  CHECK(fake.last_request.font_pixels == 10);
  CHECK(low_surface.width == 8 && low_surface.height == 16);

  ArTextSurfaceCache_Destroy(&cache, &device);
  ArTextRasterizer_Reset(&rasterizer);
  ArRenderDevice_Reset(&device);
}

static void TestInkFormatsAndBounds(void) {
  const ArRenderPixelFormat formats[] = {
    kArRenderPixelFormat_Argb8888, kArRenderPixelFormat_Abgr8888,
    kArRenderPixelFormat_Rgba8888, kArRenderPixelFormat_Rgba4444,
    kArRenderPixelFormat_A8, kArRenderPixelFormat_Rgb565,
  };
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
    uint8_t storage[8 * 32] = {0}; /* Deliberately padded pitch. */
    const int bytes = i < 3 ? 4 : i == 4 ? 1 : 2;
    for (int y = 3; y < 6; ++y) {
      for (int x = 2; x < 5; ++x) {
        uint8_t *pixel = storage + y * 32 + x * bytes;
        if (bytes == 4) {
          const uint32_t word = i < 2 ? UINT32_C(0x80000000) : UINT32_C(0x80);
          memcpy(pixel, &word, sizeof(word));
        } else if (bytes == 2) {
          const uint16_t word = 8;
          memcpy(pixel, &word, sizeof(word));
        } else *pixel = 128;
      }
    }
    ArTextBitmap bitmap = {.pixels = storage, .width = 7, .height = 8,
                          .pitch_bytes = 32, .format = formats[i]};
    ArRenderRectI ink = ArTextBitmap_InkBounds(&bitmap, (ArRenderRectI){0, 0, 7, 8});
    if (formats[i] == kArRenderPixelFormat_Rgb565) {
      CHECK(ink.x == 0 && ink.y == 0 && ink.w == 7 && ink.h == 8);
    } else {
      CHECK(ink.x == 2 && ink.y == 3 && ink.w == 3 && ink.h == 3);
      ink = ArTextBitmap_InkBounds(&bitmap, (ArRenderRectI){3, 4, 4, 4});
      CHECK(ink.x == 3 && ink.y == 4 && ink.w == 2 && ink.h == 2);
      ink = ArTextBitmap_InkBounds(&bitmap, (ArRenderRectI){0, 0, 2, 2});
      CHECK(ink.w == 0 && ink.h == 0);
    }
    CHECK(!ArTextBitmap_InkBounds(&bitmap, (ArRenderRectI){-1, 0, 2, 2}).h);
    CHECK(!ArTextBitmap_InkBounds(&bitmap, (ArRenderRectI){1, 0, INT32_MAX, 2}).h);
    bitmap.pitch_bytes = 1;
    CHECK(!ArTextBitmap_InkBounds(&bitmap, (ArRenderRectI){0, 0, 7, 8}).h);
  }
}

static bool CreateUiFont(void *context, ArTextBackendInstance *instance,
                          const ArTextBackendConfig *config,
                          char *error, size_t error_capacity) {
  (void)error; (void)error_capacity;
  instance->implementation = context;
  return ArTextRasterizer_Init(&instance->rasterizer, &kRasterOps, context,
                                config->font_revision);
}
static void DestroyUiFont(void *context, ArTextBackendInstance *instance) {
  (void)context; (void)instance;
}

static void TestInterfaceTextCacheAndLifecycle(void) {
  ArHostFontResources font_store = {0};
  FakeRenderBackend render = {0};
  FakeRasterizer raster = {0};
  ArRenderDevice device = {0};
  CHECK(ArRenderDevice_Init(&device, &kRenderOps, &render, (ArRenderCapabilities){0}));
  const ArTextBackendOps ops = {.struct_size = sizeof(ops),
      .abi_version = AR_TEXT_BACKEND_ABI_VERSION,
      .create = CreateUiFont, .destroy = DestroyUiFont};
  const ArTextBackend backend = {.ops = &ops, .context = &raster};
  const ArTextBackendConfig fonts = {.struct_size = sizeof(fonts),
      .abi_version = AR_TEXT_BACKEND_CONFIG_ABI_VERSION,
      .font_stack_id = "interface", .primary_font = 1,
      .resources = ArHostFontResources_Provider(&font_store),
      .font_revision = 7, .cached_size_capacity = 4};
  ArUiTextRenderer ui = {0};
  CHECK(ArUiTextRenderer_Init(&ui, &device, &backend, &fonts, NULL, 0));
  CHECK(raster.calls == 1); /* Preflight before transactional publication. */
  ArUiTextRun run = {.struct_size = sizeof(run),
      .abi_version = AR_UI_TEXT_RUN_ABI_VERSION,
      .utf8 = "Français 日本語", .utf8_bytes = strlen("Français 日本語"),
      .bounds = {10, 20, 256, 32}, .tint = {1, 1, 1, 1}};
  CHECK(ArUiTextRenderer_Draw(&ui, &run));
  CHECK(raster.calls == 2 && render.draws == 1);
  CHECK(raster.last_request.utf8_bytes == run.utf8_bytes);
  CHECK(!memcmp(raster.last_request.utf8, run.utf8, run.utf8_bytes));
  CHECK(raster.last_request.direction == kArTextDirection_Auto);
  CHECK(render.last_destination.x == 10 && render.last_destination.y == 28);
  const int uploads = render.uploads;
  run.bounds.x = 30;
  run.tint = (ArRenderColorF){0.5f, 0.3f, 0.2f, 0.7f};
  run.alignment = kArUiTextAlignment_Right;
  CHECK(ArUiTextRenderer_Draw(&ui, &run));
  CHECK(raster.calls == 2 && render.uploads == uploads);
  CHECK(render.last_destination.x + render.last_destination.w == 286);
  CHECK(ArUiTextRenderer_GetStats(&ui)->hits == 1);
  CHECK(ArUiTextRenderer_GetStats(&ui)->texture_bytes < (16u << 20));
  int measured_width = 0, measured_height = 0;
  CHECK(ArUiTextRenderer_Measure(&ui, &run, &measured_width, &measured_height));
  CHECK(measured_width == (int)run.utf8_bytes * 2 && measured_height == 16);
  CHECK(raster.calls == 2 && render.uploads == uploads && render.draws == 2);

  /* Rejected initialization cannot destroy the working font/cache. */
  raster.fail = true;
  CHECK(!ArUiTextRenderer_Init(&ui, &device, &backend, &fonts, NULL, 0));
  CHECK(ArUiTextRenderer_IsReady(&ui));
  raster.fail = false;
  const int calls_after_failure = raster.calls;
  CHECK(ArUiTextRenderer_Draw(&ui, &run));
  CHECK(raster.calls == calls_after_failure);
  ArUiTextRenderer_ClearTextures(&ui);
  CHECK(ArUiTextRenderer_Draw(&ui, &run));
  CHECK(raster.calls == calls_after_failure + 1);
  run.language_bcp47 = "ja";
  CHECK(ArUiTextRenderer_Draw(&ui, &run));
  CHECK(raster.calls == calls_after_failure + 2);
  run.bounds.w = 8193;
  CHECK(!ArUiTextRenderer_Draw(&ui, &run));
  CHECK(raster.calls == calls_after_failure + 2);
  run.bounds.w = 256;
  run.abi_version++;
  CHECK(!ArUiTextRenderer_Draw(&ui, &run));
  ArUiTextRenderer_Destroy(&ui);
  CHECK(!ArUiTextRenderer_IsReady(&ui));
  CHECK(render.creates == render.destroys);
  ArUiTextRenderer_Destroy(&ui);
}

static void TestEffectPieces(void) {
  const uint32_t pixels[6] = {255, 255, 255, 255, 255, 255};
  const ArTextRevealCluster clusters[] = {{1, 0, 0, 0, 4, 1}, {2, 0, 2, 0, 4, 1}};
  const ArTextBitmap overlap = {.pixels = pixels, .width = 6, .height = 1,
      .pitch_bytes = 24, .format = kArRenderPixelFormat_Rgba8888,
      .reveal_clusters = clusters, .reveal_cluster_count = 2};
  uint32_t *owners = ArTextBitmap_BuildOwnership(&overlap);
  CHECK(owners != NULL);
  if (owners) {
    CHECK(owners[0] == 1 && owners[1] == 1);
    CHECK(owners[2] == 2 && owners[3] == 2 && owners[4] == 2 && owners[5] == 2);
    free(owners);
  }
  for (int effect = kArTextPixelation_None; effect <= kArTextPixelation_Mosaic; ++effect) {
    FakeRasterizer fake = {.with_owners = true};
    FakeRenderBackend render = {0};
    ArRenderDevice device = {0};
    ArTextRasterizer rasterizer = {0};
    ArTextSurfaceCache cache = {0};
    CHECK(ArRenderDevice_Init(&device, &kRenderOps, &render, (ArRenderCapabilities){0}));
    CHECK(ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 1));
    CHECK(ArTextSurfaceCache_Init(&cache, 2));
    ArTextRasterRequest request = Request("abcd");
    request.pixelation = (ArTextPixelation)effect;
    request.pixelation_size = effect ? 2 : 0;
    ArTextSurface surface;
    CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &request,
                                    &surface, NULL, 0));
    CHECK(surface.reveal_piece_count > 0);
    CHECK(cache.stats.effect_metadata_bytes >= surface.reveal_piece_count * sizeof(ArTextRevealPiece));
    unsigned visits[64 * 32] = {0};
    for (size_t i = 0; i < surface.reveal_piece_count; ++i) {
      const ArTextRevealPiece *piece = &surface.reveal_pieces[i];
      CHECK(piece->cluster_index < 2);
      for (int y = piece->source.y; y < piece->source.y + piece->source.h; ++y)
        for (int x = piece->source.x; x < piece->source.x + piece->source.w; ++x) {
          CHECK(x >= 0 && x < surface.width && y >= 0 && y < surface.height);
          CHECK(++visits[y * 64 + x] == 1);
          int sx = x, sy = y;
          if (effect == kArTextPixelation_LowResolution) { sx /= 2; sy /= 2; }
          if (effect == kArTextPixelation_Mosaic) { sx = x / 2 * 2 + 1; sy = y / 2 * 2 + 1; }
          CHECK(fake.owners[sy * 8 + sx] == piece->cluster_index + 1);
        }
    }
    for (int y = 0; y < surface.height; ++y)
      for (int x = 0; x < surface.width; ++x)
        CHECK(visits[y * 64 + x] == ((render.uploaded_pixels[y * 64 + x] & 255) != 0));
    const uint64_t metadata = cache.stats.effect_metadata_bytes;
    ArTextSurfaceCache_ResetStats(&cache);
    CHECK(cache.stats.effect_metadata_bytes == metadata);
    CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &request,
                                    &surface, NULL, 0));
    CHECK(fake.calls == 1 && render.uploads == 1);
    ArTextSurfaceCache_SetByteBudget(&cache, cache.stats.texture_bytes + metadata);
    request.source_revision++;
    CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &request,
                                    &surface, NULL, 0));
    CHECK(cache.stats.evictions == 1); /* CPU effects count against the budget. */
    ArTextSurfaceCache_Destroy(&cache, &device);
    CHECK(cache.stats.effect_metadata_bytes == 0);
    fake.invalid_owner = true;
    ArTextBitmap invalid;
    CHECK(!ArTextRasterizer_Rasterize(&rasterizer, &request, &invalid, NULL, NULL, 0));
    CHECK(invalid.pixels == NULL && invalid.pixel_owners == NULL);
  }
}

/* ApplyMosaic's loops as they stood before the band-row rewrite (2026-09-17),
 * kept verbatim apart from the allocation helpers, as the oracle for the
 * bytes the cache now uploads. */
static void ReferenceMosaic(const ArTextBitmap *source, int block_size,
                            uint8_t *output, int output_pitch) {
  const int bytes_per_pixel = 4;
  const uint8_t *input = (const uint8_t *)source->pixels;
  for (int block_y = 0; block_y < source->height;
       block_y += block_size) {
    const int sample_y = block_y + block_size / 2 < source->height
        ? block_y + block_size / 2 : source->height - 1;
    for (int block_x = 0; block_x < source->width;
         block_x += block_size) {
      const int sample_x = block_x + block_size / 2 < source->width
          ? block_x + block_size / 2 : source->width - 1;
      const uint8_t *sample =
          input + (size_t)sample_y * (size_t)source->pitch_bytes +
          (size_t)sample_x * (size_t)bytes_per_pixel;
      const int end_y = block_y + block_size < source->height
          ? block_y + block_size : source->height;
      const int end_x = block_x + block_size < source->width
          ? block_x + block_size : source->width;
      for (int y = block_y; y < end_y; ++y) {
        uint8_t *row = output + (size_t)y * (size_t)output_pitch;
        for (int x = block_x; x < end_x; ++x)
          memcpy(row + (size_t)x * (size_t)bytes_per_pixel,
                 sample, (size_t)bytes_per_pixel);
      }
    }
  }
}

static void TestMosaicMatchesOriginal(void) {
  FakeRenderBackend render = {0};
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &kRenderOps, &render,
      (ArRenderCapabilities){
        .maximum_texture_width = 512,
        .maximum_texture_height = 512,
      }));
  FakeRasterizer fake = {0};
  ArTextRasterizer rasterizer;
  CHECK(ArTextRasterizer_Init(&rasterizer, &kRasterOps, &fake, 23));
  ArTextSurfaceCache cache;
  CHECK(ArTextSurfaceCache_Init(&cache, 4));
  char error[256];
  static const char kText[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
  int compared = 0;
  /* Every width 2..64 against every block 2..8 covers partial blocks on both
   * axes and blocks wider than the whole surface. */
  for (int block = 2; block <= 8; ++block) {
    fake.line_advance = block * 8;
    for (size_t length = 1; length <= 32; ++length) {
      char text[33];
      memcpy(text, kText, length);
      text[length] = 0;
      ArTextRasterRequest request = Request(text);
      request.pixelation = kArTextPixelation_Mosaic;
      request.pixelation_size = block;
      ArTextSurface surface;
      if (!ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &request,
                                      &surface, error, sizeof(error))) {
        CHECK(!"mosaic acquire failed");
        continue;
      }
      const ArTextBitmap source = {
        .pixels = fake.pixels, .width = (int)length * 2, .height = 16,
        .pitch_bytes = 64 * (int)sizeof(uint32_t),
        .format = kArRenderPixelFormat_Rgba8888,
      };
      uint32_t expected[64 * 16];
      ReferenceMosaic(&source, block, (uint8_t *)expected, 64 * 4);
      CHECK(surface.width == source.width && surface.height == 16);
      for (int y = 0; y < 16; ++y)
        CHECK(!memcmp(&render.uploaded_pixels[y * 64], &expected[y * 64],
                      (size_t)source.width * sizeof(uint32_t)));
      ++compared;
    }
  }
  CHECK(compared == 7 * 32);
  ArTextSurfaceCache_Destroy(&cache, &device);
}

/* Returns one scripted bitmap, so a test can choose exact pixels, ownership
 * and crop offsets. Ownership follows alpha, as the contract requires. */
typedef struct ScriptedRasterizer {
  uint32_t *pixels;
  uint32_t *owners;
  int width, height, crop_left, crop_top, line_advance;
  ArTextRevealCluster cluster;
  ArTextRasterRequest last_request;
} ScriptedRasterizer;

static bool ScriptedRasterize(void *context, const ArTextRasterRequest *request,
                              ArTextBitmap *bitmap, ArTextRasterFailure *failure,
                              char *error, size_t error_capacity) {
  ScriptedRasterizer *scripted = (ScriptedRasterizer *)context;
  (void)failure;
  (void)error;
  (void)error_capacity;
  scripted->last_request = *request;
  for (int i = 0; i < scripted->width * scripted->height; ++i)
    scripted->owners[i] = (scripted->pixels[i] & 255u) ? 1u : 0u;
  scripted->cluster = (ArTextRevealCluster){
      .end_utf8_byte = request->utf8_bytes,
      .width = scripted->width, .height = scripted->height};
  *bitmap = (ArTextBitmap){
      .struct_size = sizeof(*bitmap),
      .abi_version = AR_TEXT_BITMAP_ABI_VERSION,
      .pixels = scripted->pixels,
      .width = scripted->width, .height = scripted->height,
      .pitch_bytes = scripted->width * 4,
      .format = kArRenderPixelFormat_Rgba8888,
      .ascent = 11, .descent = 3, .line_advance = scripted->line_advance,
      .reveal_clusters = &scripted->cluster, .reveal_cluster_count = 1,
      .token = 1, .pixel_owners = scripted->owners,
      .font_pixels = request->font_pixels,
      .crop_left = scripted->crop_left, .crop_top = scripted->crop_top,
  };
  return true;
}

static void ScriptedRelease(void *context, ArTextBitmap *bitmap) {
  (void)context;
  if (bitmap) memset(bitmap, 0, sizeof(*bitmap));
}

static const ArTextRasterizerOps kScriptedOps = {
  .struct_size = sizeof(ArTextRasterizerOps),
  .abi_version = AR_TEXT_RASTERIZER_ABI_VERSION,
  .rasterize = ScriptedRasterize,
  .release_bitmap = ScriptedRelease,
};

/* Keeps the last uploaded texture whole, whatever its size. */
typedef struct TextureCapture {
  FakeRenderBackend base;
  uint32_t pixels[256 * 256];
  int width, height;
} TextureCapture;

static bool CaptureCreate(void *context, const ArRenderTextureDesc *descriptor,
                          ArRenderTexture *texture) {
  TextureCapture *capture = (TextureCapture *)context;
  capture->width = descriptor->width;
  capture->height = descriptor->height;
  return CreateTexture(&capture->base, descriptor, texture);
}

static bool CaptureUpdate(void *context, ArRenderTexture texture,
                          const ArRenderRectI *destination,
                          const void *pixels, int pitch_bytes) {
  TextureCapture *capture = (TextureCapture *)context;
  (void)texture;
  (void)destination;
  if (!pixels || capture->width > 256 || capture->height > 256) return false;
  for (int y = 0; y < capture->height; ++y)
    memcpy(&capture->pixels[y * capture->width],
           (const uint8_t *)pixels + (size_t)y * (size_t)pitch_bytes,
           (size_t)capture->width * sizeof(uint32_t));
  return true;
}

static uint32_t NextRandom(uint64_t *state) {
  *state = *state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
  return (uint32_t)(*state >> 33);
}

/* A line mosaicked on its own must put exactly the pixels on screen that the
 * same pixels mosaicked inside their page do, once it is aligned to the page's
 * grid. The page here holds only the line, so the page's whole texture must be
 * the aligned line's texture, placed by the two origins. */
static void TestGridAlignedMosaicMatchesPage(void) {
  static TextureCapture capture;
  ArRenderBackendOps ops = kRenderOps;
  ops.create_texture = CaptureCreate;
  ops.update_texture = CaptureUpdate;
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &ops, &capture,
      (ArRenderCapabilities){
        .maximum_texture_width = 256, .maximum_texture_height = 256}));
  static ScriptedRasterizer scripted;
  static uint32_t page_pixels[128 * 96], line_pixels[128 * 96];
  static uint32_t page_owners[128 * 96], line_owners[128 * 96];
  static uint32_t page_texture[256 * 256];
  ArTextRasterizer rasterizer;
  CHECK(ArTextRasterizer_Init(&rasterizer, &kScriptedOps, &scripted, 31));
  ArTextSurfaceCache cache;
  CHECK(ArTextSurfaceCache_Init(&cache, 4));
  char error[256];
  uint64_t random = 0x5eed;
  int compared = 0, unaligned_differences = 0;
  for (int trial = 0; trial < 700; ++trial) {
    const int block = 2 + trial % 7;
    const int width = 48 + (int)(NextRandom(&random) % 64);
    const int height = 32 + (int)(NextRandom(&random) % 48);
    const int line_width = 1 + (int)(NextRandom(&random) % 24);
    const int line_height = 1 + (int)(NextRandom(&random) % 20);
    /* Clear of the page's far edges by a block: a page clamps its last
     * partial block's sample to its own edge, which a line inside it never
     * sees (the presenter's documented condition). */
    const int line_x = (int)(NextRandom(&random) %
                             (uint32_t)(width - line_width - block + 1));
    const int line_y = (int)(NextRandom(&random) %
                             (uint32_t)(height - line_height - block + 1));
    memset(page_pixels, 0, sizeof(page_pixels));
    for (int y = 0; y < line_height; ++y)
      for (int x = 0; x < line_width; ++x) {
        const uint32_t value = NextRandom(&random);
        const uint32_t pixel = (value & 3u) ? value | 1u : 0u;
        line_pixels[y * line_width + x] = pixel;
        page_pixels[(line_y + y) * width + line_x + x] = pixel;
      }

    scripted = (ScriptedRasterizer){
        .pixels = page_pixels, .owners = page_owners,
        .width = width, .height = height,
        .crop_left = (int)(NextRandom(&random) % 11),
        .crop_top = (int)(NextRandom(&random) % 5),
        .line_advance = block * 8};
    ArTextRasterRequest page_request = Request("page");
    page_request.source_revision = (uint64_t)trial + 100u;
    page_request.pixelation = kArTextPixelation_Mosaic;
    page_request.pixelation_size = block;
    page_request.maximum_height = 256;
    ArTextSurface page;
    if (!ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer,
                                    &page_request, &page, error,
                                    sizeof(error))) {
      CHECK(!"page acquire failed");
      continue;
    }
    CHECK(page.mosaic_block == block && page.raster_scale == 1);
    CHECK(page.origin_x == -scripted.crop_left &&
          page.origin_y == -scripted.crop_top);
    CHECK(page.width == width && page.height == height);
    memcpy(page_texture, capture.pixels,
           (size_t)width * (size_t)height * sizeof(uint32_t));
    /* The line's layout origin, in the page's layout coordinates. */
    const int origin_x = line_x - page.origin_x;
    const int origin_y = line_y - page.origin_y;

    for (int aligned = 1; aligned >= 0; --aligned) {
      scripted = (ScriptedRasterizer){
          .pixels = line_pixels, .owners = line_owners,
          .width = line_width, .height = line_height,
          .line_advance = block * 8};
      ArTextRasterRequest line_request = Request("line");
      line_request.source_revision = page_request.source_revision;
      line_request.pixelation = kArTextPixelation_Mosaic;
      line_request.pixelation_size = block;
      line_request.maximum_height = 256;
      if (aligned) {
        line_request.align_pixelation_grid = true;
        line_request.pixelation_grid_x = -page.origin_x - origin_x;
        line_request.pixelation_grid_y = -page.origin_y - origin_y;
      }
      ArTextSurface line;
      if (!ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer,
                                      &line_request, &line, error,
                                      sizeof(error))) {
        CHECK(!"line acquire failed");
        continue;
      }
      if (aligned) {
        CHECK(line.width % block == 0 && line.height % block == 0);
        CHECK(line.origin_x >= 0 && line.origin_x < block &&
              line.origin_y >= 0 && line.origin_y < block);
        CHECK(line.ascent == 11 + line.origin_y);
        CHECK(line.reveal_clusters[0].x == line.origin_x &&
              line.reveal_clusters[0].y == line.origin_y);
      }
      /* Texture (u, v) of the line is page texture (u + dx, v + dy). */
      const int dx = origin_x + page.origin_x - line.origin_x;
      const int dy = origin_y + page.origin_y - line.origin_y;
      int mismatches = 0;
      for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
          const int u = x - dx, v = y - dy;
          const uint32_t expected = page_texture[y * width + x];
          const bool inside = u >= 0 && v >= 0 && u < line.width &&
                              v < line.height;
          const uint32_t actual = inside ? capture.pixels[v * line.width + u] : 0u;
          if ((expected & 255u) || (actual & 255u))
            mismatches += expected != actual;
        }
      for (int v = 0; v < line.height; ++v)
        for (int u = 0; u < line.width; ++u) {
          const int x = u + dx, y = v + dy;
          if ((x < 0 || y < 0 || x >= width || y >= height) &&
              (capture.pixels[v * line.width + u] & 255u))
            ++mismatches;
        }
      if (aligned) {
        CHECK(mismatches == 0);
        ++compared;
      } else {
        unaligned_differences += mismatches != 0;
      }
    }
  }
  CHECK(compared == 700);
  /* Positive control: without alignment the same comparison does fail. */
  CHECK(unaligned_differences > 100);
  ArTextSurfaceCache_Destroy(&cache, &device);
  ArTextRasterizer_Reset(&rasterizer);
  ArRenderDevice_Reset(&device);
}

/* A line cut from a page keeps the page's fitted size and low-resolution
 * scale; the backend only sees an ordinary request with no room to fit. */
static void TestExactRasterSize(void) {
  static TextureCapture capture;
  ArRenderBackendOps ops = kRenderOps;
  ops.create_texture = CaptureCreate;
  ops.update_texture = CaptureUpdate;
  ArRenderDevice device;
  CHECK(ArRenderDevice_Init(&device, &ops, &capture,
      (ArRenderCapabilities){
        .maximum_texture_width = 256, .maximum_texture_height = 256}));
  static ScriptedRasterizer scripted;
  static uint32_t pixels[16 * 8], owners[16 * 8];
  for (int i = 0; i < 16 * 8; ++i) pixels[i] = UINT32_C(0xffffffff);
  ArTextRasterizer rasterizer;
  CHECK(ArTextRasterizer_Init(&rasterizer, &kScriptedOps, &scripted, 37));
  ArTextSurfaceCache cache;
  CHECK(ArTextSurfaceCache_Init(&cache, 4));
  char error[256];

  ArTextRasterRequest request = Request("line");
  CHECK(ArTextRasterRequest_IsValid(&request));
  request.pixelation_grid_x = 3;
  CHECK(!ArTextRasterRequest_IsValid(&request)); /* grid without alignment */
  request.align_pixelation_grid = true;
  CHECK(ArTextRasterRequest_IsValid(&request));
  request = Request("line");
  request.raster_font_pixels = -1;
  CHECK(!ArTextRasterRequest_IsValid(&request));
  request.raster_font_pixels = 4097;
  CHECK(!ArTextRasterRequest_IsValid(&request));

  request = Request("line");
  request.font_pixels = 40;
  request.minimum_font_pixels = 26;
  const ArTextCacheKey fitted = ArTextSurfaceCache_MakeKey(&rasterizer, &request);
  request.raster_font_pixels = 31;
  CHECK(!ArTextCacheKey_Equals(
      fitted, ArTextSurfaceCache_MakeKey(&rasterizer, &request)));
  ArTextRasterRequest aligned = request;
  aligned.align_pixelation_grid = true;
  const ArTextCacheKey aligned_key =
      ArTextSurfaceCache_MakeKey(&rasterizer, &aligned);
  CHECK(!ArTextCacheKey_Equals(
      ArTextSurfaceCache_MakeKey(&rasterizer, &request), aligned_key));
  aligned.pixelation_grid_y = -5;
  CHECK(!ArTextCacheKey_Equals(
      aligned_key, ArTextSurfaceCache_MakeKey(&rasterizer, &aligned)));
  scripted = (ScriptedRasterizer){.pixels = pixels, .owners = owners,
      .width = 16, .height = 8, .crop_left = 2, .crop_top = 1,
      .line_advance = 8};
  ArTextSurface surface;
  CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &request,
                                   &surface, error, sizeof(error)));
  CHECK(scripted.last_request.font_pixels == 31 &&
        scripted.last_request.minimum_font_pixels == 31);
  CHECK(!scripted.last_request.raster_font_pixels &&
        !scripted.last_request.align_pixelation_grid);
  CHECK(surface.raster_font_pixels == 31 && surface.raster_scale == 1);
  CHECK(surface.mosaic_block == 1);
  CHECK(surface.origin_x == -2 && surface.origin_y == -1);

  /* Low resolution: the scale still comes from the requested size (40 / 8
   * allows 4), and the exact size is already in rasterized pixels. Grid
   * alignment is a mosaic matter and changes nothing here. */
  request.pixelation = kArTextPixelation_LowResolution;
  request.pixelation_size = 4;
  request.raster_font_pixels = 7;
  request.align_pixelation_grid = true;
  request.pixelation_grid_x = 1;
  CHECK(ArTextSurfaceCache_Acquire(&cache, &device, &rasterizer, &request,
                                   &surface, error, sizeof(error)));
  CHECK(scripted.last_request.font_pixels == 7 &&
        scripted.last_request.minimum_font_pixels == 7);
  CHECK(scripted.last_request.maximum_width == request.maximum_width / 4);
  CHECK(surface.raster_font_pixels == 7 && surface.raster_scale == 4);
  CHECK(surface.width == 64 && surface.height == 32);
  CHECK(surface.origin_x == -8 && surface.origin_y == -4);
  ArTextSurfaceCache_Destroy(&cache, &device);
  ArTextRasterizer_Reset(&rasterizer);
  ArRenderDevice_Reset(&device);
}

int main(void) {
  TestExactRasterSize();
  TestGridAlignedMosaicMatchesPage();
  TestEffectPieces();
  TestAbiValidation();
  TestCacheHitsMissesAndFailureAtomicity();
  TestTransientFailureRecovers();
  TestByteBudgetAndOversizeRejection();
  TestOwnershipAcrossStatsResetAndEviction();
  TestFramePinsSurviveEntryPressure();
  TestPixelationTreatments();
  TestMosaicMatchesOriginal();
  TestInkFormatsAndBounds();
  TestInterfaceTextCacheAndLifecycle();
  if (g_failures) {
    fprintf(stderr, "%d text-surface cache test(s) failed\n", g_failures);
    return 1;
  }
  puts("text-surface cache tests passed");
  return 0;
}
