/* Stage-owned environmental capture. Phase: capture; read-only WRAM. */
#include "action/action_environment_capture_internal.h"
#include "action_bloodpool_surface.h"
#include "action_bloodpool_occluders.h"
#include "action_castle_sources.h"


static bool BloodpoolWaterTile(uint8_t tile) {
  switch (tile) {
    case 0x18: case 0x19: case 0x20: case 0x21: case 0x22:
    case 0x5E: case 0x9E: case 0x9F: case 0xA0: return true;
    default: return false;
  }
}

static void CaptureBloodpoolWaterScroll(ActionBloodpoolDetails *dst,
    const ActionEnvironmentScene *scene) {
  dst->water_scroll_valid = scene->water_scroll_valid;
  memcpy(dst->water_scroll,scene->water_scroll,sizeof(dst->water_scroll));
}

/* Merge scanline runs, then identical runs in adjacent rows. The 768x352
 * window includes the largest extended capture plus its horizontal apron.
 * Reads are bounded by the validated native map/definition table. */
static bool CaptureMoonOccluders(ActionMoonlightOcclusion *dst,
    const ActionBgMapView *map, const ActionEnvironmentScene *scene, unsigned mask, unsigned attributes) {
  const int camera_x = scene->camera_x[0];
  const int camera_y = scene->camera_y[0];
  const int x0 = camera_x > 256 ? (camera_x-256)&~7 : 0;
  const int x1 = camera_x+512 < 4096 ? (camera_x+519)&~7 : 4096;
  const int y0 = camera_y > 64 ? camera_y-64 : 0;
  const int y1 = camera_y+288 < 512 ? camera_y+288 : 512;
  /* Alternating pixels are the worst case; rounding the window to whole CHR
   * tiles can add seven columns. No heap or full-resolution mask is needed. */
  uint16_t runs[2][388];
  uint16_t words[97];
  unsigned previous_count = 0;
  dst->count = 0;
  for (int y = y0; y < y1; y++) {
    if (y == y0 || !(y&7)) {
      for (int x = x0; x < x1; x += 8) {
        uint8_t metatile;
        uint16_t word = 0xFF;
        if (ActionBgMapView_LookupMetatile(map,x,y,&metatile)) {
          const unsigned quadrant = ((unsigned)y&8u)/4+((unsigned)x&8u)/8;
          word = (ActionEnvironmentScene_Word(scene,0,metatile,quadrant)&mask)|attributes;
        }
        words[(x-x0)/8] = word;
      }
    }
    uint16_t *previous = runs[(y-y0)&1], *current = runs[(y-y0+1)&1];
    unsigned count = 0, match = 0;
    int start = -1;
    for (int tile_x = x0; tile_x <= x1; tile_x += 8) {
      unsigned bits = 0;
      if (tile_x < x1) {
        const uint16_t word = words[(tile_x-x0)/8];
        unsigned row = (unsigned)y&7u;
        if (word&0x8000u) row = 7-row;
        if (!(word&0x2000u)) bits = (unsigned)(kBloodpoolTileOpacity[word&255u] >> (row*8))&255u;
        if (word&0x4000u) {
          bits = ((bits&0x55u)<<1)|((bits>>1)&0x55u);
          bits = ((bits&0x33u)<<2)|((bits>>2)&0x33u);
          bits = (bits<<4)|(bits>>4);
          bits &= 255u;
        }
      }
      if (bits == 255u) { if (start < 0) start = tile_x; continue; }
      if (!bits && start < 0) continue;
      for (int pixel = 0; pixel < 8; pixel++) {
        const int x = tile_x+pixel;
        if (bits&(1u<<pixel)) { if (start < 0) start = x; continue; }
        if (start < 0) continue;
        while (match < previous_count && dst->rectangles[previous[match]].x0 < start) match++;
        unsigned index;
        if (match < previous_count && dst->rectangles[previous[match]].x0 == start &&
            dst->rectangles[previous[match]].x1 == x) {
          index = previous[match++];
          dst->rectangles[index].y1 = (int16_t)(y+1);
        } else {
          if (dst->count == kActionMoonlightMaxOccluders) return false;
          index = dst->count++;
          dst->rectangles[index] = (ActionMoonlightOccluder){
            (int16_t)start,(int16_t)y,(int16_t)x,(int16_t)(y+1)};
        }
        if (count == sizeof(runs[0])/sizeof(runs[0][0])) return false;
        current[count++] = (uint16_t)index;
        start = -1;
      }
    }
    previous_count = count;
  }
  dst->valid = true;
  return true;
}

