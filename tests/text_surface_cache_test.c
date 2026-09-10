#include <stdio.h>
#include <string.h>

#include "localization/text_rasterizer.h"
#include "render/render_device.h"
#include "render/text_surface_cache.h"
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
  ArTextRevealCluster cluster;
  int calls;
  int releases;
  bool fail;
  ArTextRasterFailure fail_kind;
  bool invalid_bitmap;
  bool invalid_cluster_bounds;
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
  low.pixelation = kArTextPixelation_LowResolution;
  low.pixelation_size = 2;
  ArTextSurface low_surface;
  CHECK(ArTextSurfaceCache_Acquire(
      &cache, &device, &rasterizer, &low,
      &low_surface, error, sizeof(error)));
  CHECK(fake.last_request.font_pixels == 12);
  CHECK(fake.last_request.minimum_font_pixels == 12);
  CHECK(fake.last_request.maximum_width == 128);
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
      .font_stack_id = "interface", .primary_font_path = "host-resolved.ttf",
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
  run.alignment = kArTextHorizontalAlignment_Trailing;
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

int main(void) {
  TestAbiValidation();
  TestCacheHitsMissesAndFailureAtomicity();
  TestTransientFailureRecovers();
  TestByteBudgetAndOversizeRejection();
  TestOwnershipAcrossStatsResetAndEviction();
  TestFramePinsSurviveEntryPressure();
  TestPixelationTreatments();
  TestInkFormatsAndBounds();
  TestInterfaceTextCacheAndLifecycle();
  if (g_failures) {
    fprintf(stderr, "%d text-surface cache test(s) failed\n", g_failures);
    return 1;
  }
  puts("text-surface cache tests passed");
  return 0;
}
