/* Read-only material/contact observation; visual data belongs to the field. */
#include "action_environment_capture_internal.h"
#include "action_bloodpool_occluders.h"
static bool TileIn(const float *tiles,unsigned count,uint8_t tile) {
  for(unsigned i=0;i<count;++i)if((unsigned)tiles[i]==tile)return true;
  return false;
}
static bool BloodpoolPixel(const ActionBgMapView *map, const ActionEnvironmentScene *scene, int x, int y, bool low_only) {
  uint8_t tile;
  if (!ActionBgMapView_LookupMetatile(map,x,y,&tile)) return false;
  const unsigned quadrant = ((unsigned)y&8u)/4+((unsigned)x&8u)/8;
  const uint16_t word = (ActionEnvironmentScene_Word(scene,0,tile,quadrant)&scene->word_mask[0])|scene->attributes[0];
  if (low_only && (word&0x2000u)) return false;
  const unsigned px = (word&0x4000u) ? 7-((unsigned)x&7u) : (unsigned)x&7u;
  const unsigned py = (word&0x8000u) ? 7-((unsigned)y&7u) : (unsigned)y&7u;
  return (kBloodpoolTileOpacity[word&255u] >> (py*8+px))&1u;
}

static bool BloodpoolExposedWater(const ActionMarshField *m,int x, uint16_t sources) {
  for (unsigned i = 0; i < (unsigned)m->SpanCount[0]; i++)
    if ((sources&(1u<<i)) && x >= m->spans[i][0]+m->Surface[3] &&
        x < m->spans[i][1]-m->Surface[3]) return true;
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

static bool BloodpoolTimberMaterial(const ActionMarshField *m,uint8_t tile,const ActionEnvironmentScene *scene) {
  for(unsigned i=0;i<9;++i) {
    const float *material=m->materials[i];
    if(tile!=(unsigned)material[0])continue;
    for(unsigned q=0;q<4;++q)
      if(ActionEnvironmentScene_Word(scene,0,tile,q)!=(unsigned)material[q+1])return false;
    return true;
  }
  return false;
}

static void CaptureBloodpoolDetails(const ActionMarshField *m,ActionBloodpoolDetails *dst, const ActionBgMapView *map,
    const ActionEnvironmentScene *scene, uint16_t sources) {
  dst->valid = dst->timber_count = dst->post_count = 0;
  const int camera = scene->camera_x[0];
  const int x0 = camera > m->Bounds[0]+256 ? (camera-256)&~15 : (int)m->Bounds[0];
  const int x1 = camera+512 < m->Bounds[2] ? (camera+527)&~15 : (int)m->Bounds[2];
  for (int y = (int)m->Bounds[1]; y < (int)m->Surface[0]; y += 16) for (int x = x0; x < x1; x += 16) {
    uint8_t tile;
    if (!ActionBgMapView_LookupMetatile(map,x,y,&tile) ||
        !BloodpoolTimberMaterial(m,tile,scene)) continue;
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
    while (landing < (int)m->Surface[0] && !BloodpoolPixel(map,scene,edge->drip_x,landing,false))
      landing++;
    edge->water_landing = landing == (int)m->Surface[0] && BloodpoolExposedWater(m,edge->drip_x,sources);
    edge->landing_y = (int16_t)(edge->water_landing ? (int)m->Surface[1] : landing);
  }
  int start = -1;
  for (int x = x0; x <= x1; x++) {
    uint8_t water_tile = 0;
    const bool post_tile = ActionBgMapView_LookupMetatile(map,x,(int)m->Surface[0],&water_tile) &&
        TileIn(m->PostTiles,4,water_tile);
    if (x < x1 && BloodpoolExposedWater(m,x,sources) &&
        post_tile && BloodpoolPostPixel(map,scene,x,(int)m->Surface[0]-1)) {
      if (start < 0) start = x;
      continue;
    }
    if (start < 0) continue;
    if (x-start <= 20 && start > x0 && x < x1) {
      if (dst->post_count == kActionBloodpoolMaxPosts) return;
      int bottom = (int)m->Surface[0];
      for (int y = (int)m->Surface[0]; y <= (int)m->Surface[2]; y++) for (int post_x = start; post_x < x; post_x++)
        if (BloodpoolPostPixel(map,scene,post_x,y)) bottom = y+1;
      dst->posts[dst->post_count++] = (ActionBloodpoolPost){
        .x0 = (int16_t)start, .x1 = (int16_t)x, .y = (int16_t)bottom};
    }
    start = -1;
  }
  dst->valid = true;
}


static bool Witnesses(const ActionEnvironmentScene *scene,const ActionMarshField *m) {
  for(unsigned bg=0;bg<2;++bg)
    if((m->Dimensions[bg*2]&&scene->maps[bg].world_width!=m->Dimensions[bg*2])||
       (m->Dimensions[bg*2+1]&&scene->maps[bg].world_height!=m->Dimensions[bg*2+1]))return false;
  const float *witness[]={m->Witness1,m->Witness2};
  for(unsigned i=0;i<(unsigned)m->WitnessCount[0];++i) {
    const float *w=witness[i];uint8_t tile;
    if(!ActionBgMapView_LookupMetatile(&scene->maps[(unsigned)w[0]],(int)w[1],(int)w[2],&tile)||tile!=(unsigned)w[3])return false;
  }
  return true;
}
static bool DetailReady(const ActionEnvironmentScene *scene,const ActionMarshField *m) {
  if(!scene->metatiles[0]||scene->word_mask[0]!=(unsigned)m->DetailWords[0]||scene->attributes[0]!=(unsigned)m->DetailWords[1])return false;
  const float *witness[]={m->DetailWitness1,m->DetailWitness2};
  for(unsigned i=0;i<(unsigned)m->DetailWitnessCount[0];++i) {
    const float *w=witness[i];uint8_t tile;
    if(!ActionBgMapView_LookupMetatile(&scene->maps[(unsigned)w[0]],(int)w[1],(int)w[2],&tile)||tile!=(unsigned)w[3])return false;
  }
  return true;
}
void ActionMarshField_Capture(const ActionEnvironmentScene *scene,ActionSceneEffectFrame *dst,
    const ActionMarshField *m,uint32_t source) {
  if(!scene||!dst||!m||dst->decoration_overflow||!Witnesses(scene,m))return;
  const unsigned components=(unsigned)m->Components[0];
  if(!components){dst->bloodpool.field=*m;dst->bloodpool.field_valid=true;return;}
  const ActionBgMapView *map=&scene->maps[0];uint16_t sources=0;
  for(unsigned i=0;i<(unsigned)m->SpanCount[0];++i) {
    bool valid=true;
    for(int x=(int)m->spans[i][0];valid&&x<(int)m->spans[i][1];x+=16) {
      uint8_t tile;
      valid=ActionBgMapView_LookupMetatile(map,x,(int)m->Surface[0],&tile)&&TileIn(m->WaterTiles,9,tile);
    }
    if(valid)sources|=(uint16_t)(1u<<i);
  }
  if(!sources||dst->decoration_count>kActionSceneDecorationMaxInstances-4)return;
  const bool details=DetailReady(scene,m);
  if(details)CaptureBloodpoolDetails(m,&dst->bloodpool,map,scene,sources);
  dst->bloodpool.field=*m;dst->bloodpool.field_valid=true;
  const int x=scene->camera_x[0]+128;
  const unsigned kinds[]={kActionEffect_BloodpoolWater,kActionEffect_BloodpoolMist,kActionEffect_BloodpoolTimber,kActionEffect_BloodpoolAir};
  for(unsigned i=0;i<4;++i) {
    if(!(components&(1u<<i))||(i>=2&&!details))continue;
    ActionEffectInstance e={.generation=source+(i<2?i:i+2),.pulse_generation=source+(i<2?i:i+2),
      .world_x=(int16_t)x,.world_y=i<2?(int16_t)m->Surface[0]:0,.source_mask=sources,
      .environment_room=scene->room,.age_ticks=scene->clock,.phase_ticks=scene->clock,.pulse_ticks=scene->clock,
      .kind=kinds[i],.phase=kActionEffectPhase_BloodpoolEnvironment,
      .flags=kActionEffectFlag_Visible|kActionEffectFlag_ClipToRect,
      .render_layer=i==0?kActionEffectRenderLayer_Bg1HighPlane:i==2?kActionEffectRenderLayer_Bg1Plane:kActionEffectRenderLayer_Bg2HighAlpha,
      .projection_plane=i==0?kActionEffectProjectionPlane_Bg1High:kActionEffectProjectionPlane_Bg1,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={-384,i<2?-48:m->Bounds[1],384,i<2?32:m->Bounds[3]}},
      .clip_rect={i<2?m->Bounds[0]-x:-384,i<2?-48:m->Bounds[1],i<2?m->Bounds[2]-x:384,i<2?32:m->Bounds[3]},
    };
    /* Water/mist precede the moon; timber/air follow its two additive records. */
    unsigned insert=dst->decoration_count;
    for(unsigned j=0;j<dst->decoration_count;++j) {
      const unsigned kind=dst->decorations[j].kind;
      if((i<2&&(kind==kActionEffect_BloodpoolMoonlight||kind==kActionEffect_BloodpoolMoonReflection||kind==kActionEffect_BloodpoolTimber||kind==kActionEffect_BloodpoolAir))||kind==kActionEffect_BloodpoolCloud){insert=j;break;}
    }
    memmove(dst->decorations+insert+1,dst->decorations+insert,(dst->decoration_count-insert)*sizeof(e));
    dst->decorations[insert]=e;++dst->decoration_count;++dst->decoration_visible_count;
  }
  /* A replacement can be captured after the linked moon field. Refresh its
   * shoreline witness mask just as moon capture does when it runs second. */
  for(unsigned i=0;i<dst->decoration_count;++i)
    if(dst->decorations[i].kind==kActionEffect_BloodpoolCloud)
      dst->decorations[i].source_mask=(components&1u)?sources:0;
}