static void CaptureBloodpoolMoon(const ActionEnvironmentScene *scene, ActionSceneEffectFrame *dst,
    const ActionBgMapView *foreground) {
  const ActionBgMapView sky = scene->maps[1];
  uint8_t top, bottom;
  const unsigned mask = scene->word_mask[0], attributes = scene->attributes[0];
  if (dst->decoration_count > kActionSceneDecorationMaxInstances-2 ||
      !scene->metatiles[0] || mask != 0xECFF || attributes != 0x1000 ||
      !ActionBgMapView_LookupMetatile(&sky,112,48,&top) || top != 0x3C ||
      !ActionBgMapView_LookupMetatile(&sky,112,64,&bottom) || bottom != 0x44)
    return;
  if (!CaptureMoonOccluders(&dst->moonlight,foreground,scene,mask,attributes)) {
    dst->moonlight.count = 0;
    dst->moonlight.valid = false;
    return;
  }
  for (unsigned family = 0; family < 2; family++) {
    const ActionEffectInstance effect = {
      .generation = 0xB1000002u+family, .pulse_generation = 0xB1000002u+family,
      .world_x = 112, .world_y = 62, .environment_room = 1,
      .age_ticks = scene->clock, .phase_ticks = scene->clock,
      .pulse_ticks = scene->clock,
      .kind = family ? kActionEffect_BloodpoolMoonReflection : kActionEffect_BloodpoolMoonlight,
      .phase = kActionEffectPhase_BloodpoolEnvironment,
      .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
      .render_layer = kActionEffectRenderLayer_Bg2Plane,
      .projection_plane = kActionEffectProjectionPlane_Bg2,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,194}},
      .clip_rect = {-384,0,384,194},
    };
    (void)SceneDecorationAppend(dst,&effect);
  }
}

static bool BloodpoolPixel(const ActionBgMapView *map, const ActionEnvironmentScene *scene, int x, int y, bool low_only) {
  uint8_t tile;
  if (!ActionBgMapView_LookupMetatile(map,x,y,&tile)) return false;
  const unsigned quadrant = ((unsigned)y&8u)/4+((unsigned)x&8u)/8;
  const uint16_t word = (ActionEnvironmentScene_Word(scene,0,tile,quadrant)&0xECFFu)|0x1000u;
  if (low_only && (word&0x2000u)) return false;
  const unsigned px = (word&0x4000u) ? 7-((unsigned)x&7u) : (unsigned)x&7u;
  const unsigned py = (word&0x8000u) ? 7-((unsigned)y&7u) : (unsigned)y&7u;
  return (kBloodpoolTileOpacity[word&255u] >> (py*8+px))&1u;
}

static bool BloodpoolExposedWater(int x, uint16_t sources) {
  for (unsigned i = 0; i < kBloodpoolWaterSpanCount; i++)
    if ((sources&(1u<<i)) && x >= kBloodpoolWaterSpans[i].left+6 &&
        x < kBloodpoolWaterSpans[i].right-6) return true;
  return false;
}

static bool BloodpoolPostPixel(const ActionBgMapView *map, const ActionEnvironmentScene *scene, int x, int y) {
  uint8_t tile;
  if (!ActionBgMapView_LookupMetatile(map,x,y,&tile)) return false;
  const unsigned quadrant = ((unsigned)y&8u)/4+((unsigned)x&8u)/8;
  const uint16_t word = ActionEnvironmentScene_Word(scene,0,tile,quadrant);
  const unsigned px = (word&0x4000u) ? 7-((unsigned)x&7u) : (unsigned)x&7u;
  const unsigned py = (word&0x8000u) ? 7-((unsigned)y&7u) : (unsigned)y&7u;
  return (kBloodpoolPostTimber[word&255u] >> (py*8+px))&1u;
}

