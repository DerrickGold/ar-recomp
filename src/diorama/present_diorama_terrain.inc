typedef struct PresentTerrainRequest {
  ArRenderDevice *device;
  const DioramaTerrainSource *source;
  const DioramaCapture *primary;
  const ActionVoxelGeometry *geometry;
  ArRenderRectI world;
  unsigned room;
} PresentTerrainRequest;

static DioramaVoxelTerrainCapture s_terrain_view;
/* Distinct native animation snapshots can split into the exact same resident
 * textures. Keep those identities while the upload mirrors prove residency. */
static const DioramaTerrainSource *s_resident_terrain[4];
static unsigned s_resident_terrain_next;
static PresentationUploadMirror s_terrain_mirrors[3];
static uint32_t *s_terrain_split;
static size_t s_terrain_split_capacity;
static uint32_t *s_terrain_split_previous;
static size_t s_terrain_split_previous_capacity;
static const DioramaTerrainSource *s_terrain_split_source;
static unsigned s_terrain_split_binary_alpha;

static void ClearResidentTerrainSources(void) {
  for (unsigned i = 0; i < sizeof(s_resident_terrain) / sizeof(*s_resident_terrain); ++i) {
    DioramaTerrainSource_Release(s_resident_terrain[i]);
    s_resident_terrain[i] = NULL;
  }
  s_resident_terrain_next = 0;
}

static void ResetLiveTerrain(ArRenderDevice *device) {
  for (unsigned i = 0; i < 3; ++i) {
    ArRenderDevice_DestroyTexture(device, s_terrain_view.textures[i]);
    PresentationUploadMirror_Reset(&s_terrain_mirrors[i]);
  }
  s_terrain_view = (DioramaVoxelTerrainCapture){0};
  ClearResidentTerrainSources();
  free(s_terrain_split);
  s_terrain_split = NULL;
  s_terrain_split_capacity = 0;
  free(s_terrain_split_previous);
  s_terrain_split_previous = NULL;
  s_terrain_split_previous_capacity = 0;
  DioramaTerrainSource_Release(s_terrain_split_source);
  s_terrain_split_source = NULL;
}

/* Reuse split pixels only for the intersection the native capture certifies
 * as unchanged. Keep the existing contiguous atlas and its UV contract. */
