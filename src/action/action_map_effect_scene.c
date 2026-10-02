#include "action_environment_capture_internal.h"
#include "action_map_effect_sources.h"
#include "action_bg_plan.h"
#include <math.h>

static uint32_t SurfaceIdentity(const ActionSurfaceField *f,unsigned offset){
  return ((uint32_t)f->Identity[offset]<<16)|(uint32_t)f->Identity[offset+1];
}
static bool AppendSurface(ActionSceneEffectFrame *dst,const ActionSurfaceField *f,ActionEffectInstance *e){
  e->render_layer=(uint8_t)f->Projection[1];e->projection_plane=(uint8_t)f->Projection[0];
  if(f->Receivers[0]<8){e->tuning.light_receivers_set=1;e->tuning.light_receivers=(uint8_t)f->Receivers[0];}
  return SceneDecorationAppend(dst,e);
}

static void CaptureWallTorches(ActionSceneEffectFrame *dst,const ActionEnvironmentScene *scene){
  if(!scene->suppress_default_glow_field)
    ActionGlowField_Capture(scene,dst,ActionGlowField_Bundled(scene->group,scene->room),0x54000000u);
}

static void CaptureAitosLavaPits(ActionSceneEffectFrame *dst,
                                 const ActionEnvironmentScene *scene,const ActionSurfaceField *f) {
  if (!dst || !f || f->Mode[0]) return;
  const ActionBgMapView map = scene->maps[0];
  if (!map.map) return;
  const int camera_x = scene->camera_x[0];
  const int camera_y = scene->camera_y[0];
  SceneBgScanBounds bounds;
  if (!SceneBgScanBounds_InitWindow(&bounds,&map,camera_x,camera_y,(int)f->Scan[0],(int)f->Scan[1],false))
    return;
  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      uint8_t metatile = 0;
      if (!ActionBgMapView_LookupMetatile(
               &map, (int)x, (int)y, &metatile) ||
          metatile != (unsigned)f->MapRule[0])
        continue;

      unsigned middle_cells = 0;
      unsigned total_cells = 0;
      for (unsigned step = 1; step <= (unsigned)f->MapRule[5] + 1;
           step++) {
        const unsigned cell_x = x + step * kActionBgMetatilePixels;
        if (cell_x < x ||
            !ActionBgMapView_LookupMetatile(
                &map, (int)cell_x, (int)y, &metatile))
          break;
        if (step <= (unsigned)f->MapRule[5] &&
            metatile == (unsigned)f->MapRule[1]) {
          middle_cells++;
          continue;
        }
        if (middle_cells && metatile == (unsigned)f->MapRule[2])
          total_cells = step + 1;
        break;
      }
      if (!total_cells) continue;

      bool bubbles_valid = true;
      unsigned bubble_rows = 1;
      const uint8_t kBubbleRows[] = {
        (unsigned)f->MapRule[3], (unsigned)f->MapRule[4],
      };
      const bool has_second_bubble_row =
          map.world_height >= 3u * kActionBgMetatilePixels && y <= map.world_height - 3u * kActionBgMetatilePixels;
      if (has_second_bubble_row) bubble_rows++;
      for (unsigned row = 0; row < bubble_rows; row++) {
        for (unsigned cell = 0; cell < total_cells; cell++) {
          const unsigned cell_x = x + cell * kActionBgMetatilePixels;
          const unsigned cell_y = y + (row + 1u) * kActionBgMetatilePixels;
          if (cell_x < x || cell_y < y ||
              !ActionBgMapView_LookupMetatile(
                  &map, (int)cell_x, (int)cell_y, &metatile) ||
              metatile != kBubbleRows[row]) {
            bubbles_valid = false;
            break;
          }
        }
        if (!bubbles_valid) break;
      }
      if (!bubbles_valid) continue;

      const unsigned width = total_cells * kActionBgMetatilePixels;
      const unsigned height = bubble_rows * kActionBgMetatilePixels;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(x / kActionBgMetatilePixels);
      const float half_width = (float)width * 0.5f;
      const float half_height = (float)height * 0.5f;
      ActionEffectInstance effect = {
        .generation = SurfaceIdentity(f,0) ^ identity,
        .pulse_generation = SurfaceIdentity(f,2) ^ identity,
        .world_x = (int16_t)(x + width / 2u + (int)f->Anchor[0]),
        .world_y = (int16_t)(
            y + (int)f->Anchor[1] + height / 2u),
        .left_extent = (uint16_t)(width / 2u),
        .top_extent = (uint16_t)(height / 2u),
        .right_extent = (uint16_t)(width / 2u),
        .bottom_extent = (uint16_t)(height / 2u),
        .age_ticks = scene->clock,
        .phase_ticks = scene->clock,
        .pulse_ticks = scene->clock,
        .kind = kActionEffect_AitosLavaPit,
        .phase = kActionEffectPhase_AitosLavaPit,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_WorldOverlay,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-half_width, -half_height,
                        half_width, half_height},
        },
      };
      AppendSurface(dst,f,&effect);
    }
  }
}