static bool BloodpoolTimberMaterial(uint8_t tile, const ActionEnvironmentScene *scene) {
  /* Authored horizontal timber, including moss and support variants. Verify
   * definitions as well as map IDs before interpreting the CHR silhouette. */
  static const struct { uint8_t tile; uint16_t words[4]; } materials[] = {
    {0x51,{0x0A3A,0x0A3B,0x084A,0x084B}}, {0x52,{0x0A77,0x0A3B,0x00FF,0x084B}},
    {0x59,{0x0A8D,0x0A77,0x00FF,0x00FF}}, {0x5A,{0x0A77,0x0A78,0x00FF,0x0863}},
    {0x5B,{0x0A77,0x0A77,0x087A,0x00FF}}, {0x5C,{0x0A77,0x0A7D,0x087A,0x00FF}},
    {0x77,{0x0A77,0x0A77,0x00FF,0x00FF}}, {0x78,{0x0A77,0x0A8C,0x087A,0x00FF}},
    {0x79,{0x0A77,0x0A7D,0x00FF,0x00FF}},
  };
  for (unsigned i = 0; i < sizeof(materials)/sizeof(materials[0]); i++) {
    if (tile != materials[i].tile) continue;
    for (unsigned q = 0; q < 4; q++)
      if (ActionEnvironmentScene_Word(scene,0,tile,q) != materials[i].words[q]) return false;
    return true;
  }
  return false;
}

static void CaptureBloodpoolDetails(ActionBloodpoolDetails *dst, const ActionBgMapView *map,
    const ActionEnvironmentScene *scene, uint16_t sources) {
  dst->valid = dst->timber_count = dst->post_count = 0;
  const int camera = scene->camera_x[0];
  const int x0 = camera > 256 ? (camera-256)&~15 : 0;
  const int x1 = camera+512 < 4096 ? (camera+527)&~15 : 4096;
  for (int y = 0; y < 480; y += 16) for (int x = x0; x < x1; x += 16) {
    uint8_t tile;
    if (!ActionBgMapView_LookupMetatile(map,x,y,&tile) ||
        !BloodpoolTimberMaterial(tile,scene)) continue;
    int start = 0, best_start = 0, best_length = 0;
    for (int p = 0; p <= 16; p++) {
      if (p < 16 && BloodpoolPixel(map,scene,x+p,y,true) &&
          !BloodpoolPixel(map,scene,x+p,y-1,false)) continue;
      if (p-start > best_length) { best_start = start; best_length = p-start; }
      start = p+1;
    }
    if (best_length < 4) continue;
    if (dst->timber_count == kActionBloodpoolMaxTimber) return;
    ActionBloodpoolTimber *edge = &dst->timber[dst->timber_count++];
    *edge = (ActionBloodpoolTimber){.x0=(int16_t)(x+best_start),
      .x1=(int16_t)(x+best_start+best_length),.y=(int16_t)y};
    edge->drip_x = (int16_t)(edge->x0+1+((x/16+y/16*7)%(best_length-2)));
    int bottom = y;
    while (bottom < y+16 && BloodpoolPixel(map,scene,edge->drip_x,bottom,false)) bottom++;
    edge->drip_y = edge->landing_y = (int16_t)bottom;
    if (bottom == y+16) continue; /* Supporting post: no invented underside. */
    int landing = bottom+1;
    while (landing < 480 && !BloodpoolPixel(map,scene,edge->drip_x,landing,false))
      landing++;
    edge->water_landing = landing == 480 && BloodpoolExposedWater(edge->drip_x,sources);
    edge->landing_y = (int16_t)(edge->water_landing ? 488 : landing);
  }
  int start = -1;
  for (int x = x0; x <= x1; x++) {
    uint8_t water_tile = 0;
    const bool post_tile = ActionBgMapView_LookupMetatile(map,x,480,&water_tile) &&
        (water_tile == 0x21 || water_tile == 0x22 || water_tile == 0x5E || water_tile == 0xA0);
    if (x < x1 && BloodpoolExposedWater(x,sources) &&
        post_tile && BloodpoolPostPixel(map,scene,x,479)) {
      if (start < 0) start = x;
      continue;
    }
    if (start < 0) continue;
    if (x-start <= 20 && start > x0 && x < x1) {
      if (dst->post_count == kActionBloodpoolMaxPosts) return;
      int bottom = 480;
      for (int y = 480; y < 512; y++) for (int post_x = start; post_x < x; post_x++)
        if (BloodpoolPostPixel(map,scene,post_x,y)) bottom = y+1;
      dst->posts[dst->post_count++] = (ActionBloodpoolPost){
        .x0 = (int16_t)start, .x1 = (int16_t)x, .y = (int16_t)bottom};
    }
    start = -1;
  }
  dst->valid = true;
}

