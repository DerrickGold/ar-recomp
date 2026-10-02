/* Whole-room adapter. Pixel sampling, priority splitting, edit application and
 * margin scanout run through the native PPU, never a browser approximation. */
#include "room_scene.h"
#include "action/action_bg_world.h"
#include "actraiser/actraiser_room_profiles.h"
#include "diorama/diorama_capture_blend.h"
#include "diorama/diorama_scene_extent.h"
#include "deterministic_hash.h"
#include <math.h>
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
  ActionEnvironmentScene environment;
  ActionSceneryCaptureCache scenery_cache;
  ActionSceneEffectFrame effects;
  EditorRoomEffectSource default_sources[1024];
  unsigned default_source_count;
  ArRenderRectF timber_guides[1024];
  unsigned timber_guide_count;
  bool default_sources_ready;
  ActionEffectRecipes recipes;
  uint32_t actor_preview_source;
  int16_t actor_preview_x,actor_preview_y;
  uint16_t actor_preview_start;
  bool actor_preview_enabled;
  ActionEffectPreviewEvent event;
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
const ActionSurfaceField *EditorRoomScene_SurfaceField(const EditorRoomScene *r,unsigned index){
  if(!r||index>=kActionSurfaceFieldKinds)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i){const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->surface_field&&ActionSurfaceField_Index(e->kind)==(int)index&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&e->terrain==r->assets.terrain_profile)
      return &r->recipes.surface_fields[e->surface_field-1];}
  return ActionSurfaceField_Bundled((unsigned)index);
}
const ActionProjectileField *EditorRoomScene_ProjectileField(const EditorRoomScene *r,unsigned index){
  if(!r||index>=kActionProjectileFieldKinds)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i){const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->projectile_field&&ActionProjectileField_Index(e->kind)==(int)index&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&e->terrain==r->assets.terrain_profile)
      return &r->recipes.projectile_fields[e->projectile_field-1];}
  return ActionProjectileField_Bundled((unsigned)index);
}
const ActionArcField *EditorRoomScene_ArcField(const EditorRoomScene *r,unsigned index){
  if(!r||index>=kActionArcFieldKinds)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i){const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->arc_field&&ActionArcField_Index(e->kind)==(int)index&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&e->terrain==r->assets.terrain_profile)
      return &r->recipes.arc_fields[e->arc_field-1];}
  return ActionArcField_Bundled((unsigned)index);
}
const ActionGlowField *EditorRoomScene_GlowField(const EditorRoomScene *r) {
  if(!r)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i) {
    const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->glow_field&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&
       e->terrain==r->assets.terrain_profile)return &r->recipes.glow_fields[e->glow_field-1];
  }
  const ActionGlowField *field=ActionGlowField_Bundled(r->assets.scene.group,r->assets.scene.map);
  return field?field:ActionGlowField_Bundled(2,3);
}
const ActionCastleField *EditorRoomScene_CastleField(const EditorRoomScene *r) {
  if(!r)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i) {
    const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->castle_field&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&
       e->terrain==r->assets.terrain_profile)return &r->recipes.castle_fields[e->castle_field-1];
  }
  return ActionCastleField_Bundled(r->assets.scene.group==2&&r->assets.scene.map>=2?r->assets.scene.map:3);
}
const ActionMarshField *EditorRoomScene_MarshField(const EditorRoomScene *r) {
  if(!r)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i) {
    const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->marsh_field&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&
       e->terrain==r->assets.terrain_profile)return &r->recipes.marsh_fields[e->marsh_field-1];
  }
  return ActionMarshField_Bundled();
}
const ActionMoonField *EditorRoomScene_MoonField(const EditorRoomScene *r) {
  if(!r)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i) {
    const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->moon_field&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&
       e->terrain==r->assets.terrain_profile)return &r->recipes.moon_fields[e->moon_field-1];
  }
  return ActionMoonField_Bundled();
}
const ActionWaterField *EditorRoomScene_WaterField(const EditorRoomScene *r) {
  if(!r)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i) {
    const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->water_field&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&
       e->terrain==r->assets.terrain_profile)return &r->recipes.water_fields[e->water_field-1];
  }
  return ActionWaterField_Bundled();
}
const ActionAtmosphereField *EditorRoomScene_AtmosphereField(const EditorRoomScene *r) {
  if(!r)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i) {
    const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->atmosphere_field&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&
       e->terrain==r->assets.terrain_profile)return &r->recipes.atmosphere_fields[e->atmosphere_field-1];
  }
  return ActionAtmosphereField_Bundled(r&&r->assets.scene.group==1&&r->assets.scene.map>=2&&r->assets.scene.map<=4?r->assets.scene.map:2);
}
double EditorRoomScene_WaterFieldMapScale(const EditorRoomScene *r,unsigned axis) {
  if(!r||axis>1)return 1;
  ActionRoomSceneFrameRequest probe=r->assets.frame;probe.camera_x=probe.camera_y=256;
  ActionRoomSceneFrameState frame;
  if(!ActionRoomScene_BuildFrameState(&r->assets.scene,&probe,&frame))return 1;
  const int32_t *camera=axis?frame.layer_camera_y:frame.layer_camera_x;
  return camera[1]>0?256.0/camera[1]:1;
}
const ActionRayField *EditorRoomScene_RayField(const EditorRoomScene *r) {
  if(!r)return NULL;
  for(unsigned i=0;i<r->recipes.count;++i) {
    const ActionEffectRecipe *e=&r->recipes.records[i];
    if(e->field&&e->group==r->assets.scene.group&&e->room==r->assets.scene.map&&
       e->terrain==r->assets.terrain_profile)return &r->recipes.fields[e->field-1];
  }
  return ActionRayField_Bundled();
}
double EditorRoomScene_RayFieldMapScale(const EditorRoomScene *r, unsigned axis) {
  if (!r || axis > 1) return 1;
  ActionRoomSceneFrameRequest probe = r->assets.frame;
  probe.camera_x = probe.camera_y = 256;
  ActionRoomSceneFrameState frame;
  if (!ActionRoomScene_BuildFrameState(&r->assets.scene, &probe, &frame)) return 1;
  const int32_t *camera = axis ? frame.layer_camera_y : frame.layer_camera_x;
  const float position = (camera[0] + camera[1]) * .5f;
  return position > 0 ? 256 / position : 1;
}

