#include "app/runtime_log.h"

/* The live capture is world-space and independent of the SDL camera thread.
 * Retain a bounded neighborhood so orbit and ordinary scroll reuse the same
 * native lanes, even when the renderer re-presents a retained game frame. */
static const DioramaTerrainSource *s_live_terrain;
static ArRenderRectI s_live_terrain_world;
/* Four snapshots cover the native castle cycle. Keep the budget at four
 * sources even when a room has continuously changing graphics or colors. */
typedef struct LiveTerrainCacheEntry {
  const DioramaTerrainSource *source;
  uint64_t last_use;
  uint32_t world_serial, capture_flags;
  uint16_t tile_base;
  uint8_t display, main_screen, sub_screen, windows[8];
  uint16_t characters[16384], palette[128];
} LiveTerrainCacheEntry;
static LiveTerrainCacheEntry s_terrain_cache[4];
static LiveTerrainCacheEntry *s_terrain_candidate;
static uint64_t s_terrain_use;
static ArRenderRectI s_terrain_strip;
static uint64_t s_terrain_serial;
static uint32_t *s_terrain_strip_pixels;
static size_t s_terrain_strip_capacity;
static atomic_uint_fast64_t s_terrain_request;

void DioramaTerrain_Request(unsigned room, DioramaTerrainMargins margins) {
  const uint64_t packed = ((uint64_t)(room & 65535) << 48) |
      ((uint64_t)(margins.bottom & 4095) << 36) |
      ((uint64_t)(margins.top & 4095) << 24) |
      ((uint64_t)(margins.right & 4095) << 12) | (margins.left & 4095);
  atomic_store_explicit(&s_terrain_request, packed, memory_order_relaxed);
}

DioramaTerrainMargins DioramaTerrain_Requested(unsigned room) {
  const uint64_t packed = atomic_load_explicit(&s_terrain_request, memory_order_relaxed);
  if (packed >> 48 != room) return (DioramaTerrainMargins){0};
  return (DioramaTerrainMargins){packed & 4095, (packed >> 12) & 4095,
      (packed >> 24) & 4095, (packed >> 36) & 4095};
}

const DioramaTerrainSource *ActRaiser_LiveDioramaTerrain(void) {
  return s_live_terrain;
}

ArRenderRectI ActRaiser_LiveDioramaTerrainWorld(void) {
  return s_live_terrain_world;
}

void ActRaiser_ResetDioramaTerrain(void) {
  DioramaTerrain_Request(0, (DioramaTerrainMargins){0});
  s_live_terrain = NULL;
  s_live_terrain_world = (ArRenderRectI){0};
  s_terrain_candidate = NULL;
  for (unsigned i = 0; i < sizeof(s_terrain_cache) / sizeof(*s_terrain_cache); ++i) {
    DioramaTerrainSource_Release(s_terrain_cache[i].source);
    s_terrain_cache[i].source = NULL;
  }
  s_terrain_use = 0;
  free(s_terrain_strip_pixels);
  s_terrain_strip_pixels = NULL;
  s_terrain_strip_capacity = 0;
}

/* Keep the cached overlap when the rectangle moves across one axis. Two
 * exposed edges can still use the full-rectangle capture. */
static ArRenderRectI TerrainUncoveredStrip(ArRenderRectI old, ArRenderRectI next) {
  const int right = next.x + next.w, bottom = next.y + next.h;
  const int old_right = old.x + old.w, old_bottom = old.y + old.h;
  if (old.y <= next.y && old_bottom >= bottom) {
    if (old.x <= next.x && old_right > next.x && old_right < right)
      return (ArRenderRectI){old_right, next.y, right - old_right, next.h};
    if (old.x > next.x && old.x < right && old_right >= right)
      return (ArRenderRectI){next.x, next.y, old.x - next.x, next.h};
  }
  if (old.x <= next.x && old_right >= right) {
    if (old.y <= next.y && old_bottom > next.y && old_bottom < bottom)
      return (ArRenderRectI){next.x, old_bottom, next.w, bottom - old_bottom};
    if (old.y > next.y && old.y < bottom && old_bottom >= bottom)
      return (ArRenderRectI){next.x, next.y, next.w, old.y - next.y};
  }
  return next;
}