void CaptureBloodpoolMarshScene(const ActionEnvironmentScene *scene, ActionSceneEffectFrame *dst) {
  if (scene->group != kActRaiserMapGroup_Bloodpool || scene->room != 1 ||
      scene->maps[0].world_width != 4096 || scene->maps[0].world_height != 512 ||
      scene->maps[1].world_width != 256 || scene->maps[1].world_height != 256) return;
  const ActionBgMapView map = scene->maps[0];
  uint8_t bank, timber;
  if (!ActionBgMapView_LookupMetatile(&map, 0, 432, &bank) || bank != 0x8A ||
      !ActionBgMapView_LookupMetatile(&map, 736, 320, &timber) || timber != 0xB9)
    return; /* Reject inherited maps during entry/room changes. */
  uint16_t sources = 0;
  for (unsigned i = 0; i < kBloodpoolWaterSpanCount; i++) {
    bool valid = true;
    for (int x = kBloodpoolWaterSpans[i].left; x < kBloodpoolWaterSpans[i].right; x += 16) {
      uint8_t tile;
      if (!ActionBgMapView_LookupMetatile(&map, x, 480, &tile) || !BloodpoolWaterTile(tile)) {
        valid = false;
        break;
      }
    }
    if (valid) sources |= (uint16_t)(1u << i);
  }
  if (!sources || dst->decoration_count > kActionSceneDecorationMaxInstances - 2) return;
  const int x = scene->camera_x[0] + 128;
  for (unsigned family = 0; family < 2; family++) {
    const ActionEffectInstance effect = {
      .generation = 0xB1000000u | family, .pulse_generation = 0xB1000000u | family,
      .world_x = (int16_t)x, .world_y = 480, .environment_room = 1, .source_mask = sources,
      .age_ticks = scene->clock, .phase_ticks = scene->clock,
      .pulse_ticks = scene->clock,
      .kind = family ? kActionEffect_BloodpoolMist : kActionEffect_BloodpoolWater,
      .phase = kActionEffectPhase_BloodpoolEnvironment,
      .flags = kActionEffectFlag_Visible | kActionEffectFlag_ClipToRect,
      .render_layer = family ? kActionEffectRenderLayer_Bg2HighAlpha :
                              kActionEffectRenderLayer_Bg1HighPlane,
      .projection_plane = family ? kActionEffectProjectionPlane_Bg1 :
                                  kActionEffectProjectionPlane_Bg1High,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,-48,384,32}},
      .clip_rect = {-(float)x,-48,4096.0f-x,32},
    };
    (void)SceneDecorationAppend(dst, &effect);
  }
  CaptureBloodpoolMoon(scene,dst,&map);
  if (!dst->moonlight.valid || dst->decoration_count > kActionSceneDecorationMaxInstances-3)
    return;
  CaptureBloodpoolDetails(&dst->bloodpool,&map,scene,sources);
  CaptureBloodpoolWaterScroll(&dst->bloodpool,scene);
  const ActionEffectInstance moon = dst->decorations[dst->decoration_count-2];
  for (unsigned family = 0; family < 3; family++) {
    ActionEffectInstance detail = moon;
    detail.kind = (uint8_t)(kActionEffect_BloodpoolTimber+family);
    detail.generation = detail.pulse_generation = 0xB1000004u+family;
    detail.world_x = (int16_t)x;
    detail.world_y = 0;
    detail.source_mask = sources;
    detail.render_layer = family == 0 ? kActionEffectRenderLayer_Bg1Plane :
        family == 1 ? kActionEffectRenderLayer_Bg2HighAlpha : kActionEffectRenderLayer_Bg2Alpha;
    detail.projection_plane = family == 2 ? kActionEffectProjectionPlane_Bg2 :
        kActionEffectProjectionPlane_Bg1;
    detail.geometry.data.rect = detail.clip_rect = (ActionEffectLocalRect){-384,0,384,512};
    if (family == 2) {
detail.world_x = 112;
      detail.world_y = 62;
      detail.geometry.data.rect = detail.clip_rect = (ActionEffectLocalRect){-100,-40,100,44};
    }
    (void)SceneDecorationAppend(dst,&detail);
  }
}