static int CompareTimberGuides(const void *a, const void *b) {
  const ArRenderRectF *left = a, *right = b;
  if (left->y != right->y) return left->y < right->y ? -1 : 1;
  return (left->x > right->x) - (left->x < right->x);
}

unsigned EditorRoomScene_DefaultSourceCount(EditorRoomScene *r) {
  if (!r) return 0;
  if (r->default_sources_ready) return r->default_source_count;
  r->default_sources_ready = true;
  const unsigned width = r->assets.scene.bg[0].pages_wide * 256u;
  const unsigned height = r->assets.scene.bg[0].pages_high * 256u;
  float map_scale[2][2] = {{1, 1}, {1, 1}};
  ActionRoomSceneFrameRequest probe = r->assets.frame;
  probe.camera_x = probe.camera_y = 256;
  ActionRoomSceneFrameState p;
  if (ActionRoomScene_BuildFrameState(&r->assets.scene, &probe, &p))
    for (unsigned axis = 0; axis < 2; ++axis) {
      const int32_t *camera = axis ? p.layer_camera_y : p.layer_camera_x;
      const float positions[2] = {camera[1], (camera[0] + camera[1]) * .5f};
      for (unsigned plane = 0; plane < 2; ++plane)
        if (positions[plane] > 0) map_scale[plane][axis] = 256 / positions[plane];
    }
  ActionSceneEffectFrame sample = {0};
  for (unsigned y = 0; y < height; y += 224)
    for (unsigned x = 0; x < width; x += 256) {
      ActionRoomSceneFrameRequest request = r->assets.frame;
      request.camera_x = x;
      request.camera_y = y;
      request.game_frame = 0;
      ActionRoomSceneFrameState frame;
      ActionEnvironmentScene scene;
      if (!ActionRoomScene_BuildFrameState(&r->assets.scene, &request, &frame) ||
          !ActionEnvironmentScene_FromRoom(&scene, &r->assets.scene, &frame))
        continue;
      memset(&sample, 0, sizeof(sample));
      ActionMapEnvironmentScene_Capture(&scene, &sample, NULL);
      ActionEnvironmentScene_Capture(&scene, &sample);
      /* Capture only once while inventorying the room. These silhouettes are
       * derived from terrain, so expose the actual timber rather than the
       * camera-local origin of its aggregate render record. */
      for (unsigned i = 0; i < sample.bloodpool.timber_count; ++i) {
        const ActionBloodpoolTimber *t = &sample.bloodpool.timber[i];
        const ArRenderRectF guide = {t->x0, t->y, t->x1 - t->x0, 2};
        bool found = false;
        for (unsigned j = 0; j < r->timber_guide_count; ++j)
          if (!memcmp(&guide, &r->timber_guides[j], sizeof(guide))) { found = true; break; }
        if (!found && r->timber_guide_count < 1024)
          r->timber_guides[r->timber_guide_count++] = guide;
      }
      for (unsigned i = 0; i < sample.decoration_count; ++i) {
        const ActionEffectInstance *e = &sample.decorations[i];
        bool found = false;
        for (unsigned j = 0; j < r->default_source_count; ++j)
          if (r->default_sources[j].effect.kind == e->kind &&
              r->default_sources[j].effect.generation == e->generation)
            found = true;
        if (found) continue;
        if (r->default_source_count == 1024) return r->default_source_count;
        EditorRoomEffectSource *source = &r->default_sources[r->default_source_count++];
        source->effect = *e;
        /* Catalogue guides use the light layer's native coordinates. Map
         * handles invert its scroll ratio to operate in BG1 map pixels. */
        source->map_scale_x = source->map_scale_y = 1;
        if (e->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds ||
            e->projection_plane == kActionEffectProjectionPlane_Bg2) {
          const unsigned plane =
              e->projection_plane == kActionEffectProjectionPlane_BetweenBackgrounds ? 1 : 0;
          source->map_scale_x = map_scale[plane][0];
          source->map_scale_y = map_scale[plane][1];
        }
      }
    }
  /* A log can span several metatiles. One guide per continuous exposed edge
   * is enough to reach its shared controls and avoids a row of stacked icons. */
  qsort(r->timber_guides, r->timber_guide_count, sizeof(r->timber_guides[0]), CompareTimberGuides);
  unsigned count = 0;
  for (unsigned i = 0; i < r->timber_guide_count; ++i) {
    const ArRenderRectF next = r->timber_guides[i];
    ArRenderRectF *last = count ? &r->timber_guides[count-1] : NULL;
    if (last && last->y == next.y && next.x <= last->x + last->w)
      last->w = fmaxf(last->x + last->w, next.x + next.w) - last->x;
    else r->timber_guides[count++] = next;
  }
  r->timber_guide_count = count;
  return r->default_source_count;
}
const EditorRoomEffectSource *EditorRoomScene_DefaultSource(EditorRoomScene *r, unsigned index) {
  return index < EditorRoomScene_DefaultSourceCount(r) ? &r->default_sources[index] : NULL;
}