static bool IsAitosSideLavaAnimatedCell(uint8_t metatile,const ActionSurfaceField *f) {
  return (metatile >= (unsigned)f->MapRule[2] &&
          metatile <= (unsigned)f->MapRule[3]) ||
      metatile == (unsigned)f->MapRule[4];
}

static bool IsAitosSideLavaBankPair(uint8_t left, uint8_t right,const ActionSurfaceField *f) {
  return (left == (unsigned)f->MapRule[7] && right == (unsigned)f->MapRule[8]) ||
      (left == (unsigned)f->MapRule[9] && right == (unsigned)f->MapRule[10]) ||
      (left == (unsigned)f->MapRule[11] && right == (unsigned)f->MapRule[12]);
}

/* Act 2 turns the isometric pit mouths into broad side-on lakes. Their exact
 * semantic is a maximal $01 lip run, one of the measured bank pairs, an
 * animated/transparent surface row immediately above, and lava body below.
 * Lighting is anchored to the lip rather than the red volume: using the full
 * lake depth as an emitter would put sparks hundreds of pixels underwater. */
static void CaptureAitosSideLavaReservoirs(
    ActionSceneEffectFrame *dst, const ActionEnvironmentScene *scene,const ActionSurfaceField *f) {
  if (!dst || !f || f->Mode[0]) return;
  const ActionBgMapView map = scene->maps[0];
  if (!map.map) return;
  const int camera_x = scene->camera_x[0];
  const int camera_y = scene->camera_y[0];
  SceneBgScanBounds bounds;
  if (!SceneBgScanBounds_InitWindow(&bounds,&map,camera_x,camera_y,(int)f->Scan[0],(int)f->Scan[1],false))
    return;

  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    if (y < kActionBgMetatilePixels ||
        y + kActionBgMetatilePixels >= map.world_height)
      continue;
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      if (x < kActionBgMetatilePixels) continue;
      uint8_t metatile = 0, left_bank = 0;
      if (!ActionBgMapView_LookupMetatile(
               &map, (int)x, (int)y, &metatile) ||
          metatile != (unsigned)f->MapRule[0])
        continue;

      /* If the camera window begins inside a very wide lake, walk left to its
       * authentic bank once. Later in-window cells see a lip immediately to
       * their left and skip as duplicates. Without this, map $06's 640px lake
       * lost all heat whenever its left bank sat beyond the scan margin. */
      unsigned run_x = x;
      unsigned walked = 0;
      if (x == bounds.x0) {
        while (run_x >= 2u * kActionBgMetatilePixels &&
               walked < (unsigned)f->MapRule[5]) {
          uint8_t previous = 0;
          if (!ActionBgMapView_LookupMetatile(
                  &map, (int)(run_x - kActionBgMetatilePixels),
                  (int)y, &previous) ||
              previous != (unsigned)f->MapRule[0])
            break;
          run_x -= kActionBgMetatilePixels;
          walked++;
        }
        if (walked == (unsigned)f->MapRule[5]) continue;
      }
      if (!ActionBgMapView_LookupMetatile(
               &map, (int)(run_x - kActionBgMetatilePixels),
               (int)y, &left_bank) ||
          left_bank == (unsigned)f->MapRule[0])
        continue;

      unsigned cells = 0;
      bool surface_valid = true;
      bool has_animated_surface = false;
      bool has_lava_body = false;
      for (; cells < (unsigned)f->MapRule[5]; cells++) {
        const unsigned cell_x = run_x + cells * kActionBgMetatilePixels;
        if (cell_x < run_x || cell_x >= map.world_width ||
            !ActionBgMapView_LookupMetatile(
                &map, (int)cell_x, (int)y, &metatile) ||
            metatile != (unsigned)f->MapRule[0])
          break;
        uint8_t surface = 0, body = 0;
        if (!ActionBgMapView_LookupMetatile(
                 &map, (int)cell_x,
                 (int)(y - kActionBgMetatilePixels), &surface) ||
            (surface != 0 && !IsAitosSideLavaAnimatedCell(surface,f)) ||
            !ActionBgMapView_LookupMetatile(
                 &map, (int)cell_x,
                 (int)(y + kActionBgMetatilePixels), &body)) {
          surface_valid = false;
          break;
        }
        has_animated_surface |= IsAitosSideLavaAnimatedCell(surface,f);
        has_lava_body |= body == (unsigned)f->MapRule[1];
      }
      if (!surface_valid || cells < (unsigned)f->MapRule[6] || !has_animated_surface ||
          !has_lava_body || cells == (unsigned)f->MapRule[5])
        continue;
      const unsigned right_x = run_x + cells * kActionBgMetatilePixels;
      uint8_t right_bank = 0;
      if (right_x < run_x || right_x >= map.world_width ||
          !ActionBgMapView_LookupMetatile(
              &map, (int)right_x, (int)y, &right_bank) ||
          !IsAitosSideLavaBankPair(left_bank, right_bank,f))
        continue;

      const unsigned width = cells * kActionBgMetatilePixels;
      const float half_width = (float)width * 0.5f;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(run_x / kActionBgMetatilePixels);
      ActionEffectInstance effect = {
        .generation = SurfaceIdentity(f,0) ^ identity,
        .pulse_generation = SurfaceIdentity(f,2) ^ identity,
        .world_x = (int16_t)(run_x + width / 2u + (int)f->Anchor[0]),
        /* The visible orange lip occupies the top half of the $01 row. */
        .world_y = (int16_t)(y + (int)f->Anchor[1]),
        .left_extent = (uint16_t)(width / 2u),
        .top_extent = (uint16_t)fmaxf(0,-f->Bounds[1]),
        .right_extent = (uint16_t)(width / 2u),
        .bottom_extent = (uint16_t)fmaxf(0,f->Bounds[3]),
        .age_ticks = scene->clock,
        .phase_ticks = scene->clock,
        .pulse_ticks = scene->clock,
        .kind = kActionEffect_AitosLavaReservoir,
        .phase = kActionEffectPhase_AitosLavaReservoir,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_Bg1HighPlane,
        .projection_plane = kActionEffectProjectionPlane_Bg1High,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-half_width, f->Bounds[1], half_width, f->Bounds[3]},
        },
      };
      AppendSurface(dst,f,&effect);
    }
  }
}