static void CopyTerrainRegion(DioramaTerrainSource *dest, const uint32_t *pixels,
    ArRenderRectI source, ArRenderRectI region) {
  const ArRenderRectI target = dest->bounds;
  const size_t source_count = (size_t)source.w * source.h;
  const size_t target_count = (size_t)target.w * target.h;
  for (unsigned band = 0; band < 3; ++band) {
    const uint32_t *read = pixels + band * source_count +
        (size_t)(region.y - source.y) * source.w + (region.x - source.x);
    uint32_t *write = dest->pixels + band * target_count +
        (size_t)(region.y - target.y) * target.w + (region.x - target.x);
    for (int y = 0; y < region.h; ++y)
      memcpy(write + (size_t)y * target.w, read + (size_t)y * source.w,
          (size_t)region.w * sizeof(uint32_t));
  }
}

static DioramaTerrainSource *PrepareLiveTerrain(
    const SrPpuFrameTransactionContext *context, uint32_t world_serial,
    SrPpuBackgroundGuardRequest *request, SrPpuBackgroundGuardBands *bands) {
  static int enabled = -1;
  if (enabled < 0) {
    const char *value = getenv("AR_VOXEL_LIVE_TERRAIN");
    enabled = !value || strcmp(value, "0");
  }
  if (!enabled || g_settings.diorama_mode != kAction3D_Voxel)
    return NULL;
  const unsigned group = g_ram[kActRaiserWram_MapGroup], map = g_ram[kActRaiserWram_CurrentMap];
  const ActionVoxelGeometry *geometry = ActRaiserActionBg_VoxelGeometry(group, map);
  const ActionBgLayerPlan *layer = &ActRaiser_PendingActionBgPlan()->layer[0];
  const SrPpuOverlayCaptureState *capture = ActRaiser_PpuCapture(SR_PPU_OVERLAY_BG1);
  const SrPpuStateSnapshot *ppu = &context->state;
  const uint8_t windows[] = {ppu->main_windowed & 1, ppu->sub_windowed & 1,
      ppu->window_select & 15, ppu->window_logic & 3, ppu->window1_left, ppu->window1_right, ppu->window2_left, ppu->window2_right};
  const unsigned tile_base = ppu->backgrounds[0].tile_base_word;
  const unsigned guard_capture_flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME |
      SR_PPU_OVERLAY_MARK_BG_HALF_ADD | SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT;
  if (!world_serial || !geometry || !capture || !DioramaCameraPolicy_FiniteLayer(layer) ||
      (capture->flags & ~guard_capture_flags) || !((ppu->main_screen | ppu->sub_screen) & 1) ||
      ppu->backgrounds[0].tile_size_pixels != 8 ||
      !context->vram.data || context->vram.element_count < tile_base + 16384u ||
      !context->cgram.data || context->cgram.element_count < 128 ||
      ppu->bg_mode != 1 || ppu->display_control != 15 ||
      ((ppu->mosaic_control & 1) && (ppu->mosaic_control >> 4))) {
    static bool reported;
    if (!reported && ppu->brightness == 15 && DioramaPerformance_LoggingEnabled()) {
      RuntimeLog_Printf("[voxel-live-terrain] unavailable world=%u geometry=%d capture=%d finite=%d vram=%llu base=%u palette=%llu mode=%u display=%u windows=%u/%u mosaic=%u\n",
          world_serial, geometry != NULL, capture != NULL, DioramaCameraPolicy_FiniteLayer(layer),
          (unsigned long long)context->vram.element_count, tile_base,
          (unsigned long long)context->cgram.element_count, ppu->bg_mode, ppu->display_control,
          ppu->main_windowed, ppu->sub_windowed, ppu->mosaic_control);
      reported = true;
    }
    return NULL;
  }
  const DioramaRoomOverride *room = ActRaiser_CurrentVirtualLayerRoom();
  ArRenderRectI world;
  DioramaScenery_AxisExtent(room, 0, layer->world_width, false, &world.x, &world.w);
  DioramaScenery_AxisExtent(room, 0, layer->world_height, true, &world.y, &world.h);
  s_live_terrain_world = world;
  const DioramaTerrainMargins requested = DioramaTerrain_Requested((group << 8) | map);
  if (!(requested.left || requested.right || requested.top || requested.bottom)) return NULL;
  const int camera_x = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraX);
  const int camera_y = (int16_t)ActRaiser_ReadWram16(kActRaiserWram_Bg1CameraY);
  /* Keep native scroll headroom while rotation/zoom request extra coverage.
   * The inner extent triggers recentering ahead of the texture edge. */
  const int primary_x = camera_x - g_ws_extra;
  const int primary_y = camera_y - g_ws_extra_top + 1;
  const int primary_right = camera_x + 256 + g_ws_extra;
  const int primary_bottom = camera_y + 225 + g_ws_extra_bottom;
  int x0, y0, x1, y1, inner_x0, inner_y0, inner_x1, inner_y1;
  Diorama_ResolveVoxelTerrainAxis(world.x, world.w, primary_x, primary_right,
      fmax(192, requested.left + 64), fmax(192, requested.right + 64), &x0, &x1);
  Diorama_ResolveVoxelTerrainAxis(world.y, world.h, primary_y, primary_bottom,
      fmax(192, requested.top + 64), fmax(192, requested.bottom + 64), &y0, &y1);
  Diorama_ResolveVoxelTerrainAxis(world.x, world.w, primary_x, primary_right,
      fmax(128, requested.left), fmax(128, requested.right), &inner_x0, &inner_x1);
  Diorama_ResolveVoxelTerrainAxis(world.y, world.h, primary_y, primary_bottom,
      fmax(128, requested.top), fmax(128, requested.bottom), &inner_y0, &inner_y1);
  /* When either budget is full, use the same axis as the inner query.
   * Otherwise incompatible capped margins could force a rebuild every tick. */
  if (x1 - x0 == kDioramaVoxelTerrainMaximum) {
    inner_x0 = (int)fmax(inner_x0, x0 == world.x ? x0 : x0 + 32);
    inner_x1 = (int)fmin(inner_x1, x1 == world.x + world.w ? x1 : x1 - 32);
  }
  if (y1 - y0 == kDioramaVoxelTerrainMaximum) {
    inner_y0 = (int)fmax(inner_y0, y0 == world.y ? y0 : y0 + 32);
    inner_y1 = (int)fmin(inner_y1, y1 == world.y + world.h ? y1 : y1 - 32);
  }
  const ArRenderRectI bounds = {x0, y0, x1 - x0, y1 - y0};
  if (bounds.w <= 0 || bounds.h <= 0 || bounds.w > kDioramaVoxelTerrainMaximum ||
      bounds.h > kDioramaVoxelTerrainMaximum) return NULL;
  const uint64_t revision = ActionVoxel_Revision(geometry);
  LiveTerrainCacheEntry *replacement = &s_terrain_cache[0];
  const DioramaTerrainSource *overlap_source = NULL;
  ArRenderRectI sample = bounds;
  for (unsigned i = 0; i < sizeof(s_terrain_cache) / sizeof(*s_terrain_cache); ++i) {
    LiveTerrainCacheEntry *entry = &s_terrain_cache[i];
    const DioramaTerrainSource *cached = entry->source;
    if (!cached || (replacement->source && entry->last_use < replacement->last_use))
      replacement = entry;
    if (cached && cached->geometry_revision == revision &&
        cached->fixed_color == ppu->fixed_color && cached->cgwsel == ppu->color_math_control &&
        cached->cgadsub == ppu->color_math_designation &&
        entry->display == ppu->display_control &&
        entry->main_screen == ppu->main_screen &&
        entry->sub_screen == ppu->sub_screen &&
        !memcmp(entry->windows, windows, sizeof(windows)) &&
        entry->world_serial == world_serial &&
        entry->capture_flags == capture->flags && entry->tile_base == tile_base &&
        cached->bounds.x >= world.x && cached->bounds.y >= world.y &&
        cached->bounds.x + cached->bounds.w <= world.x + world.w &&
        cached->bounds.y + cached->bounds.h <= world.y + world.h &&
        !memcmp(entry->characters, context->vram.data + tile_base,
            sizeof(entry->characters)) &&
        !memcmp(entry->palette, context->cgram.data, sizeof(entry->palette))) {
      if (cached->bounds.x <= inner_x0 && cached->bounds.y <= inner_y0 &&
          cached->bounds.x + cached->bounds.w >= inner_x1 &&
          cached->bounds.y + cached->bounds.h >= inner_y1) {
        s_terrain_candidate = entry;
        s_live_terrain = cached;
        return NULL;
      }
      const ArRenderRectI strip = TerrainUncoveredStrip(cached->bounds, bounds);
      if ((size_t)strip.w * strip.h < (size_t)sample.w * sample.h) {
        overlap_source = cached;
        sample = strip;
      }
    }
  }
  const size_t count = (size_t)bounds.w * bounds.h;
  DioramaTerrainSource *source = malloc(sizeof(*source) + 3 * count * sizeof(uint32_t));
  if (!source) return NULL;
  atomic_init(&source->references, 1);
  source->bounds = bounds;
  source->geometry_revision = revision;
  source->fixed_color = ppu->fixed_color;
  source->brightness = ppu->brightness;
  source->cgwsel = ppu->color_math_control;
  source->cgadsub = ppu->color_math_designation;
  source->serial = ++s_terrain_serial;
  if (overlap_source && s_terrain_strip_capacity < (size_t)sample.w * sample.h) {
    const size_t capacity = (size_t)sample.w * sample.h;
    uint32_t *pixels = realloc(s_terrain_strip_pixels, 3 * capacity * sizeof(uint32_t));
    if (pixels) {
      s_terrain_strip_pixels = pixels;
      s_terrain_strip_capacity = capacity;
    } else {
      overlap_source = NULL;
      sample = bounds;
    }
  }
  source->overlap_serial = overlap_source ? overlap_source->serial : 0;
  if (overlap_source) {
    const ArRenderRectI old = overlap_source->bounds;
    const int left = fmax(old.x, x0), top = fmax(old.y, y0);
    const ArRenderRectI overlap = {left, top,
        (int)fmin(old.x + old.w, x1) - left, (int)fmin(old.y + old.h, y1) - top};
    CopyTerrainRegion(source, overlap_source->pixels, old, overlap);
  }
  s_terrain_strip = overlap_source ? sample : (ArRenderRectI){0};
  *request = (SrPpuBackgroundGuardRequest){.struct_size = sizeof(*request),
      .layer = 0, .screen_x0 = sample.x - camera_x, .screen_y0 = sample.y - camera_y - 1,
      .width = sample.w, .height = sample.h, .source_x0 = x0, .source_y0 = y0,
      .source_width = bounds.w, .source_height = bounds.h};
  *bands = (SrPpuBackgroundGuardBands){.struct_size = sizeof(*bands),
      .flags = SR_PPU_BACKGROUND_GUARD_BANDS_REQUIRE_ANCHOR_SCROLL |
               SR_PPU_BACKGROUND_GUARD_BANDS_WORLD_TERRAIN};
  const size_t sample_count = (size_t)sample.w * sample.h;
  uint32_t *pixels = overlap_source ? s_terrain_strip_pixels : source->pixels;
  for (unsigned i = 0; i < 3; ++i)
    bands->bands[i] = (SrPpuBackgroundGuardBandOutput){pixels + i * sample_count,
      (uint64_t)sample.w * sizeof(uint32_t), sample_count * sizeof(uint32_t)};
  /* Drop only the evicted cache reference. Queued and retained frames
   * continue to own their immutable pixels. Finish publishes this slot
   * after the runner checks the native capture. */
  DioramaTerrainSource_Release(replacement->source);
  replacement->source = NULL;
  s_terrain_candidate = replacement;
  replacement->world_serial = world_serial;
  replacement->capture_flags = capture->flags;
  replacement->tile_base = tile_base;
  replacement->display = ppu->display_control;
  replacement->main_screen = ppu->main_screen;
  replacement->sub_screen = ppu->sub_screen;
  memcpy(replacement->windows, windows, sizeof(windows));
  memcpy(replacement->characters, context->vram.data + tile_base,
      sizeof(replacement->characters));
  memcpy(replacement->palette, context->cgram.data, sizeof(replacement->palette));
  return source;
}