unsigned EditorRoomScene_DefaultGuideCount(EditorRoomScene *r, unsigned index) {
  const EditorRoomEffectSource *s = EditorRoomScene_DefaultSource(r, index);
  if (!s) return 0;
  switch (s->effect.kind) {
  case kActionEffect_BloodpoolWater: case kActionEffect_BloodpoolMist:
    return (unsigned)EditorRoomScene_MarshField(r)->SpanCount[0];
  case kActionEffect_BloodpoolAir:
    return (unsigned)EditorRoomScene_MarshField(r)->SpanCount[0] * 2;
  case kActionEffect_BloodpoolTimber: return r->timber_guide_count;
  case kActionEffect_BloodpoolMoonlight: case kActionEffect_BloodpoolMoonReflection:
  case kActionEffect_BloodpoolCloud: return 1;
  default: return 0;
  }
}
bool EditorRoomScene_DefaultGuide(EditorRoomScene *r, unsigned index, unsigned guide,
                                  ArRenderRectF *bounds) {
  if (!bounds || guide >= EditorRoomScene_DefaultGuideCount(r, index)) return false;
  const unsigned kind = r->default_sources[index].effect.kind;
  const ActionMarshField *m = EditorRoomScene_MarshField(r);
  const ActionMoonField *f = EditorRoomScene_MoonField(r);
  if (kind == kActionEffect_BloodpoolTimber) *bounds = r->timber_guides[guide];
  else if (kind == kActionEffect_BloodpoolAir) {
    const float bank = guide & 1 ? m->spans[guide/2][1] - m->InsectRegion[0] :
                                  m->spans[guide/2][0] + m->InsectRegion[0];
    const float rx = m->InsectMotion[2] + m->InsectMotion[4], ry = m->InsectMotion[6];
    *bounds = (ArRenderRectF){bank-rx, m->InsectRegion[1]-ry, rx*2, m->InsectRegion[2]+ry*2};
  } else if (kind == kActionEffect_BloodpoolWater || kind == kActionEffect_BloodpoolMist) {
    const float top = kind == kActionEffect_BloodpoolMist ? m->Surface[0]+m->MistWindow[0] : m->Surface[1];
    const float bottom = kind == kActionEffect_BloodpoolMist ? m->Surface[0]+m->MistWindow[1] : m->Surface[2];
    *bounds = (ArRenderRectF){m->spans[guide][0], top, m->spans[guide][1]-m->spans[guide][0], bottom-top};
  } else if (kind == kActionEffect_BloodpoolMoonlight) {
    *bounds = (ArRenderRectF){f->Anchor[0]-8, f->Anchor[1]-8, 16, 16};
  } else if (kind == kActionEffect_BloodpoolCloud) {
    *bounds = (ArRenderRectF){f->Anchor[0]+f->CloudMotion[2]-f->CloudShape[0],
        f->Anchor[1]+f->CloudMotion[5]-f->CloudShape[1], f->CloudShape[0]*2, f->CloudShape[1]*2};
  } else {
    *bounds = (ArRenderRectF){f->Anchor[0]+f->ReflectionGlow[0]-f->ReflectionGlow[2],
        f->Anchor[1]+f->ReflectionGlow[1]-f->ReflectionGlow[3], f->ReflectionGlow[2]*2, f->ReflectionGlow[3]*2};
  }
  return true;
}

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
static bool EnvironmentTileEdit(void *context, unsigned bg, int x, int y,
                                ActionEnvironmentTileEdit *out) {
  EditorRoomScene *r = context;
  if (bg >= 2) return false;
  RoomLayer *l = &r->layers[bg];
  SrPpuCaptureTile tile;
  if (!Edit(l, x, y, &tile)) {
    uint16_t word;
    if (!Lookup(l, x, y, &word)) return false;
    tile = (SrPpuCaptureTile){.entry = word, .band = (word & 0x2000) ? 2 : 1};
    (void)Band(l, x, y, word, &tile.band);
  }
  *out = (ActionEnvironmentTileEdit){.entry = tile.entry,
                                     .band = tile.band,
                                     /* Both paths above resolve the actual world word. */
                                     .replace = 1,
                                     .blank = (tile.flags & SR_PPU_CAPTURE_TILE_BLANK) != 0};
  memcpy(out->black, tile.black_rows, 8);
  memcpy(out->transparent, tile.transparent_rows, 8);
  return true;
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
  const ActionRoomSceneFrameRequest request={0};
  if (!ActionRoomScene_BuildFrameState(&r->assets.scene,&request,&r->frame) ||
      !ActionEnvironmentScene_FromRoom(&r->environment,&r->assets.scene,&r->frame)) {
    EditorRoomScene_Destroy(r); return NULL;
  }
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
bool EditorRoomScene_PreviewBinding(EditorRoomScene *r,bool enabled,uint32_t source,int x,int y,unsigned start) {
  if(!r||x<-2048||x>16384||y<-2048||y>16384||start>65535)return false;
  r->actor_preview_enabled=enabled;r->actor_preview_source=source;
  r->actor_preview_x=x;r->actor_preview_y=y;r->actor_preview_start=start;return true;
}
static void CaptureBindingPreview(EditorRoomScene *r) {
  if(!r->actor_preview_enabled)return;
  for(unsigned i=0;i<r->recipes.count;++i){const ActionEffectRecipe *e=&r->recipes.records[i];
    if(!e->emitter||!e->actor.target||e->source!=r->actor_preview_source||e->group!=r->environment.group||
        e->room!=r->environment.room||e->terrain!=r->assets.terrain_profile)continue;
    const ActionEffectActorSelector *a=&e->actor;
    const uint16_t age=(uint16_t)(r->environment.clock-r->actor_preview_start);
    r->effects.actors[0]=(ActionEffectActor){.generation=1,.source=a->source,.parent_source=a->parent,.animation=a->animation,
      .state=a->state_first,.visual=a->visual_first,.handler=a->handler,.resume=a->resume,.age=age,.phase_ticks=age,
      .x=r->actor_preview_x,.y=r->actor_preview_y,.bank=0x7e,.priority=2,.visible=1,.player=a->target==2};
    r->effects.actor_count=1;return;
  }
}
void EditorRoomScene_SetEvent(EditorRoomScene *r,const ActionEffectPreviewEvent *event) {if(r)r->event=event?*event:(ActionEffectPreviewEvent){0};}
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
  memset(&r->effects,0,sizeof(r->effects));
  if (!ActionEnvironmentScene_FromRoom(&r->environment,s,f)) return false;
  uint8_t chars[kActionRoomSceneCharacterBytes];
  if (!ActionRoomScene_BuildCharacters(s, frame, f->animation_phase, chars, sizeof(chars)))
    return false;
  for (unsigned i = 0; i < sizeof(chars) / 2; ++i)
    r->vram[i] = chars[i * 2] | ((uint16_t)chars[i * 2 + 1] << 8);
  for (unsigned i = 0; i < sizeof(s->extra_characters) / 2; ++i)
    r->vram[sizeof(chars) / 2 + i] =
        s->extra_characters[i * 2] | ((uint16_t)s->extra_characters[i * 2 + 1] << 8);
  for (unsigned i = 0; i < sizeof(s->palette) / 2; ++i)
    r->palette[i] = s->palette[i * 2] | ((uint16_t)s->palette[i * 2 + 1] << 8);
  r->environment.vram = r->vram;
  r->environment.tile_base[0] = 0;
  r->environment.tile_base[1] = 0;
  r->environment.tile_edit = EnvironmentTileEdit;
  r->environment.tile_edit_context = r;
  r->effects.game_frame = r->environment.clock;
  r->environment.suppress_default_glow_field=ActionEffectRecipes_ReplacesGlowField(&r->recipes,
      r->environment.group,r->environment.room,r->assets.terrain_profile);
  ActionEffectRecipes_SurfaceFields(&r->recipes,r->environment.group,r->environment.room,r->assets.terrain_profile,r->environment.surface_fields);
  ActionMapEnvironmentScene_Capture(&r->environment,&r->effects,NULL);
  r->environment.suppress_default_castle_field=ActionEffectRecipes_ReplacesCastleField(&r->recipes,
      r->environment.group,r->environment.room,r->assets.terrain_profile);
  r->environment.suppress_default_marsh_field=ActionEffectRecipes_ReplacesMarshField(&r->recipes,
      r->environment.group,r->environment.room,r->assets.terrain_profile);
  r->environment.suppress_default_moon_field=ActionEffectRecipes_ReplacesMoonField(&r->recipes,
      r->environment.group,r->environment.room,r->assets.terrain_profile);
  r->environment.suppress_default_water_field=ActionEffectRecipes_ReplacesWaterField(&r->recipes,
      r->environment.group,r->environment.room,r->assets.terrain_profile);
  r->environment.suppress_default_atmosphere_field=ActionEffectRecipes_ReplacesAtmosphereField(&r->recipes,
      r->environment.group,r->environment.room,r->assets.terrain_profile);
  r->environment.suppress_default_ray_field=ActionEffectRecipes_ReplacesRayField(&r->recipes,
      r->environment.group,r->environment.room,r->assets.terrain_profile);
  ActionEnvironmentScene_Capture(&r->environment,&r->effects);
  r->scene.layer_section = kDioramaLayerSection_Room;
  if (!r->effects.decoration_overflow)
    for (unsigned i = 0; i < r->effects.decoration_count; ++i)
      if (r->effects.decorations[i].kind == kActionEffect_AitosWaterfall)
        r->scene.layer_section = kDioramaLayerSection_AitosWaterfall;
  ActionEffectInstance event;
  if(ActionEffectPreview_Build(&r->event,r->environment.clock,&event)) {
    if(event.render_layer==kActionEffectRenderLayer_WorldDust && r->effects.decoration_count<kActionSceneDecorationMaxInstances)
      r->effects.decorations[r->effects.decoration_count++]=event;
    else {r->effects.effects[0]=event;r->effects.effect_count=r->effects.visible_count=1;}
  }
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
        r->scene.layer_section, bg, &fill, &b->fill_cgram);
    b->fill_mode = (uint8_t)fill; r->fill_configured[bg] = b->fill_configured;
  }
  CaptureBindingPreview(r);
  r->environment.scenery_cache = &r->scenery_cache;
  ActionEffectRecipes_Apply(&r->recipes, s->group, s->map, r->assets.terrain_profile,
                            r->environment.clock, &r->environment, &r->effects);
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
bool EditorRoomScene_ConfigureEffects(EditorRoomScene *r, const char *text, size_t size, unsigned *line) {
  return r && ActionEffectRecipes_Parse(&r->recipes,text,size,line);
}
const ActionSceneEffectFrame *EditorRoomScene_Effects(const EditorRoomScene *r) { return &r->effects; }
const ActionEnvironmentScene *EditorRoomScene_Environment(const EditorRoomScene *r) { return &r->environment; }
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

