#include "action_environment_capture_internal.h"

static bool CastleTileIs(const ActionBgMapView *map, int x, int y, uint8_t expected) {
  uint8_t tile;
  return ActionBgMapView_LookupMetatile(map,x,y,&tile) && tile == expected;
}

static void CaptureCastleWater(ActionSceneEffectFrame *dst, const ActionEnvironmentScene *scene,
    const ActionEffectInstance *castle,const ActionCastleField *f) {
  const ActionBgMapView water = scene->maps[1];
  /* Room 5 blends BG2 water into the resolved BG1 masonry. Both maps scroll
   * together; light the resolved scenery rather than an occluded rear plane. */
  if (scene->camera_x[0] != scene->camera_x[1] ||
      scene->camera_y[0] != scene->camera_y[1]) return;
  for (unsigned q = 0; q < 4; q++)
    if (ActionEnvironmentScene_Word(scene,1,(unsigned)f->WaterMaterial[0],q) != f->WaterMaterial[q+1]) return;
  ActionEffectInstance effect = *castle;
  effect.kind = kActionEffect_CastleWater;
  effect.render_layer = kActionEffectRenderLayer_Bg1Plane;
  effect.projection_plane = kActionEffectProjectionPlane_Bg1;
  effect.world_x = scene->camera_x[0]+128;
  effect.world_y = f->WaterSurface[1];
  effect.geometry.data.rect = (ActionEffectLocalRect){-384,0,384,16};
  effect.clip_rect = (ActionEffectLocalRect){f->WaterSurface[0]-effect.world_x,0,
      (f->WaterSurface[0]+f->WaterSurface[2]*f->WaterSurface[3])-effect.world_x,16};
  effect.source_mask = 0;
  for (unsigned strip = 0; strip < (unsigned)f->WaterSurface[3]; strip++) {
    const int left = f->WaterSurface[0]+strip*f->WaterSurface[2];
    bool valid = true;
    for (int x = left; valid && x < left+f->WaterSurface[2]; x += 16)
      valid = CastleTileIs(&water,x,f->WaterSurface[1],(uint8_t)f->WaterTiles[0]) &&
          CastleTileIs(&water,x,f->WaterSurface[1]-16,(uint8_t)f->WaterTiles[1]);
    if (valid) effect.source_mask |= (uint16_t)(1u<<strip);
  }
  if (effect.source_mask) (void)SceneDecorationAppend(dst,&effect);
}

void ActionCastleField_Capture(const ActionEnvironmentScene *scene,ActionSceneEffectFrame *dst,
    const ActionCastleField *f,uint32_t source) {
  if(!scene||!dst||!f||dst->decoration_overflow||dst->decoration_count>kActionSceneDecorationMaxInstances-4)return;
  for(unsigned bg=0;bg<2;++bg)if((f->Dimensions[bg*2]&&scene->maps[bg].world_width!=f->Dimensions[bg*2])||
    (f->Dimensions[bg*2+1]&&scene->maps[bg].world_height!=f->Dimensions[bg*2+1]))return;
  const float *witness[]={f->Witness1,f->Witness2};
  for(unsigned i=0;i<(unsigned)f->WitnessCount[0];++i){const float *w=witness[i];
    if(!CastleTileIs(&scene->maps[(unsigned)w[0]],w[1],w[2],(uint8_t)w[3]))return;}
  for(unsigned q=0;q<4;++q)if(ActionEnvironmentScene_Word(scene,0,(unsigned)f->Material[0],q)!=f->Material[q+1])return;
  dst->castle_field=*f;dst->castle_field_valid=true;
  const unsigned components=(unsigned)f->Components[0];
  const unsigned room=scene->room;
  const unsigned width=scene->maps[0].world_width,height=scene->maps[0].world_height;
  const ActionBgMapView map=scene->maps[0];
  const int x = scene->camera_x[0]+128;
  const int y = scene->camera_y[0]-160;
  ActionEffectInstance effect = {
    .generation = source, .pulse_generation = source^0x01000000u,
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
  effect.tuning.dim_receivers_set=1;effect.tuning.light_receivers_set=f->Receivers[1]<8;
  effect.tuning.dim_receivers=(uint8_t)f->Receivers[0];effect.tuning.light_receivers=(uint8_t)f->Receivers[1];
  unsigned index = 0;
  for (unsigned i = 0; i < (unsigned)f->SourceCount[0]; i++) {
    const ActionCastleSource *s = &f->sources[i];
    if (index >= 16) return;
    if (CastleTileIs(&map,s->check_x,s->check_y,s->top_tile) &&
        CastleTileIs(&map,s->check_x,s->check_y+16,s->below_tile) &&
        (s->kind != kActionCastleSource_Window || CastleTileIs(&map,s->x,s->sill-1,s->sill_tile)))
      effect.source_mask |= (uint16_t)(1u<<index);
    index++;
  }
  if ((components&1u)&&effect.source_mask) (void)SceneDecorationAppend(dst,&effect);

  /* Low haze has an actual supporting surface across its full span. Never
   * reuse a single floor height over a pit or newly replaced collision map. */
  const float *floor=f->Floor;
  bool supported = (components&2u)&&floor[2] != 0;
  for (int fx = floor[0]; supported && fx < floor[1]; fx += 16) {
    uint8_t above, below;
    supported = ActionBgMapView_LookupMetatile(&map,fx,floor[2]-1,&above) &&
        ActionBgMapView_LookupMetatile(&map,fx,floor[2],&below) &&
        scene->collision[below] == 15 && scene->collision[above] == 0 &&
        above != f->FloorExclude[0] && above != f->FloorExclude[1] && above != f->FloorExclude[2] && above != f->FloorExclude[3];
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
  if(components&8u)CaptureCastleWater(dst,scene,&effect,f);
  if(!(components&4u))return;
  const float *sky_witness[]={f->SkyWitness1,f->SkyWitness2};
  for(unsigned i=0;i<2;++i){const float *w=sky_witness[i];
    if(!CastleTileIs(&scene->maps[(unsigned)w[0]],w[1],w[2],(uint8_t)w[3]))return;}
  effect.kind = kActionEffect_CastleSky;
  effect.render_layer = kActionEffectRenderLayer_Bg2Plane;
  effect.projection_plane = kActionEffectProjectionPlane_Bg2;
  effect.world_x=(int16_t)f->SkyAnchor[0];effect.world_y=(int16_t)f->SkyAnchor[1];effect.source_mask=0;
  effect.geometry.data.rect=(ActionEffectLocalRect){f->SkyWindow[0],f->SkyWindow[1],f->SkyWindow[2],f->SkyWindow[3]};
  effect.clip_rect=(ActionEffectLocalRect){f->SkyClip[0],f->SkyClip[1],f->SkyClip[2],f->SkyClip[3]};
  (void)SceneDecorationAppend(dst,&effect);
}