static bool CastleTileIs(const ActionBgMapView *map, int x, int y, uint8_t expected) {
  uint8_t tile;
  return ActionBgMapView_LookupMetatile(map,x,y,&tile) && tile == expected;
}

static void CaptureCastleWater(ActionSceneEffectFrame *dst, const ActionEnvironmentScene *scene,
    const ActionEffectInstance *castle) {
  const ActionBgMapView water = scene->maps[1];
  /* Room 5 blends BG2 water into the resolved BG1 masonry. Both maps scroll
   * together; light the resolved scenery rather than an occluded rear plane. */
  if (scene->camera_x[0] != scene->camera_x[1] ||
      scene->camera_y[0] != scene->camera_y[1]) return;
  static const uint16_t surface[] = {0x0470,0x0471,0x0402,0x0403};
  for (unsigned q = 0; q < 4; q++)
    if (ActionEnvironmentScene_Word(scene,1,0x15,q) != surface[q]) return;
  ActionEffectInstance effect = *castle;
  effect.kind = kActionEffect_CastleWater;
  effect.render_layer = kActionEffectRenderLayer_Bg1Plane;
  effect.projection_plane = kActionEffectProjectionPlane_Bg1;
  effect.world_x = scene->camera_x[0]+128;
  effect.world_y = kActionCastleWaterSurface;
  effect.geometry.data.rect = (ActionEffectLocalRect){-384,0,384,16};
  effect.clip_rect = (ActionEffectLocalRect){kActionCastleWaterLeft-effect.world_x,0,
      kActionCastleWaterRight-effect.world_x,16};
  effect.source_mask = 0;
  for (unsigned strip = 0; strip < kActionCastleWaterStripCount; strip++) {
    const int left = kActionCastleWaterLeft+strip*kActionCastleWaterStripWidth;
    bool valid = true;
    for (int x = left; valid && x < left+kActionCastleWaterStripWidth; x += 16)
      valid = CastleTileIs(&water,x,kActionCastleWaterSurface,0x15) &&
          CastleTileIs(&water,x,kActionCastleWaterSurface-16,0);
    if (valid) effect.source_mask |= (uint16_t)(1u<<strip);
  }
  if (effect.source_mask) (void)SceneDecorationAppend(dst,&effect);
}