static void FinishLiveTerrain(const SrPpuFrameTransactionContext *context,
    DioramaTerrainSource *pending, SrResult status, const SrPpuScanoutResult *result) {
  const DioramaTerrainSource *source = pending ? pending : s_live_terrain;
  if (!source) return;
  LiveTerrainCacheEntry *entry = s_terrain_candidate;
  s_terrain_candidate = NULL;
  const SrPpuStateSnapshot *before = &context->state, *after = &result->final_state;
  if (status != SR_RESULT_OK ||
      (pending && !(result->flags & SR_PPU_SCANOUT_WORLD_TERRAIN_READY)) ||
      before->backgrounds[0].h_scroll != after->backgrounds[0].h_scroll ||
      before->backgrounds[0].v_scroll != after->backgrounds[0].v_scroll ||
      before->backgrounds[0].tile_base_word != after->backgrounds[0].tile_base_word ||
      before->display_control != after->display_control ||
      before->main_screen != after->main_screen || before->sub_screen != after->sub_screen ||
      (((before->main_windowed ^ after->main_windowed) |
        (before->sub_windowed ^ after->sub_windowed)) & 1) ||
      ((before->window_select ^ after->window_select) & 15) ||
      ((before->window_logic ^ after->window_logic) & 3) ||
      before->window1_left != after->window1_left || before->window1_right != after->window1_right ||
      before->window2_left != after->window2_left || before->window2_right != after->window2_right ||
      ((after->mosaic_control & 1) && (after->mosaic_control >> 4)) ||
      after->color_math_control != source->cgwsel ||
      after->color_math_designation != source->cgadsub || after->fixed_color != source->fixed_color ||
      memcmp(entry->characters, context->vram.data + entry->tile_base,
          sizeof(entry->characters)) ||
      memcmp(entry->palette, context->cgram.data, sizeof(entry->palette))) {
    static bool reported;
    if (!reported && source->brightness == 15 && DioramaPerformance_LoggingEnabled()) {
      RuntimeLog_Printf("[voxel-live-terrain] rejected scanout status=%u flags=%x scroll=%u,%u to %u,%u windows=%u/%u\n",
          status, result->flags, before->backgrounds[0].h_scroll, before->backgrounds[0].v_scroll,
          after->backgrounds[0].h_scroll, after->backgrounds[0].v_scroll,
          after->main_windowed, after->sub_windowed);
      reported = true;
    }
    DioramaTerrainSource_Release(pending);
    s_live_terrain = NULL;
    return;
  }
  entry->last_use = ++s_terrain_use;
  if (pending) {
    const bool partial = s_terrain_strip.w > 0;
    if (partial)
      CopyTerrainRegion(pending, s_terrain_strip_pixels, s_terrain_strip, s_terrain_strip);
    if (DioramaPerformance_LoggingEnabled())
      RuntimeLog_Printf("[voxel-live-terrain] capture map=%02x/%02x source=%d,%d %dx%d bytes=%zu partial=%d\n",
          g_ram[kActRaiserWram_MapGroup], g_ram[kActRaiserWram_CurrentMap],
          source->bounds.x, source->bounds.y, source->bounds.w, source->bounds.h,
          3 * (partial ? (size_t)s_terrain_strip.w * s_terrain_strip.h :
              (size_t)source->bounds.w * source->bounds.h) * sizeof(uint32_t), partial);
    /* Native palette or character animation can produce identical BG1
     * pixels. Share the immutable snapshot across those cache keys so the
     * presenter can also reuse its split and textures. */
    const ArRenderRectI bounds = pending->bounds;
    const size_t bytes = 3 * (size_t)bounds.w * bounds.h * sizeof(uint32_t);
    for (unsigned i = 0; i < sizeof(s_terrain_cache) / sizeof(*s_terrain_cache); ++i) {
      const DioramaTerrainSource *cached = s_terrain_cache[i].source;
      if (cached && cached->geometry_revision == pending->geometry_revision &&
          cached->bounds.x == bounds.x && cached->bounds.y == bounds.y &&
          cached->bounds.w == bounds.w && cached->bounds.h == bounds.h &&
          cached->brightness == pending->brightness &&
          cached->fixed_color == pending->fixed_color &&
          cached->cgwsel == pending->cgwsel && cached->cgadsub == pending->cgadsub &&
          !memcmp(cached->pixels, pending->pixels, bytes)) {
        source = DioramaTerrainSource_Retain(cached);
        DioramaTerrainSource_Release(pending);
        break;
      }
    }
    entry->source = source;
    s_live_terrain = source;
  }
}
