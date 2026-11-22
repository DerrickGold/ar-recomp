#include "app/runtime_log.h"

typedef struct PresentTerrainRequest {
  ArRenderDevice *device;
  const DioramaTerrainSource *source;
  const DioramaCapture *primary;
  const ActionVoxelGeometry *geometry;
  ArRenderRectI world;
  unsigned room;
} PresentTerrainRequest;

static DioramaVoxelTerrainCapture s_terrain_view;
static DioramaTerrainSpare s_terrain_spare;
static bool s_terrain_transfer_failed;
/* Distinct animation snapshots can produce identical resident textures. Keep
 * the aliases until the texture contents or capture bounds change. */
static const DioramaTerrainSource *s_resident_terrain[4];
static unsigned s_resident_terrain_next;
static PresentationUploadMirror s_terrain_mirrors[3];

typedef struct TerrainSplitBuffer {
  uint32_t *pixels;
  size_t capacity;
} TerrainSplitBuffer;

typedef struct TerrainSplitState {
  TerrainSplitBuffer buffer;
  const DioramaTerrainSource *source;
  unsigned binary_alpha;
} TerrainSplitState;

static TerrainSplitBuffer s_terrain_scratch;
static TerrainSplitState s_terrain_split;

static void ClearResidentTerrainSources(void) {
  for (unsigned i = 0; i < sizeof(s_resident_terrain) / sizeof(*s_resident_terrain); ++i) {
    DioramaTerrainSource_Release(s_resident_terrain[i]);
    s_resident_terrain[i] = NULL;
  }
  s_resident_terrain_next = 0;
}

static void ResetLiveTerrainTextures(ArRenderDevice *device) {
  for (unsigned i = 0; i < 3; ++i) {
    ArRenderDevice_DestroyTexture(device, s_terrain_view.textures[i]);
    PresentationUploadMirror_Reset(&s_terrain_mirrors[i]);
  }
  s_terrain_view = (DioramaVoxelTerrainCapture){0};
  DioramaTerrainSpare_Reset(device, &s_terrain_spare);
  ClearResidentTerrainSources();
}

static void ResetLiveTerrain(ArRenderDevice *device) {
  ResetLiveTerrainTextures(device);
  free(s_terrain_scratch.pixels);
  free(s_terrain_split.buffer.pixels);
  DioramaTerrainSource_Release(s_terrain_split.source);
  s_terrain_scratch = (TerrainSplitBuffer){0};
  s_terrain_split = (TerrainSplitState){0};
}

static bool TerrainSplitOverlap(const DioramaTerrainSource *source, const DioramaTerrainSource *previous) {
  return previous && source->overlap_serial && source->overlap_serial == previous->serial &&
      source->geometry_revision == previous->geometry_revision &&
      source->brightness == previous->brightness && source->cgwsel == previous->cgwsel &&
      source->cgadsub == previous->cgadsub && source->fixed_color == previous->fixed_color;
}

static void TerrainSplitBands(const DioramaTerrainSource *source, const uint32_t *split,
    const uint32_t *bands[3]) {
  const size_t count = (size_t)source->bounds.w * source->bounds.h;
  bands[0] = split;
  bands[1] = source->pixels + count;
  bands[2] = split + count;
}

/* Reuse split pixels only where the native capture certifies the overlap. */
static bool SplitLiveTerrain(const PresentTerrainRequest *request, uint32_t *split,
    const uint32_t *pixels[3], unsigned *binary_alpha) {
  const DioramaTerrainSource *source = request->source, *previous = s_terrain_split.source;
  const ArRenderRectI bounds = source->bounds;
  const size_t count = (size_t)bounds.w * bounds.h;
  const ActionRoomSceneFrameState colors = {.brightness = source->brightness,
      .cgwsel = source->cgwsel, .cgadsub = source->cgadsub, .fixed_color = source->fixed_color};
  const bool reuse = TerrainSplitOverlap(source, previous);
  TerrainSplitBands(source, split, pixels);
  if (!reuse) {
    if (!ActionVoxel_Split(request->geometry, bounds.x, bounds.y, bounds.w,
        bounds.h, bounds.w, &colors, source->pixels, bounds.w, split, split + count)) return false;
    *binary_alpha = 0;
    for (unsigned band = 0; band < 3; ++band) {
      size_t i = 0;
      for (; i < count; ++i) {
        const unsigned a = pixels[band][i] >> 24;
        if (a && a != 255) break;
      }
      if (i == count) *binary_alpha |= 1u << band;
    }
    return true;
  }
  unsigned alpha = 7;
  size_t reused_pixels = 0;
  enum { kSplitChunk = 32 };
  for (int y = 0; y < bounds.h;) {
    const int wy = bounds.y + y;
    int h = kSplitChunk - ((unsigned)wy & (kSplitChunk - 1));
    if (h > bounds.h - y) h = bounds.h - y;
    for (int x = 0; x < bounds.w;) {
      const int wx = bounds.x + x;
      int w = kSplitChunk - ((unsigned)wx & (kSplitChunk - 1));
      if (w > bounds.w - x) w = bounds.w - x;
      const size_t at = (size_t)y * bounds.w + x;
      const bool cached = wx >= previous->bounds.x && wy >= previous->bounds.y &&
          wx + w <= previous->bounds.x + previous->bounds.w &&
          wy + h <= previous->bounds.y + previous->bounds.h;
      if (cached) {
        reused_pixels += (size_t)w * h;
        const size_t old_count = (size_t)previous->bounds.w * previous->bounds.h;
        const size_t old_at = (size_t)(wy - previous->bounds.y) * previous->bounds.w +
            wx - previous->bounds.x;
        for (unsigned lane = 0; lane < 2; ++lane)
          for (int row = 0; row < h; ++row)
            memcpy(split + lane * count + at + (size_t)row * bounds.w,
                s_terrain_split.buffer.pixels + lane * old_count + old_at +
                    (size_t)row * previous->bounds.w, (size_t)w * sizeof(uint32_t));
      } else if (!ActionVoxel_Split(request->geometry, wx, wy, w, h,
          bounds.w, &colors, source->pixels + at, bounds.w, split + at, split + count + at)) {
        return false;
      }
      for (unsigned band = 0; band < 3; ++band) {
        const unsigned bit = 1u << band;
        if (!(alpha & bit) || (cached && (s_terrain_split.binary_alpha & bit))) continue;
        for (int row = 0; row < h && (alpha & bit); ++row) {
          const uint32_t *pixel = pixels[band] + at + (size_t)row * bounds.w;
          for (int column = 0; column < w; ++column) {
            const unsigned a = pixel[column] >> 24;
            if (a && a != 255) { alpha &= ~bit; break; }
          }
        }
      }
      x += w;
    }
    y += h;
  }
  if (DioramaPerformance_LoggingEnabled())
    RuntimeLog_Printf("[voxel-live-terrain] split pixels=%zu reused=%zu\n", count - reused_pixels, reused_pixels);
  *binary_alpha = alpha;
  return true;
}

