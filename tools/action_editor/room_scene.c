/* Whole-room adapter. Pixel sampling, priority splitting, edit application and
 * margin scanout run through the native PPU, never a browser approximation. */
#include "room_scene.h"
#include "action/action_bg_world.h"
#include "actraiser/actraiser_room_profiles.h"
#include "diorama/diorama_capture_blend.h"
#include "diorama/diorama_scene_extent.h"
#include "deterministic_hash.h"
#include <stdlib.h>
#include <string.h>

typedef struct RoomLayer {
  ActionBgWorld *world;
  const DioramaRoomOverride *edits;
  ActionBgLayerPlan plan;
  unsigned bg, page;
  bool page_cycle;
  uint8_t *bands;
} RoomLayer;
struct EditorRoomScene {
  ActionSceneSnapshot assets;
  SrSceneRenderer *renderer;
  RoomLayer layers[2];
  DioramaLayerOrderTable *table;
  DioramaRoomOverride terrain;
  ActionRoomSceneFrameState frame;
  SrSceneSurfaces surfaces;
  SrSceneRowPolicy policies[2][224];
  uint16_t vram[32768], palette[256];
  DioramaCapture capture;
  DioramaScene scene;
  DioramaRenderOptions options;
  DioramaBgValidSpanPlan spans;
  DioramaSkyboxView skybox;
  const uint8_t *pixels[kDioramaPlane_Count];
  bool fill_configured[2];
  uint32_t fill_argb[2];
};