void CaptureBloodpoolCastleScene(const ActionEnvironmentScene *scene, ActionSceneEffectFrame *dst) {
  const unsigned room = scene->room;
  if (scene->group != kActRaiserMapGroup_Bloodpool || room < 2 || room > 8 ||
      dst->decoration_count > kActionSceneDecorationMaxInstances-3) return;
  static const uint16_t dimensions[][4] = {
    {768,512,256,256}, {1024,1024,256,256}, {512,512,256,256},
    {1792,1024,1792,1024}, {768,256,256,256}, {1024,1024,256,256}, {256,256,256,256},
  };
  const uint16_t *d = dimensions[room-2];
  if (scene->maps[0].world_width != d[0] || scene->maps[0].world_height != d[1] ||
      scene->maps[1].world_width != d[2] || scene->maps[1].world_height != d[3]) return;
  const unsigned width = scene->maps[0].world_width;
  const unsigned height = scene->maps[0].world_height;
  const ActionBgMapView map = scene->maps[0];
  static const uint8_t witnesses[][2] = {
    {0x08,0x08}, {0x17,0x23}, {0x09,0xFA}, {0x09,0xA3},
    {0x09,0x23}, {0x09,0x09}, {0x77,0x09},
  };
  if (!CastleTileIs(&map,0,(int)height-16,witnesses[room-2][0]) ||
      !CastleTileIs(&map,(int)width-16,0,witnesses[room-2][1])) return;
  /* Check decoded shared window definitions too: same map ids with replaced
   * graphics must not silently become light emitters. No ROM address coupling. */
  static const uint16_t window[] = {0x04EE,0x44EE,0x04FE,0x44FE};
  for (unsigned q = 0; q < 4; q++)
    if (ActionEnvironmentScene_Word(scene,0,0x44,q) != window[q]) return;
  const int x = scene->camera_x[0]+128;
  const int y = scene->camera_y[0]-160;
  ActionEffectInstance effect = {
    .generation = 0xCA000000u|room, .pulse_generation = 0xCB000000u|room,
    .world_x = (int16_t)x, .world_y = (int16_t)y, .environment_room = (uint16_t)room,
    .age_ticks = scene->clock, .phase_ticks = scene->clock,
    .pulse_ticks = scene->clock,
    .kind = kActionEffect_CastleLight, .phase = kActionEffectPhase_CastleEnvironment,
    .flags = kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
    .render_layer = kActionEffectRenderLayer_Bg1Plane,
    .projection_plane = kActionEffectProjectionPlane_Bg1,
    .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-384,0,384,544}},
    .clip_rect = {-x,-y,(float)width-x,(float)height-y},
  };
  unsigned index = 0;
  for (unsigned i = 0; i < kActionCastleSourceCount; i++) {
    const ActionCastleSource *s = &kActionCastleSources[i];
    if (s->room != room) continue;
    if (index >= 16) return;
    if (CastleTileIs(&map,s->check_x,s->check_y,s->top_tile) &&
        CastleTileIs(&map,s->check_x,s->check_y+16,s->below_tile) &&
        (s->kind != kActionCastleSource_Window || CastleTileIs(&map,s->x,s->sill-1,s->sill_tile)))
      effect.source_mask |= (uint16_t)(1u<<index);
    index++;
  }
  if (effect.source_mask) (void)SceneDecorationAppend(dst,&effect);

  /* Low haze has an actual supporting surface across its full span. Never
   * reuse a single floor height over a pit or newly replaced collision map. */
  static const uint16_t floors[][3] = {
    {32,608,480}, {0,0,0}, {176,336,464}, {928,1504,976},
    {160,640,208}, {352,816,208}, {32,224,224},
  };
  const uint16_t *floor = floors[room-2];
  bool supported = floor[2] != 0;
  for (int fx = floor[0]; supported && fx < floor[1]; fx += 16) {
    uint8_t above, below;
    supported = ActionBgMapView_LookupMetatile(&map,fx,floor[2]-1,&above) &&
        ActionBgMapView_LookupMetatile(&map,fx,floor[2],&below) &&
        scene->collision[below] == 15 && scene->collision[above] == 0 &&
        above != 0x03 && above != 0x04 && above != 0x46 && above != 0x4E;
  }
  if (supported) {
    ActionEffectInstance mist = effect;
    mist.kind = kActionEffect_CastleMist;
    mist.render_layer = kActionEffectRenderLayer_Bg1Mist;
    mist.world_x = (int16_t)floor[0];
    mist.world_y = (int16_t)floor[2];
    mist.source_mask = 0;
    mist.geometry.data.rect = (ActionEffectLocalRect){0,-18,floor[1]-floor[0],0};
    mist.clip_rect = mist.geometry.data.rect;
    (void)SceneDecorationAppend(dst,&mist);
  }
  if (room == 5) {
    CaptureCastleWater(dst,scene,&effect);
    return; /* Interior BG2 has water, but no moon/sky artwork. */
  }
  const ActionBgMapView sky = scene->maps[1];
  if (!CastleTileIs(&sky,112,48,room < 6 ? 0x3C : 0x10) ||
      !CastleTileIs(&sky,128,64,room < 6 ? 0x45 : 0x05)) return;
  effect.kind = kActionEffect_CastleSky;
  effect.render_layer = kActionEffectRenderLayer_Bg2Plane;
  effect.projection_plane = kActionEffectProjectionPlane_Bg2;
  effect.world_x = room < 6 ? 112 : 128;
  effect.world_y = room < 6 ? 62 : 48;
  effect.source_mask = 0;
  effect.geometry.data.rect = (ActionEffectLocalRect){-144,-48,144,208};
  effect.clip_rect = (ActionEffectLocalRect){-effect.world_x,-effect.world_y,
      256-effect.world_x,256-effect.world_y};
  if (room == 2 || room == 6 || room == 7) {
    /* Exterior rays spread beyond the moon's native 256px sky map, just as
     * in Act 1. The presentation plane supplies the actual visible bounds. */
    effect.geometry.data.rect = effect.clip_rect =
        (ActionEffectLocalRect){-384,-48,384,194};
  }
  (void)SceneDecorationAppend(dst,&effect);
}