static bool UploadLiveTerrain(ArRenderDevice *device, const DioramaTerrainSource *source,
    const uint32_t *const pixels[3], uint64_t *bytes, bool *transferred) {
  const ArRenderRectI bounds = source->bounds;
  const char *setting = getenv("AR_VOXEL_TERRAIN_TRANSFER");
  const bool try_transfer = !s_terrain_transfer_failed && (!setting || strcmp(setting, "0") != 0) &&
      ArRenderCapabilities_Has(ArRenderDevice_Capabilities(device),
          kArRenderCapability_TextureRegionTransfer);
  if (try_transfer) {
    const DioramaTerrainSource *previous = s_terrain_split.source;
    const ArRenderRectI old = previous ? s_terrain_view.bounds : (ArRenderRectI){0};
    const uint32_t *old_pixels[3] = {0};
    if (previous) TerrainSplitBands(previous, s_terrain_split.buffer.pixels, old_pixels);
    *transferred = DioramaTransferTerrainWindow(device, &s_terrain_spare, s_terrain_view.textures,
        bounds, old, TerrainSplitOverlap(source, previous), pixels, old_pixels, bytes, NULL);
    DioramaPerformance_AddPlaneSync(*transferred, *bytes != 0, *transferred ? *bytes : 0);
    if (*transferred) return true;
    s_terrain_transfer_failed = true;
    ResetLiveTerrainTextures(device);
    *bytes = 0;
    RuntimeLog_Printf("[voxel-live-terrain] texture transfer failed; restoring ordinary uploads\n");
  } else if (s_terrain_view.bounds.w != bounds.w || s_terrain_view.bounds.h != bounds.h) {
    ResetLiveTerrainTextures(device);
  }

  if (!DioramaTerrainTextures(device, s_terrain_view.textures, bounds.w, bounds.h) ||
      !ArRenderDevice_BeginTextureUploads(device)) return false;
  bool ok = true;
  for (unsigned band = 0; ok && band < 3; ++band) {
    PresentationUploadResult update;
    ok = PresentationUploadMirror_UploadArgb8888(&s_terrain_mirrors[band], device,
        s_terrain_view.textures[band], (const uint8_t *)pixels[band],
        bounds.w, bounds.h, bounds.w * sizeof(uint32_t), 0, 0, &update);
    DioramaPerformance_AddPlaneSync(ok, ok && update.changed, ok ? update.uploaded_bytes : 0);
    if (ok) *bytes += update.uploaded_bytes;
  }
  return ArRenderDevice_EndTextureUploads(device, ok) && ok;
}

