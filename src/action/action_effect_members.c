#include "action_effect_members.h"
#include "action_light_kinds.h"
#include "action_ray_field.h"
#include "action_castle_field.h"
#include "action_water_field.h"
#include <math.h>
unsigned ActionEffectMembers_WaterLimit(unsigned kind) {
  if (kind == kActionEffect_CaveWater) return kActionWaterFieldMaxPools+kActionWaterFieldMaxFalls;
  if (kind == kActionEffect_CaveMist) return kActionWaterFieldMaxFalls;
  if (kind == kActionEffect_CaveDrips || kind == kActionEffect_CaveSheen) return kActionWaterFieldMaxWet;
  return 0;
}
unsigned ActionEffectMembers_WaterCount(unsigned kind, const ActionWaterField *field) {
  if (!field) return 0;
  if (kind == kActionEffect_CaveWater) return (unsigned)(field->Counts[0]+field->Counts[1]);
  if (kind == kActionEffect_CaveMist) return (unsigned)field->Counts[1];
  if (kind == kActionEffect_CaveDrips || kind == kActionEffect_CaveSheen) return (unsigned)field->Counts[2];
  return 0;
}
bool ActionEffectMembers_WaterSource(unsigned kind, unsigned id, const ActionWaterField *field,
                                    ActionNativeMemberSource *out) {
  if (!out || !id || id > ActionEffectMembers_WaterCount(kind,field)) return false;
  const unsigned index=id-1;
  if (kind == kActionEffect_CaveWater && index < (unsigned)field->Counts[0]) {
    const ActionCaveWaterRegion s=ActionWaterField_Pool(field,index);
    *out=(ActionNativeMemberSource){(s.left+s.right)*.5f,s.surface_y,s.right-s.left,16};
  } else if (kind == kActionEffect_CaveWater || kind == kActionEffect_CaveMist) {
    const ActionCaveWaterfall s=ActionWaterField_Fall(field,index-(kind==kActionEffect_CaveWater?(unsigned)field->Counts[0]:0));
    *out=(ActionNativeMemberSource){s.x,s.y,field->SprayShape[0]*2,field->SprayShape[1]};
  } else {
    const ActionCaveWetSource s=ActionWaterField_Wet(field,index);
    if (kind == kActionEffect_CaveSheen) {
      /* A wet patch belongs to the landing surface, not its drip ceiling. */
      const float depth=field->SheenShape[3]+field->SheenShape[4];
      *out=(ActionNativeMemberSource){s.x,s.landing_y,32,fmaxf(4,depth)};
    } else *out=(ActionNativeMemberSource){s.x,s.ceiling_y,32,s.landing_y-s.ceiling_y};
  }
  return true;
}
unsigned ActionEffectMembers_Count(unsigned kind, unsigned group, unsigned room) {
  if (group == 1 && room == 1 &&
      (kind == kActionEffect_ForestCanopyLight || kind == kActionEffect_ForestForwardLight)) {
    const ActionRayField *field = ActionRayField_Bundled();
    return field ? field->ray_count : 0;
  }
  if (group == 2 && kind == kActionEffect_CastleLight) {
    const ActionCastleField *f=ActionCastleField_Bundled(room);
    return f?(unsigned)f->SourceCount[0]:0;
  }
  if (group == 1 && room == 2) return ActionEffectMembers_WaterCount(kind,ActionWaterField_Bundled());
  return 0;
}
bool ActionEffectMembers_Source(unsigned kind, unsigned group, unsigned room, unsigned id,
                                ActionNativeMemberSource *out) {
  if (!out || !id || id > ActionEffectMembers_Count(kind, group, room)) return false;
  const unsigned index = id - 1;
  if (group == 1 && room == 1) {
    const ActionRayOpening *s = &ActionRayField_Bundled()->rays[index];
    *out = (ActionNativeMemberSource){s->x, 0, s->half_width * 2, 544};
  } else if (kind == kActionEffect_CastleLight) {
    const ActionCastleSource *s=&ActionCastleField_Bundled(room)->sources[index];
    *out=(ActionNativeMemberSource){s->x,s->y,s->width*2,s->length};
    return true;
  } else return ActionEffectMembers_WaterSource(kind,id,ActionWaterField_Bundled(),out);
  return true;
}
bool ActionEffectMembers_MovableSource(unsigned kind) {
  return kind == kActionEffect_WallTorch || kind == kActionEffect_AitosWaterSplash ||
         kind == kActionEffect_AitosWaterfall || kind == kActionEffect_AitosWaterfallMist;
}
bool ActionEffectMembers_Angled(unsigned kind, unsigned group, unsigned room, unsigned id) {
  if (!id || id > ActionEffectMembers_Count(kind, group, room)) return false;
  if (kind == kActionEffect_ForestCanopyLight || kind == kActionEffect_ForestForwardLight)
    return true;
  if (kind == kActionEffect_CastleLight) {
    return ActionCastleField_Bundled(room)->sources[id-1].kind!=kActionCastleSource_Torch;
  }
  return false;
}
const ActionNativeMember *ActionEffectMembers_Find(const ActionNativeMembers *members,
                                                   unsigned kind, unsigned index) {
  if (!members || members->count > kActionNativeMemberMax) return NULL;
  for (unsigned i = 0; i < members->count; ++i)
    if (members->records[i].kind == kind && members->records[i].index == index)
      return &members->records[i];
  return NULL;
}
void ActionEffectMembers_Tint(const ActionNativeMember *m, ArRenderVertex2D *v, int begin, int end,
                              bool multiply) {
  if (!m) return;
  const float gain = multiply ? m->intensity : ActionLightIntensity_Gain(m->intensity);
  const float opacity = ActionLightIntensity_Opacity(m->intensity);
  const float red = ((m->color >> 16) & 255) / 255.f, green = ((m->color >> 8) & 255) / 255.f,
              blue = (m->color & 255) / 255.f;
  for (int i = begin; i < end; ++i) {
    v[i].color.r *= red;
    v[i].color.g *= green;
    v[i].color.b *= blue;
    if (gain != 1) { v[i].color.r *= gain; v[i].color.g *= gain; v[i].color.b *= gain; }
    if (!multiply)
      v[i].color.a = fminf(1, v[i].color.a * opacity);
  }
}