bool IsBloodpoolMarsh(const uint8_t *wram, size_t size) {
  return wram && size > kActRaiserWram_Bg2Height + 1 &&
      Read8(wram, size, kActRaiserWram_MapGroup) == kActRaiserMapGroup_Bloodpool &&
      Read8(wram, size, kActRaiserWram_CurrentMap) == 1 &&
      Read16(wram, size, kActRaiserWram_Bg1Width) == 4096 &&
      Read16(wram, size, kActRaiserWram_Bg1Height) == 512 &&
      Read16(wram, size, kActRaiserWram_Bg2Width) == 256 &&
      Read16(wram, size, kActRaiserWram_Bg2Height) == 256;
}

unsigned BloodpoolCastleRoom(const uint8_t *ram, size_t size) {
  if (!ram || Read8(ram,size,kActRaiserWram_MapGroup) != kActRaiserMapGroup_Bloodpool)
    return 0;
  const unsigned room = Read8(ram,size,kActRaiserWram_CurrentMap);
  if (room < 2 || room > 8) return 0;
  static const uint16_t dimensions[][4] = {
    {768,512,256,256}, {1024,1024,256,256}, {512,512,256,256},
    {1792,1024,1792,1024}, {768,256,256,256}, {1024,1024,256,256}, {256,256,256,256},
  };
  const uint16_t *d = dimensions[room-2];
  return Read16(ram,size,kActRaiserWram_Bg1Width) == d[0] &&
      Read16(ram,size,kActRaiserWram_Bg1Height) == d[1] &&
      Read16(ram,size,kActRaiserWram_Bg2Width) == d[2] &&
      Read16(ram,size,kActRaiserWram_Bg2Height) == d[3] ? room : 0;
}

void CaptureBloodpoolMarsh(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *ram, size_t size) {
  ActionEnvironmentScene scene;
  if (ActionEnvironmentScene_FromWram(&scene,ram,size,observer->scene_clock) &&
      observer->scene_map_number == scene.room) CaptureBloodpoolMarshScene(&scene,dst);
}

void CaptureBloodpoolCastle(ActionEffectObserver *observer, ActionSceneEffectFrame *dst,
    const uint8_t *ram, size_t size) {
  ActionEnvironmentScene scene;
  if (ActionEnvironmentScene_FromWram(&scene,ram,size,observer->scene_clock) &&
      observer->scene_map_number == scene.room) CaptureBloodpoolCastleScene(&scene,dst);
}