/* Canonical source digest deliberately excludes structure padding/pointers. */
uint32_t EditorRoomScene_EffectHash(const EditorRoomScene *r) {
  if (!r) return 0;
  const ActionSceneEffectFrame *frame = &r->effects;
  uint32_t h = DETERMINISTIC_HASH_FNV1A32_OFFSET;
  for(unsigned i=0;i<kActionSurfaceFieldKinds;++i){
    const ActionSurfaceField *field=(frame->surface_fields_valid&(1u<<i))?&frame->surface_fields[i]:ActionSurfaceField_Bundled(i);
    h=DeterministicHash_Fnv1a32Word(h,field->hash);
    h=DeterministicHash_Fnv1a32Word(h,(unsigned)field->Components[0]);
  }
  for(unsigned i=0;i<kActionProjectileFieldKinds;++i){
    const ActionProjectileField *field=(frame->projectile_fields_valid&(1u<<i))?&frame->projectile_fields[i]:ActionProjectileField_Bundled(i);
    h=DeterministicHash_Fnv1a32Word(h,field->hash);
    h=DeterministicHash_Fnv1a32Word(h,(unsigned)field->Components[0]);
  }
  for(unsigned i=0;i<kActionArcFieldKinds;++i){
    const ActionArcField *field=(frame->arc_fields_valid&(1u<<i))?&frame->arc_fields[i]:ActionArcField_Bundled(i);
    h=DeterministicHash_Fnv1a32Word(h,field->hash);
    h=DeterministicHash_Fnv1a32Word(h,(unsigned)field->Components[0]);
  }
  h=DeterministicHash_Fnv1a32Word(h,frame->glow_field_valid);
  if(frame->glow_field_valid) {
    char definition[16384];
    const size_t size=ActionGlowField_Write(&frame->glow_field,definition,sizeof(definition));
    for(size_t i=0;i<size;++i)h=(h^(unsigned char)definition[i])*16777619u;
  }
  h=DeterministicHash_Fnv1a32Word(h,frame->castle_field_valid);
  if(frame->castle_field_valid) {
    char definition[16384];
    const size_t size=ActionCastleField_Write(&frame->castle_field,definition,sizeof(definition));
    for(size_t i=0;i<size;++i)h=(h^(unsigned char)definition[i])*16777619u;
  }
  h=DeterministicHash_Fnv1a32Word(h,frame->bloodpool.field_valid);
  if(frame->bloodpool.field_valid) {
    char definition[16384];
    const size_t size=ActionMarshField_Write(&frame->bloodpool.field,definition,sizeof(definition));
    for(size_t i=0;i<size;++i)h=(h^(unsigned char)definition[i])*16777619u;
  }
  h=DeterministicHash_Fnv1a32Word(h,frame->moon_field_valid);
  if(frame->moon_field_valid) {
    char definition[16384];
    const size_t size=ActionMoonField_Write(&frame->moon_field,definition,sizeof(definition));
    for(size_t i=0;i<size;++i)h=(h^(unsigned char)definition[i])*16777619u;
  }
  h=DeterministicHash_Fnv1a32Word(h,frame->water_field_valid);
  if(frame->water_field_valid) {
    char definition[16384];
    const size_t size=ActionWaterField_Write(&frame->water_field,definition,sizeof(definition));
    for(size_t i=0;i<size;++i)h=(h^(unsigned char)definition[i])*16777619u;
  }
  h=DeterministicHash_Fnv1a32Word(h,frame->atmosphere_field_valid);
  if(frame->atmosphere_field_valid) {
    char definition[16384];
    const size_t size=ActionAtmosphereField_Write(&frame->atmosphere_field,definition,sizeof(definition));
    for(size_t i=0;i<size;++i)h=(h^(unsigned char)definition[i])*16777619u;
  }
  h = DeterministicHash_Fnv1a32Word(h, frame->ray_field_valid);
  if (frame->ray_field_valid) {
    /* The codec omits padding and uses round-trip decimal values. This debug
     * digest is outside capture/rendering; it must include complete recipes. */
    char definition[16384];
    const size_t size = ActionRayField_Write(&frame->ray_field, definition, sizeof(definition));
    for (size_t i = 0; i < size; ++i) {
      h ^= (unsigned char)definition[i];
      h *= UINT32_C(16777619);
    }
  }
  for (unsigned list=0;list<3;++list) {
    const unsigned count=list==2?frame->effect_count:list?frame->authored_count:frame->decoration_count;
    const ActionEffectInstance *instances=list==2?frame->effects:list?frame->authored:frame->decorations;
    h=DeterministicHash_Fnv1a32Word(h,count);
    for (unsigned i=0;i<count;++i) {
      const ActionEffectInstance *e=&instances[i];
      const uint32_t words[]={e->generation,e->pulse_generation,(uint32_t)(int32_t)e->world_x,
        (uint32_t)(int32_t)e->world_y,e->kind,e->phase,e->flags,e->render_layer,e->projection_plane,
        e->age_ticks,e->phase_ticks,e->pulse_ticks,e->source_mask,e->tuning.active,e->tuning.color,
        e->tuning.light_receivers,e->tuning.dim_receivers,e->tuning.light_receivers_set,e->tuning.dim_receivers_set,
        e->particle_count,e->particle_lifetime,e->particle_style.active,e->particle_style.seed,e->particle_style.color_end,e->field_style.strands,e->field_style.pattern,e->field_style.placement,e->field_style.point_count};
      for (unsigned j=0;j<sizeof(words)/sizeof(words[0]);++j) h=DeterministicHash_Fnv1a32Word(h,words[j]);
      for(unsigned j=0;j<e->field_style.point_count && j<kActionContourMaxPoints;++j) {
        uint32_t bits;memcpy(&bits,&e->field_style.points[j].x,4);h=DeterministicHash_Fnv1a32Word(h,bits);
        memcpy(&bits,&e->field_style.points[j].y,4);h=DeterministicHash_Fnv1a32Word(h,bits);
      }
      const float values[]={e->geometry.data.rect.x0,e->geometry.data.rect.y0,
        e->geometry.data.rect.x1,e->geometry.data.rect.y1,e->clip_rect.x0,e->clip_rect.y0,
        e->clip_rect.x1,e->clip_rect.y1,e->tuning.intensity,e->tuning.reach,
        e->particle_style.size_min,e->particle_style.size_max,e->particle_style.travel_x,
        e->particle_style.travel_y,e->particle_style.wander,e->particle_style.spread,e->field_style.angle,e->field_style.fan,
        e->field_style.softness,e->field_style.drift,e->field_style.amplitude};
      for (unsigned j=0;j<sizeof(values)/sizeof(values[0]);++j) {
        uint32_t bits;memcpy(&bits,&values[j],sizeof(bits));h=DeterministicHash_Fnv1a32Word(h,bits);
      }
    }
  }
  for (unsigned i=0;i<frame->authored_count;++i) {
    const ActionEffectFloorField *floor=&frame->authored_floor[i];
    h=DeterministicHash_Fnv1a32Word(h,floor->count);
    for (unsigned j=0;j<floor->count;++j) {
      const ActionEffectFloorSpan *s=&floor->spans[j];
      const float values[]={s->x0,s->x1,s->y,s->height};
      for (unsigned k=0;k<4;++k) {
        uint32_t bits;memcpy(&bits,&values[k],sizeof(bits));h=DeterministicHash_Fnv1a32Word(h,bits);
      }
    }
  }
  h = DeterministicHash_Fnv1a32Word(h, frame->members.count);
  for (unsigned i = 0; i < frame->members.count; ++i) {
    const ActionNativeMember *m = &frame->members.records[i];
    h = DeterministicHash_Fnv1a32Word(h, m->kind | ((uint32_t)m->index << 8) |
                                             ((uint32_t)m->enabled << 16));
    h = DeterministicHash_Fnv1a32Word(h, m->color);
    const float values[] = {m->offset_x,     m->offset_y, m->width_scale,
                            m->length_scale, m->angle,    m->intensity};
    for (unsigned j = 0; j < 6; ++j) {
      uint32_t bits;
      memcpy(&bits, &values[j], 4);
      h = DeterministicHash_Fnv1a32Word(h, bits);
    }
  }
  h = DeterministicHash_Fnv1a32Word(h, frame->scenery.valid);
  h = DeterministicHash_Fnv1a32Word(h, frame->scenery.count);
  for (unsigned i = 0; i < frame->scenery.count; ++i) {
    const ActionMoonlightOccluder *o = &frame->scenery.rectangles[i];
    h = DeterministicHash_Fnv1a32Word(h, (uint16_t)o->x0 | ((uint32_t)(uint16_t)o->x1 << 16));
    h = DeterministicHash_Fnv1a32Word(h, (uint16_t)o->y0 | ((uint32_t)(uint16_t)o->y1 << 16));
  }
  h=DeterministicHash_Fnv1a32Word(h,frame->moonlight.valid);
  for (unsigned i=0;i<frame->moonlight.count;++i) {
    const ActionMoonlightOccluder *o=&frame->moonlight.rectangles[i];
    h=DeterministicHash_Fnv1a32Word(h,(uint16_t)o->x0|((uint32_t)(uint16_t)o->x1<<16));
    h=DeterministicHash_Fnv1a32Word(h,(uint16_t)o->y0|((uint32_t)(uint16_t)o->y1<<16));
  }
  return h;
}