static bool SplitLiveTerrain(const PresentTerrainRequest *request, uint32_t *split,
                             unsigned *binary_alpha) {
  const DioramaTerrainSource *source = request->source, *previous = s_terrain_split_source;
  const ArRenderRectI bounds = source->bounds;
  const size_t count = (size_t)bounds.w * bounds.h;
  const ActionRoomSceneFrameState colors = {.brightness = source->brightness,
      .cgwsel = source->cgwsel, .cgadsub = source->cgadsub, .fixed_color = source->fixed_color};
  const bool reuse = previous && source->overlap_serial && source->overlap_serial == previous->serial &&
      source->geometry_revision == previous->geometry_revision &&
      source->brightness == previous->brightness &&
      source->cgwsel == previous->cgwsel && source->cgadsub == previous->cgadsub &&
      source->fixed_color == previous->fixed_color;
  const uint32_t *pixels[] = {split, source->pixels + count, split + count};
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
                s_terrain_split_previous + lane * old_count + old_at +
                    (size_t)row * previous->bounds.w, (size_t)w * sizeof(uint32_t));
      } else if (!ActionVoxel_Split(request->geometry, wx, wy, w, h,
          bounds.w, &colors, source->pixels + at, bounds.w, split + at, split + count + at)) {
        return false;
      }
      for (unsigned band = 0; band < 3; ++band) {
        const unsigned bit = 1u << band;
        if (!(alpha & bit) || (cached && (s_terrain_split_binary_alpha & bit))) continue;
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
  if (DioramaPerformance_Enabled())
    fprintf(stderr, "[voxel-live-terrain] split pixels=%zu reused=%zu\n", count - reused_pixels, reused_pixels);
  *binary_alpha = alpha;
  return true;
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
    if (DioramaPerformance_Enabled() && ++hits % 120 == 0)
      fprintf(stderr, "[voxel-live-terrain] texture hits=%u uploads=%u\n", hits, uploads);
    *out = s_terrain_view;
    out->visible = need;
    return true;
  }
  const ArRenderRectI bounds = source->bounds;
  const size_t count = (size_t)bounds.w * bounds.h;
  if (s_terrain_view.bounds.w != bounds.w || s_terrain_view.bounds.h != bounds.h) {
    for (unsigned i = 0; i < 3; ++i) {
      ArRenderDevice_DestroyTexture(request->device, s_terrain_view.textures[i]);
      s_terrain_view.textures[i] = ArRenderTexture_Invalid();
      PresentationUploadMirror_Reset(&s_terrain_mirrors[i]);
    }
    ClearResidentTerrainSources();
  }
  if (s_terrain_split_capacity < count) {
    uint32_t *split = realloc(s_terrain_split, count * 2 * sizeof(*split));
    if (!split) return false;
    s_terrain_split = split;
    s_terrain_split_capacity = count;
  }
  uint32_t *split = s_terrain_split;
  const DioramaPerformanceScope scope = DioramaPerformance_Begin(kDioramaPerformance_Upload);
  unsigned binary_alpha_mask;
  bool ok = SplitLiveTerrain(request, split, &binary_alpha_mask);
  const uint32_t *pixels[] = {split, source->pixels + count, split + count};
  const ArRenderTextureDesc desc = {.width = bounds.w, .height = bounds.h,
      .format = kArRenderPixelFormat_Argb8888, .usage = kArRenderTextureUsage_Streaming,
      .filter = kArRenderFilter_Nearest, .blend = kArRenderBlendMode_Alpha};
  for (unsigned i = 0; ok && i < 3; ++i)
    if (!ArRenderTexture_IsValid(s_terrain_view.textures[i]))
      ok = ArRenderDevice_CreateTexture(request->device, &desc, &s_terrain_view.textures[i]);
  uint64_t bytes = 0;
  if (ok && ArRenderDevice_BeginTextureUploads(request->device)) {
    for (unsigned i = 0; ok && i < 3; ++i) {
      PresentationUploadResult update;
      ok = PresentationUploadMirror_UploadArgb8888(&s_terrain_mirrors[i], request->device,
          s_terrain_view.textures[i], (const uint8_t *)pixels[i],
          bounds.w, bounds.h, bounds.w * sizeof(uint32_t), 0, 0, &update);
      DioramaPerformance_AddPlaneSync(ok, ok && update.changed, ok ? update.uploaded_bytes : 0);
      if (ok) bytes += update.uploaded_bytes;
    }
    ok = ArRenderDevice_EndTextureUploads(request->device, ok) && ok;
  } else {
    ok = false;
  }
  if (!ok) {
    /* A failed batch can leave partial pixels. Drop all three mirrors and
     * textures so later frames retry cleanly and this frame uses native scanout. */
    ResetLiveTerrain(request->device);
    DioramaPerformance_End(scope);
    return false;
  }
  s_terrain_split = s_terrain_split_previous;
  s_terrain_split_previous = split;
  const size_t capacity = s_terrain_split_capacity;
  s_terrain_split_capacity = s_terrain_split_previous_capacity;
  s_terrain_split_previous_capacity = capacity;
  DioramaTerrainSource_Release(s_terrain_split_source);
  s_terrain_split_source = DioramaTerrainSource_Retain(source);
  s_terrain_split_binary_alpha = binary_alpha_mask;
  if (bytes || s_terrain_view.bounds.x != bounds.x || s_terrain_view.bounds.y != bounds.y)
    ClearResidentTerrainSources();
  s_terrain_view.bounds = bounds;
  s_terrain_view.binary_alpha_mask = binary_alpha_mask;
  DioramaTerrainSource_Release(s_resident_terrain[s_resident_terrain_next]);
  s_resident_terrain[s_resident_terrain_next] = DioramaTerrainSource_Retain(source);
  s_resident_terrain_next = (s_resident_terrain_next + 1) %
      (sizeof(s_resident_terrain) / sizeof(*s_resident_terrain));
  if (DioramaPerformance_Enabled())
    fprintf(stderr, "[voxel-live-terrain] texture uploads=%u hits=%u bytes=%llu\n",
        ++uploads, hits, (unsigned long long)bytes);
  DioramaPerformance_End(scope);
  *out = s_terrain_view;
  out->visible = need;
  return true;
}