static bool AitosSplashStructureWidth(
    const ActionBgMapView *map, unsigned x, unsigned y,
    unsigned *total_cells,const ActionSurfaceField *f) {
  if (total_cells) *total_cells = 0;
  if (!map || !total_cells) return false;
  uint8_t tile = 0;
  if (!ActionBgMapView_LookupMetatile(map, (int)x, (int)y, &tile) ||
      tile != (unsigned)f->MapRule[0])
    return false;
  for (unsigned cells = 2; cells <= (unsigned)f->MapRule[9]; cells++) {
    const unsigned right_x = x + (cells - 1u) * kActionBgMetatilePixels;
    if (right_x < x ||
        !ActionBgMapView_LookupMetatile(
            map, (int)right_x, (int)y, &tile))
      return false;
    if (tile == (unsigned)f->MapRule[1]) continue;
    if (tile != (unsigned)f->MapRule[2]) return false;
    const uint8_t kLeft[] = {
      (unsigned)f->MapRule[3], (unsigned)f->MapRule[6],
    };
    const uint8_t kMiddle[] = {
      (unsigned)f->MapRule[4], (unsigned)f->MapRule[7],
    };
    const uint8_t kRight[] = {
      (unsigned)f->MapRule[5], (unsigned)f->MapRule[8],
    };
    for (unsigned row = 0; row < 2; row++) {
      const unsigned row_y = y + (row + 1u) * kActionBgMetatilePixels;
      for (unsigned cell = 0; cell < cells; cell++) {
        const uint8_t expected = cell == 0 ? kLeft[row]
            : cell + 1u == cells ? kRight[row] : kMiddle[row];
        if (!ActionBgMapView_LookupMetatile(
                map, (int)(x + cell * kActionBgMetatilePixels),
                (int)row_y, &tile) || tile != expected)
          return false;
      }
    }
    *total_cells = cells;
    return true;
  }
  return false;
}