static int Wrap(int x, unsigned size) {
  int v = x % (int)size;
  return v < 0 ? v + (int)size : v;
}
static void Coordinates(const RoomLayer *l, int *x, int *y) {
  if (l->page_cycle) {
    *x = Wrap(*x, 32) + (l->page & 1) * 32;
    *y = Wrap(*y, 32) + (l->page >> 1) * 32;
  } else if (l->plan.source != kActionBgSource_WorldMap) {
    *x = Wrap(*x, ActionBgWorld_TileWidth(l->world));
    *y = Wrap(*y, ActionBgWorld_TileHeight(l->world));
  } else if (l->plan.wrap_world_x) {
    *x = Wrap(*x, ActionBgWorld_TileWidth(l->world));
  }
}
static uint32_t Lookup(void *context, int32_t x, int32_t y, uint16_t *entry) {
  RoomLayer *l = context;
  Coordinates(l, &x, &y);
  return ActionBgWorld_Lookup(l->world, x, y, entry) == kActionBgLookup_Tile;
}
static uint32_t Span(void *context, int32_t x, int32_t y, int32_t step,
                      uint32_t capacity, const uint16_t **entries, int64_t *stride) {
  RoomLayer *l = context;
  Coordinates(l, &x, &y);
  unsigned width = l->page_cycle ? 32 : ActionBgWorld_TileWidth(l->world);
  if (l->page_cycle || l->plan.source != kActionBgSource_WorldMap || l->plan.wrap_world_x) {
    unsigned n = step > 0 ? width - (unsigned)x % width : (unsigned)x % width + 1;
    if (capacity > n) capacity = n;
  }
  ptrdiff_t words = 0;
  size_t count = ActionBgWorld_LookupSpan(l->world, x, y, step, capacity, entries, &words);
  *stride = words;
  return (uint32_t)count;
}
static uint32_t Band(void *context, int32_t x, int32_t y, uint16_t entry, uint8_t *band) {
  RoomLayer *l = context;
  Coordinates(l, &x, &y);
  const unsigned w = ActionBgWorld_TileWidth(l->world), h = ActionBgWorld_TileHeight(l->world);
  if (x < 0 || y < 0 || (unsigned)x >= w || (unsigned)y >= h) return 0;
  *band = l->bands ? l->bands[(size_t)y * w + x] : (entry & 0x2000) ? 2 : 1;
  return 1;
}
static uint32_t Edit(void *context, int32_t x, int32_t y, SrPpuCaptureTile *tile) {
  RoomLayer *l = context;
  const DioramaRoomOverride *r = l->edits;
  int cx = x >= 0 ? x / 2 : (x - 1) / 2, cy = y >= 0 ? y / 2 : (y - 1) / 2;
  const DioramaTileStamp *stamp = DioramaLayerOrder_StampAt(&r->stamp_layers[l->bg], cx, cy);
  int metatile = -1;
  *tile = (SrPpuCaptureTile){0};
  if (stamp) {
    unsigned q = (y & 1) * 2 + (x & 1);
    tile->entry = stamp->words[q];
    tile->band = (stamp->bands >> (q * 2)) & 3;
    tile->flags = SR_PPU_CAPTURE_TILE_REPLACE | (stamp->blank ? SR_PPU_CAPTURE_TILE_BLANK : 0);
    if (!stamp->blank) metatile = stamp->metatile;
  } else {
    uint8_t id;
    Coordinates(l, &x, &y);
    if (!r->pixel_layers[l->bg].count ||
        ActionBgWorld_Lookup(l->world, x, y, &tile->entry) != kActionBgLookup_Tile ||
        !ActionBgWorld_LookupMetatile(l->world, x, y, &id)) return 0;
    cx = x / 2; cy = y / 2; metatile = id;
    Band(l, x, y, tile->entry, &tile->band);
  }
  const DioramaPixelEdit *edit = DioramaLayerOrder_PixelEditAt(r, l->bg, cx, cy, metatile);
  if (edit) for (unsigned row = 0; row < 8; ++row) {
    unsigned at = (y & 1) * 8 + row, shift = (x & 1) ? 0 : 8;
    tile->black_rows[row] = (uint8_t)(edit->black[at] >> shift);
    tile->transparent_rows[row] = (uint8_t)(edit->transparent[at] >> shift);
  }
  return tile->band < 3 && (stamp || edit);
}
EditorRoomScene *EditorRoomScene_Create(const ActionSceneSnapshot *assets) {
  if (!assets) return NULL;
  EditorRoomScene *r = calloc(1, sizeof(*r));
  if (!r) return NULL;
  r->assets = *assets;
  r->renderer = sr_scene_renderer_create();
  if (!r->renderer) { EditorRoomScene_Destroy(r); return NULL; }
  for (unsigned bg = 0; bg < 2; ++bg) {
    const ActionRoomSceneBg *b = &assets->scene.bg[bg];
    r->layers[bg] = (RoomLayer){.world = ActionBgWorld_Create(), .bg = bg, .edits = &r->terrain};
    const ActionBgImmutableInput input = {
      .map = b->map, .map_size = b->map_size, .metatiles = b->metatiles,
      .metatile_size = sizeof(b->metatiles), .world_width = b->pages_wide * 256,
      .world_height = b->pages_high * 256, .word_mask = kActionRoomSceneTileWordMask,
      .attributes = (uint8_t)(ActionRoomScene_BgAttributes(&assets->scene, bg + 1) >> 8),
      .metatile_words_big_endian = true,
    };
    if (!r->layers[bg].world || !ActionBgWorld_UpdateImmutable(r->layers[bg].world, &input)) {
      EditorRoomScene_Destroy(r); return NULL;
    }
  }
  r->options = (DioramaRenderOptions){.visible_planes = UINT32_MAX,
    .skybox = kDioramaSky_Only, .margin_fix = true, .depth_shade = 0.25f,
    .edge_aa = true, .depth_of_field = true, .rim_light = true,
    .stack_grouping = true, .skybox_prefilter = true, .priority_surface = true};
  r->scene = (DioramaScene){.render = &r->options,
    .map_group = assets->scene.group, .map_number = assets->scene.map};
  return r;
}
void EditorRoomScene_Destroy(EditorRoomScene *r) {
  if (!r) return;
  for (unsigned bg = 0; bg < 2; ++bg) {
    ActionBgWorld_Destroy(r->layers[bg].world); free(r->layers[bg].bands);
  }
  sr_scene_renderer_destroy(r->renderer);
  if (r->table) { DioramaLayerOrder_ClearTable(r->table); free(r->table); }
  DioramaLayerOrder_ClearRoom(&r->terrain);
  free(r);
}
bool EditorRoomScene_Configure(EditorRoomScene *r, const char *text) {
  if (!r || !text) return false;
  DioramaLayerOrderTable *table = calloc(1, sizeof(*table));
  DioramaRoomOverride *terrain = calloc(1, sizeof(*terrain));
  if (!table || !terrain) { free(table); free(terrain); return false; }
  DioramaRoomOverride *current = NULL;
  bool ok = true;
  for (const char *at = text; *at && ok;) {
    char line[512];
    size_t n = strcspn(at, "\n");
    if (n >= sizeof(line)) { ok = false; break; }
    memcpy(line, at, n); line[n] = 0; at += n; if (*at) ++at;
    line[strcspn(line, ";#")] = 0;
    char *s = line; while (*s == ' ' || *s == '\t' || *s == '\r') ++s;
    if (!*s) continue;
    if (*s == '[') {
      char *end = strchr(s, ']');
      if (!end) { ok = false; break; }
      *end = 0;
      uint8_t g, m, section;
      current = NULL;
      if (DioramaLayerOrder_ParseScopedSection(s + 1, &g, &m, &section) &&
          g == r->assets.scene.group && m == r->assets.scene.map) {
        current = DioramaLayerOrder_FindOrAddSection(table, g, m, section);
        if (!current) ok = false;
      }
    } else if (current && !DioramaLayerOrder_ParseLine(current, s, NULL)) ok = false;
  }
  const DioramaRoomOverride *base = DioramaLayerOrder_Find(table, r->scene.map_group, r->scene.map_number);
  if (ok && base) ok = DioramaLayerOrder_ForTerrain(base, r->assets.terrain_profile, terrain);
  uint8_t *bands[2] = {0};
  for (unsigned bg = 0; ok && bg < 2; ++bg) {
    if (!DioramaLayerOrder_VirtualLayerHasClassification(&terrain->virtual_layers[bg])) continue;
    const ActionBgWorld *world = r->layers[bg].world;
    const unsigned w = ActionBgWorld_TileWidth(world), h = ActionBgWorld_TileHeight(world);
    bands[bg] = malloc((size_t)w * h);
    if (!bands[bg]) { ok = false; break; }
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
      uint16_t word = 0; uint8_t id = 0;
      ActionBgWorld_Lookup(world, x, y, &word);
      ActionBgWorld_LookupMetatile(world, x, y, &id);
      bands[bg][(size_t)y * w + x] = (uint8_t)DioramaLayerOrder_VirtualBand(terrain, bg, x / 2, y / 2, id, word);
    }
  }
  if (ok) {
    for (unsigned bg = 0; bg < 2; ++bg) { free(r->layers[bg].bands); r->layers[bg].bands = bands[bg]; }
    if (r->table) { DioramaLayerOrder_ClearTable(r->table); free(r->table); }
    DioramaLayerOrder_ClearRoom(&r->terrain);
    r->table = table; r->terrain = *terrain; r->options.layers = table;
  } else {
    free(bands[0]); free(bands[1]);
    DioramaLayerOrder_ClearTable(table); free(table);
    DioramaLayerOrder_ClearRoom(terrain);
  }
  free(terrain);
  return ok;
}
static int Min(int a, int b) { return a < b ? a : b; }
static int Max(int a, int b) { return a > b ? a : b; }
static uint16_t Extent(ActionBgExtentMode mode, uint16_t value) {
  return mode == kActionBgExtent_Fixed ? value : UINT16_MAX;
}
bool EditorRoomScene_Render(EditorRoomScene *r, int x, int y, uint32_t frame,
                            int extra_x, int budget) {
  if (!r || x < 0 || y < 0 || x > 65535 || y > 65535 || extra_x < 0 ||
      extra_x > 128 || budget < 0 || budget > 64) return false;
  ActionRoomSceneFrameRequest request = r->assets.frame;
  request.camera_x = x; request.camera_y = y; request.game_frame = frame;
  const ActionRoomScene *s = &r->assets.scene;
  ActionRoomSceneFrameState *f = &r->frame;
  if (!ActionRoomScene_BuildFrameState(s, &request, f)) return false;
  ActionBgFrameState state = {.map_group = s->group, .map_number = s->map,
      .decorative_padding_enabled = true};
  for (unsigned bg = 0; bg < 2; ++bg)
    state.layer[bg] = (ActionBgLayerState){
      .camera_x = (uint16_t)f->layer_camera_x[bg], .camera_y = (uint16_t)f->layer_camera_y[bg],
      .world_width = s->bg[bg].pages_wide * 256, .world_height = s->bg[bg].pages_high * 256,
      .bgsc = f->bgsc[bg], .tilemap_base = (bg ? 0x70 : 0x60) << 8};
  ActionBgPlan plan;
  if (!ActionBgPlan_Build(&state, &plan)) return false;
  const int playfield = ActionBgPlan_PlayfieldLayer(&plan);
  if (ActionBgPlan_PrimaryLayer(&plan) < 0) budget = 0;
  int world_x0, world_width, world_y0, world_height;
  DioramaScenery_AxisExtent(&r->terrain, 0, EditorRoomScene_Width(r), false, &world_x0, &world_width);
  DioramaScenery_AxisExtent(&r->terrain, 0, EditorRoomScene_Height(r), true, &world_y0, &world_height);
  const int room_y = y - world_y0;
  int top = Min(Max(0, room_y), budget), bottom = Min(Max(0, world_height - 225 - room_y), budget);
  int remaining = budget * 2 - top - bottom;
  if (bottom < budget) top += Min(remaining, Min(Max(0, room_y), budget * 2) - top);
  else if (top < budget) bottom += Min(remaining,
      Min(Max(0, world_height - 225 - room_y), budget * 2) - bottom);
  uint8_t chars[kActionRoomSceneCharacterBytes];
  if (!ActionRoomScene_BuildCharacters(s, frame, f->animation_phase, chars, sizeof(chars))) return false;
  for (unsigned i = 0; i < sizeof(chars) / 2; ++i)
    r->vram[i] = chars[i * 2] | ((uint16_t)chars[i * 2 + 1] << 8);
  for (unsigned i = 0; i < sizeof(s->extra_characters) / 2; ++i)
    r->vram[sizeof(chars) / 2 + i] = s->extra_characters[i * 2] |
        ((uint16_t)s->extra_characters[i * 2 + 1] << 8);
  for (unsigned i = 0; i < sizeof(s->palette) / 2; ++i)
    r->palette[i] = s->palette[i * 2] | ((uint16_t)s->palette[i * 2 + 1] << 8);
  SrSceneFrame input = {.struct_size = sizeof(input), .vram = r->vram,
    .cgram = r->palette, .mosaic = f->mosaic, .extra_x = extra_x, .top = top, .bottom = bottom,
    .main_screen = f->screen_enabled[0] & 3, .sub_screen = f->screen_enabled[1] & 3,
    .cgwsel = f->cgwsel, .cgadsub = f->cgadsub, .fixed_color = f->fixed_color};
  uint8_t additive = DioramaCaptureBlend_FullAddSubscreenSources(
      input.cgwsel, input.cgadsub, input.main_screen, input.sub_screen);
  r->scene.additive_plane_mask = 0;
  for (unsigned bg = 0; bg < 2; ++bg) {
    RoomLayer *l = &r->layers[bg]; l->plan = plan.layer[bg];
    l->page_cycle = bg == 1 && ActionRoomScene_HasBg2PageCycle(s);
    l->page = f->bg2_page_index;
    for (unsigned row = 0; row < 224; ++row) {
      ActionBgRowPolicy policy;
      ActionBgLayerPlan_ResolveValidatedRow(&l->plan, row, &policy);
      r->policies[bg][row] = (SrSceneRowPolicy){.fill = policy.edge + 1,
        .motion = policy.motion, .left = Extent(policy.horizontal_extent.mode, policy.horizontal_extent.left),
        .right = Extent(policy.horizontal_extent.mode, policy.horizontal_extent.right)};
    }
    SrSceneBackground *b = &input.backgrounds[bg];
    *b = (SrSceneBackground){.tiles = {.lookup = Lookup, .lookup_span = Span,
      .band_lookup = DioramaLayerOrder_VirtualLayerHasClassification(
          &r->terrain.virtual_layers[bg]) ? Band : NULL, .user_data = l, .camera_x = f->layer_camera_x[bg],
      .camera_y = f->layer_camera_y[bg], .hscroll_anchor = f->layer_camera_x[bg] & 1023,
      .vscroll_anchor = f->layer_camera_y[bg] & 1023,
      .flags = SR_PPU_VIRTUAL_TILEMAP_INCLUDE_AUTHENTIC},
      .edits = {.lookup = r->terrain.stamp_layers[bg].count || r->terrain.pixel_layers[bg].count
          ? Edit : NULL, .user_data = l, .apron = 64},
      .rows = r->policies[bg], .hscroll = f->bg_hscroll[bg], .vscroll = f->bg_vscroll[bg],
      .top = Extent(l->plan.vertical_extent.mode, l->plan.vertical_extent.top),
      .bottom = Extent(l->plan.vertical_extent.mode, l->plan.vertical_extent.bottom),
      .capture_flags = SR_PPU_OVERLAY_REMOVE_FROM_GAME,
      .clip_vertical = top || bottom,
      .clip_top = Min(Max(0, f->layer_camera_y[bg]), Max(top, bottom)),
      .clip_bottom = Min(Max(0, state.layer[bg].world_height - 225 - f->layer_camera_y[bg]), Max(top, bottom))};
    if (DioramaCaptureBlend_LayerIsHalfAdded(f->cgwsel, f->cgadsub, input.sub_screen, 1u << bg))
      b->capture_flags |= SR_PPU_OVERLAY_MARK_BG_HALF_ADD;
    if (additive & (1u << bg)) {
      b->capture_flags |= SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN;
      r->scene.additive_plane_mask |= (1u << bg) | (1u << (bg ? kDioramaPlane_Bg2Hi : kDioramaPlane_Bg1Hi)) |
          (1u << (bg ? kDioramaPlane_Bg2Far : kDioramaPlane_Bg1Far));
    }
    DioramaTransparentFill fill = kDioramaTransparentFill_None;
    b->fill_configured = DioramaLayerOrder_ResolveTransparentFill(r->table, s->group, s->map,
        0, bg, &fill, &b->fill_cgram);
    b->fill_mode = (uint8_t)fill; r->fill_configured[bg] = b->fill_configured;
  }
  if (ActRaiserRoom_ProfileFor(s->group, s->map) == kActRaiserRoomProfile_AitosWaterfall &&
      plan.layer[1].source == kActionBgSource_NativeTilemap)
    input.native_page_mask = 2;
  const ActionBgLayerPlan *sky = &plan.layer[1];
  if (sky->source == kActionBgSource_WorldMap && !sky->wrap_world_x && !sky->band_count &&
      sky->default_edge == kActionBgEdge_LiveWorld && !input.backgrounds[1].tiles.band_lookup) {
    const int left = sky->horizontal_extent.mode == kActionBgExtent_Fixed
        ? Min(extra_x, sky->horizontal_extent.left) : extra_x;
    const int right = sky->horizontal_extent.mode == kActionBgExtent_Fixed
        ? Min(extra_x, sky->horizontal_extent.right) : extra_x;
    input.background_view = (SrSceneBackgroundView){.layer = 1,
      .width = Min(256 + left + right, sky->world_width), .world_width = sky->world_width,
      .world_height = sky->world_height, .screen_x0 = -left};
  }
  if (!sr_scene_renderer_render(r->renderer, &input, &r->surfaces)) return false;
  memset(r->pixels, 0, sizeof(r->pixels));
  r->pixels[kDioramaPlane_Backdrop] = (const uint8_t *)r->surfaces.backdrop;
  static const int planes[2][3] = {{SR_PPU_OVERLAY_BG1, kDioramaPlane_Bg1Hi, kDioramaPlane_Bg1Far},
      {SR_PPU_OVERLAY_BG2, kDioramaPlane_Bg2Hi, kDioramaPlane_Bg2Far}};
  for (unsigned bg = 0; bg < 2; ++bg) {
    r->fill_argb[bg] = r->surfaces.transparent_fill[bg];
    for (unsigned band = 0; band < 3; ++band)
      if (r->surfaces.content[bg] & (1u << band))
        r->pixels[planes[bg][band]] = (const uint8_t *)r->surfaces.bands[bg][band];
  }
  const int width = r->surfaces.width, height = r->surfaces.height;
  DioramaBgSourceBounds bounds = {0};
  if (plan.layer[1].source == kActionBgSource_WorldMap && !plan.layer[1].wrap_world_x) {
    for (unsigned row = 0; row < 224; ++row) {
      int delta = ((int)f->bg_hscroll[1][row] - f->layer_camera_x[1]) & 1023;
      if (delta >= 512) delta -= 1024;
      int left = -f->layer_camera_x[1] - delta;
      DioramaBgSourceBounds_AddRow(&bounds, &plan.layer[1], row,
          left, left + state.layer[1].world_width, 1);
    }
  }
  DioramaBgValidSpanPlan_Build(extra_x + 64, extra_x, extra_x, extra_x, true,
      &plan.layer[1], &bounds, top, height, 640, &r->spans);
  r->skybox = (DioramaSkyboxView){0};
  if (r->surfaces.background_view) {
    const SrPpuBackgroundViewRequest v = {.width = input.background_view.width,
      .world_width = input.background_view.world_width, .screen_x0 = input.background_view.screen_x0};
    r->skybox.width = r->surfaces.view_width;
    r->skybox.capture_offset.x = f->layer_camera_x[1] - extra_x -
        SrPpuBackgroundView_WorldLeft(&v, f->layer_camera_x[1]);
  }
  const bool horizontal_bound = playfield == 0 &&
      plan.layer[0].source == kActionBgSource_WorldMap && !plan.layer[0].wrap_world_x &&
      plan.layer[0].default_edge == kActionBgEdge_LiveWorld && !plan.layer[0].band_count &&
      plan.layer[0].horizontal_extent.mode == kActionBgExtent_Available;
  r->capture = (DioramaCapture){.skybox = &r->skybox,.width = width, .height = height, .authentic_y0 = top,
    .obj_apron = 64, .bg_apron_mask = 3, .camera_y = y,
    .bg2_camera_x = f->layer_camera_x[1], .bg2_camera_y = f->layer_camera_y[1],
    .bg2_world_height = state.layer[1].world_height, .bg2_vertical_ratio = s->video_profile[10],
    .bg2_scroll_valid = true, .pixels = r->pixels, .bg2_valid_spans = &r->spans,
    .bg_transparent_fill_configured = r->fill_configured, .bg_transparent_fill_argb = r->fill_argb,
    .vertical_bounds = playfield == 0 ? DioramaVerticalBounds_Resolve(0, room_y, world_height, top, height)
        : (DioramaVerticalBounds){0},
    .horizontal_bounds = horizontal_bound ? DioramaHorizontalBounds_Resolve(0, x, world_x0, world_width, width, 64)
        : (DioramaHorizontalBounds){0},
    .framing_x = r->terrain.framing[0].x, .framing_y = r->terrain.framing[0].y};
  return true;
}
const DioramaCapture *EditorRoomScene_Capture(const EditorRoomScene *r) { return &r->capture; }
const DioramaScene *EditorRoomScene_Scene(const EditorRoomScene *r) { return &r->scene; }
const SrSceneSurfaces *EditorRoomScene_Surfaces(const EditorRoomScene *r) { return &r->surfaces; }
unsigned EditorRoomScene_Width(const EditorRoomScene *r) { return r->assets.scene.bg[0].pages_wide * 256u; }
unsigned EditorRoomScene_Height(const EditorRoomScene *r) { return r->assets.scene.bg[0].pages_high * 256u; }