static bool CaptureLiveTerrain(void *userdata, const DioramaProjection *projection,
    DioramaVoxelTerrainCapture *out) {
  const PresentTerrainRequest *request = userdata;
  const DioramaTerrainSource *source = request->source;
  const DioramaCapture *primary = request->primary;
  const ArRenderRectI native = {
    primary->bg1_camera_x - (primary->width - 256) / 2,
    primary->camera_y - primary->authentic_y0 + 1, primary->width, primary->height,
  };
  const int apron = primary->bg_apron_mask & 1 ? primary->obj_apron : 0;
  const float depth = ActionVoxel_Depth(request->geometry);
  const float relief = ActionVoxel_HasRearRelief(request->geometry) ? kActionVoxelMaxRearRelief : 0;
  const float far_depth = fmaxf(ActionVoxel_SceneryRecess(request->geometry),
      fmaxf(depth + relief, ActionVoxel_BranchBackDepth(request->geometry)));
  ArRenderRectI need;
  if (!Diorama_ResolveVoxelFootprint(projection, native, request->world,
      projection->bg1_plane.capture_offset, -.125f * depth, far_depth, &need) || !need.w || !need.h) {
    DioramaTerrain_Request(request->room, (DioramaTerrainMargins){0});
    return false;
  }
  const bool primary_covers = need.x >= native.x - apron &&
      need.x + need.w <= native.x + native.w + apron &&
      need.y >= native.y && need.y + need.h <= native.y + native.h;
  const DioramaTerrainMargins margins = primary_covers ? (DioramaTerrainMargins){0} :
      (DioramaTerrainMargins){
        fmin(kDioramaVoxelTerrainMaximum, fmax(0, native.x - need.x)),
        fmin(kDioramaVoxelTerrainMaximum, fmax(0, need.x + need.w - native.x - native.w)),
        fmin(kDioramaVoxelTerrainMaximum, fmax(0, native.y - need.y)),
        fmin(kDioramaVoxelTerrainMaximum, fmax(0, need.y + need.h - native.y - native.h)),
      };
  DioramaTerrain_Request(request->room, margins);
  if (primary_covers || !source ||
      source->geometry_revision != ActionVoxel_Revision(request->geometry)) return false;
  /* Render the portion available while the producer grows or moves its
   * cache. The world-space footprint itself stays independent of that cache. */
  const int left = fmax(need.x, source->bounds.x), top = fmax(need.y, source->bounds.y);
  const int right = fmin(need.x + need.w, source->bounds.x + source->bounds.w);
  const int bottom = fmin(need.y + need.h, source->bounds.y + source->bounds.h);
  if (left >= right || top >= bottom) return false;
  need = (ArRenderRectI){left, top, right - left, bottom - top};
  static unsigned hits, uploads;
  for (unsigned i = 0; i < sizeof(s_resident_terrain) / sizeof(*s_resident_terrain); ++i) {
    if (source != s_resident_terrain[i]) continue;
    if (DioramaPerformance_LoggingEnabled() && ++hits % 120 == 0)
      RuntimeLog_Printf("[voxel-live-terrain] texture hits=%u uploads=%u\n", hits, uploads);
    *out = s_terrain_view;
    out->visible = need;
    return true;
  }
  const ArRenderRectI bounds = source->bounds;
  const size_t count = (size_t)bounds.w * bounds.h;
  if (s_terrain_scratch.capacity < count) {
    uint32_t *split = realloc(s_terrain_scratch.pixels, count * 2 * sizeof(*split));
    if (!split) return false;
    s_terrain_scratch = (TerrainSplitBuffer){split, count};
  }
  const DioramaPerformanceScope scope = DioramaPerformance_Begin(kDioramaPerformance_Upload);
  unsigned binary_alpha_mask;
  const uint32_t *pixels[3];
  uint64_t bytes = 0;
  bool transferred = false;
  const bool ok = SplitLiveTerrain(request, s_terrain_scratch.pixels, pixels, &binary_alpha_mask) &&
      UploadLiveTerrain(request->device, source, pixels, &bytes, &transferred);
  if (!ok) {
    /* A failed batch can leave partial pixels. Drop all three mirrors and
     * textures so later frames retry cleanly and this frame uses native scanout. */
    ResetLiveTerrain(request->device);
    DioramaPerformance_End(scope);
    return false;
  }
  const TerrainSplitBuffer old_buffer = s_terrain_split.buffer;
  s_terrain_split.buffer = s_terrain_scratch;
  s_terrain_scratch = old_buffer;
  DioramaTerrainSource_Release(s_terrain_split.source);
  s_terrain_split.source = DioramaTerrainSource_Retain(source);
  s_terrain_split.binary_alpha = binary_alpha_mask;
  if (bytes || s_terrain_view.bounds.x != bounds.x || s_terrain_view.bounds.y != bounds.y ||
      s_terrain_view.bounds.w != bounds.w || s_terrain_view.bounds.h != bounds.h)
    ClearResidentTerrainSources();
  s_terrain_view.bounds = bounds;
  s_terrain_view.binary_alpha_mask = binary_alpha_mask;
  DioramaTerrainSource_Release(s_resident_terrain[s_resident_terrain_next]);
  s_resident_terrain[s_resident_terrain_next] = DioramaTerrainSource_Retain(source);
  s_resident_terrain_next = (s_resident_terrain_next + 1) %
      (sizeof(s_resident_terrain) / sizeof(*s_resident_terrain));
  if (DioramaPerformance_LoggingEnabled())
    RuntimeLog_Printf("[voxel-live-terrain] texture uploads=%u hits=%u bytes=%llu transfer=%d\n",
        ++uploads, hits, (unsigned long long)bytes, transferred);
  DioramaPerformance_End(scope);
  *out = s_terrain_view;
  out->visible = need;
  return true;
}