static void CaptureAitosWater(ActionSceneEffectFrame *dst,
                              const ActionEnvironmentScene *scene, ActionEnvironmentWaterfallTrace *trace,const ActionSurfaceField *const fields[5]) {
  const ActionSurfaceField *f=fields[2]?fields[2]:ActionSurfaceField_Bundled(2);
  if(!dst||(!fields[2]&&!fields[3]&&!fields[4]))return;
  const ActionBgMapView map = scene->maps[0];
  if (!map.map) return;
  const int camera_x = scene->camera_x[0];
  const int camera_y = scene->camera_y[0];
  SceneBgScanBounds bounds;
  /* Cover the maximum wide side margin and Diorama vertical extension while
   * keeping the immutable scene payload bounded to structures that can
   * actually enter this presentation. */
  if (!SceneBgScanBounds_InitWindow(
          &bounds, &map, camera_x, camera_y, (int)f->Scan[0], (int)f->Scan[1], true))
    return;

  unsigned splash_count = 0;
  for (unsigned y = bounds.y0; y < bounds.y1;
       y += kActionBgMetatilePixels) {
    for (unsigned x = bounds.x0; x < bounds.x1;
         x += kActionBgMetatilePixels) {
      unsigned cells = 0;
      if (!AitosSplashStructureWidth(&map, x, y, &cells,f)) continue;
      const unsigned width = cells * kActionBgMetatilePixels;
      const float half_width = (float)width * 0.5f;
      const uint32_t identity =
          ((uint32_t)(y / kActionBgMetatilePixels) << 16) |
          (uint32_t)(x / kActionBgMetatilePixels);
      ActionEffectInstance effect = {
        .generation = SurfaceIdentity(f,0) ^ identity,
        .pulse_generation = SurfaceIdentity(f,2) ^ identity,
        .world_x = (int16_t)(x + width / 2u + (int)f->Anchor[0]),
        .world_y = (int16_t)(y + (int)f->Anchor[1]),
        .left_extent = (uint16_t)(width / 2u),
        .top_extent = (uint16_t)fmaxf(0,-f->Bounds[1]),
        .right_extent = (uint16_t)(width / 2u),
        .bottom_extent = (uint16_t)fmaxf(0,f->Bounds[3]),
        .age_ticks = scene->clock,
        .phase_ticks = scene->clock,
        .pulse_ticks = scene->clock,
        .kind = kActionEffect_AitosWaterSplash,
        .phase = kActionEffectPhase_AitosWaterSplash,
        .role = kActionEffectRole_Body,
        .flags = kActionEffectFlag_Visible,
        .render_layer = kActionEffectRenderLayer_WorldOverlay,
        .projection_plane = kActionEffectProjectionPlane_Bg1,
        .geometry = {
          .kind = kActionEffectGeometry_Rect,
          .data.rect = {-half_width, f->Bounds[1], half_width, f->Bounds[3]},
        },
      };
      /* This stage's geometry budget remains fourteen splash structures even
       * when another environment needs more host decoration records. */
      if (splash_count >= (unsigned)f->MapRule[10]) {
        dst->decoration_overflow = 1;
        dst->decoration_count = dst->decoration_visible_count = 0;
        return;
      }
      if (!fields[2] || f->Mode[0] || AppendSurface(dst,f,&effect)) splash_count++;
    }
  }
  if (trace) *trace = (ActionEnvironmentWaterfallTrace){
    .valid=true, .published=splash_count && !dst->decoration_overflow,
    .camera_x=camera_x, .camera_y=camera_y, .splash_count=splash_count,
    .x0=bounds.x0,.y0=bounds.y0,.x1=bounds.x1,.y1=bounds.y1};
  if (!splash_count || dst->decoration_overflow) {

    return;
  }


  /* The BG1 splash witness selects the waterfall section once for all three
   * fields. Display profiles never change the native page/section proof. */
  for(unsigned index=3;index<5;++index){
    f=fields[index];if(!f||f->Mode[0])continue;
    ActionEffectInstance effect={
      .generation=SurfaceIdentity(f,0)^scene->room,.pulse_generation=SurfaceIdentity(f,2)^scene->room,
      .world_x=(int16_t)(scene->camera_x[1]+(int)f->Anchor[0]),
      .world_y=(int16_t)(scene->camera_y[1]+(int)f->Anchor[1]),
      .left_extent=(uint16_t)fmaxf(0,-f->Bounds[0]),.top_extent=(uint16_t)fmaxf(0,-f->Bounds[1]),
      .right_extent=(uint16_t)fmaxf(0,f->Bounds[2]),.bottom_extent=(uint16_t)fmaxf(0,f->Bounds[3]),
      .age_ticks=scene->clock,.phase_ticks=scene->clock,.pulse_ticks=scene->clock,
      .kind=index==3?kActionEffect_AitosWaterfall:kActionEffect_AitosWaterfallMist,
      .phase=index==3?kActionEffectPhase_AitosWaterfallFlow:kActionEffectPhase_AitosWaterfallMist,
      .role=kActionEffectRole_Body,.flags=kActionEffectFlag_Visible,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={f->Bounds[0],f->Bounds[1],f->Bounds[2],f->Bounds[3]}}
    };
    if(!AppendSurface(dst,f,&effect))return;
  }
}