uint32_t EditorRoomScene_Hash(const EditorRoomScene *r) {
  if (!r || !r->capture.pixels) return 0;
  uint32_t hash = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  hash = DeterministicHash_Fnv1a32Word(hash, r->surfaces.width);
  hash = DeterministicHash_Fnv1a32Word(hash, r->surfaces.height);
  for (int plane = 0; plane < kDioramaPlane_Count; ++plane) {
    const uint32_t *pixels = (const uint32_t *)r->pixels[plane];
    hash = DeterministicHash_Fnv1a32Word(hash, pixels != NULL);
    if (pixels) for (int i = 0; i < r->surfaces.height * r->surfaces.pitch_pixels; ++i)
      hash = DeterministicHash_Fnv1a32Word(hash, pixels[i]);
  }
  if (r->surfaces.background_view)
    for (int i = 0; i < r->surfaces.view_width * r->surfaces.height; ++i)
      hash = DeterministicHash_Fnv1a32Word(hash, r->surfaces.background_view[i]);
  for (unsigned bg = 0; bg < 2; ++bg) if (r->surfaces.native_pages[bg])
    for (unsigned i = 0; i < 256 * 256; ++i)
      hash = DeterministicHash_Fnv1a32Word(hash, r->surfaces.native_pages[bg][i]);
  return hash;
}