static void CaptureManualSurface(const ActionEnvironmentScene *scene,ActionSceneEffectFrame *dst,const ActionSurfaceField *f){
  static const uint8_t kinds[]={kActionEffect_AitosLavaPit,kActionEffect_AitosLavaReservoir,kActionEffect_AitosWaterSplash,kActionEffect_AitosWaterfall,kActionEffect_AitosWaterfallMist};
  static const uint8_t phases[]={kActionEffectPhase_AitosLavaPit,kActionEffectPhase_AitosLavaReservoir,kActionEffectPhase_AitosWaterSplash,kActionEffectPhase_AitosWaterfallFlow,kActionEffectPhase_AitosWaterfallMist};
  for(unsigned i=0;i<(unsigned)f->SourceCount[0];++i){const float *v=f->sources[i];
    ActionEffectInstance e={.generation=SurfaceIdentity(f,0)^(unsigned)v[6],.pulse_generation=SurfaceIdentity(f,2)^(unsigned)v[6],
      .world_x=(int16_t)v[0],.world_y=(int16_t)v[1],.left_extent=(uint16_t)fmaxf(0,-v[2]),.top_extent=(uint16_t)fmaxf(0,-v[3]),
      .right_extent=(uint16_t)fmaxf(0,v[4]),.bottom_extent=(uint16_t)fmaxf(0,v[5]),
      .age_ticks=scene->clock,.phase_ticks=scene->clock,.pulse_ticks=scene->clock,
      .kind=kinds[(unsigned)f->Kind[0]],.phase=phases[(unsigned)f->Kind[0]],.role=kActionEffectRole_Body,.flags=kActionEffectFlag_Visible,
      .geometry={.kind=kActionEffectGeometry_Rect,.data.rect={v[2],v[3],v[4],v[5]}}};
    if(!AppendSurface(dst,f,&e))return;
  }
}
void ActionSurfaceField_Capture(const ActionEnvironmentScene *scene,ActionSceneEffectFrame *dst,
    const ActionSurfaceField *const configured[5],ActionEnvironmentWaterfallTrace *trace){
  if(!scene||!dst||dst->decoration_overflow)return;
  const int profile=ActRaiserRoom_ProfileFor(scene->group,scene->room);
  const bool native[]={scene->group==kActRaiserMapGroup_Aitos&&scene->room==kAitosLavaMap,
    profile==kActRaiserRoomProfile_AitosAct2Lava,profile==kActRaiserRoomProfile_AitosWaterfall,
    profile==kActRaiserRoomProfile_AitosWaterfall,profile==kActRaiserRoomProfile_AitosWaterfall};
  const ActionSurfaceField *fields[5]={0};
  for(unsigned i=0;i<5;++i){const ActionSurfaceField *f=configured&&configured[i]?configured[i]:native[i]?ActionSurfaceField_Bundled(i):NULL;
    if(!f)continue;dst->surface_fields[i]=*f;dst->surface_fields_valid|=1u<<i;fields[i]=&dst->surface_fields[i];}
  CaptureAitosLavaPits(dst,scene,fields[0]);CaptureAitosSideLavaReservoirs(dst,scene,fields[1]);
  CaptureAitosWater(dst,scene,trace,fields);
  for(unsigned i=0;i<5;++i)if(fields[i]&&fields[i]->Mode[0])CaptureManualSurface(scene,dst,fields[i]);
}
bool ActionMapEnvironmentScene_UsesTorches(uint8_t group, uint8_t room) {
  return ActionGlowField_Bundled(group,room)!=NULL;
}
void ActionMapEnvironmentScene_Capture(const ActionEnvironmentScene *scene,
    ActionSceneEffectFrame *dst, ActionEnvironmentWaterfallTrace *trace) {
  if (!scene || !dst) return;
  CaptureWallTorches(dst,scene);
  ActionSurfaceField_Capture(scene,dst,scene->surface_fields,trace);
}
