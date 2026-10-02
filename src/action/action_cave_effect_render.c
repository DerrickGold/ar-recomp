/* Fillmore's cave water, authored drips and stone-interior atmosphere.
 * All positions belong to decoded room layers; no native actors, live WRAM,
 * persistent particle state, or platform-specific rendering is used here.
 * Phase: pure. Tests: tests/action_effect_render_test.c. */
#include "action/action_effect_render_internal.h"
#include "action_effect_members.h"
#include "action_water_field.h"

/* One aggregate record per family; all bounds include the entire source map,
 * even though camera culling normally emits a small fraction of this work. */
_Static_assert((82 * 2 + 48 + 4 * 11) * 4 + 3 * 64 + 4 * kActionEffectGlowVertices <=
                   kActionSceneEffectRenderMaxVertices &&
               (82 * 2 + 48 + 4 * 11) * 6 + 3 * 96 + 4 * kActionEffectGlowIndices <=
                   kActionSceneEffectRenderMaxIndices,
               "cave water must fit the shared geometry scratch");
_Static_assert(15 * 64 + 25 * 13 * 4 <= kActionSceneEffectRenderMaxVertices &&
               15 * 96 + 25 * 13 * 6 <= kActionSceneEffectRenderMaxIndices &&
               96 * 7 <= kActionSceneEffectRenderMaxVertices &&
               96 * 15 <= kActionSceneEffectRenderMaxIndices,
               "cave atmosphere and clipped tower light must fit their layers");
enum { kCaveDustMaxGrains = 28 };
_Static_assert(kActionLandingDustMaxPuffs * kCaveDustMaxGrains * 4 +
                   4*3*32*7 + 4*12*4 <=
                   kActionSceneEffectRenderMaxVertices &&
               kActionLandingDustMaxPuffs * kCaveDustMaxGrains * 6 +
                   4*3*32*15 + 4*12*6 <=
                   kActionSceneEffectRenderMaxIndices,
               "contact particles, cave mist and grit must fit the shared batch");

static bool InCaveField(const ActionEffectInstance *effect, float x, float y, float margin) {
  const ActionEffectLocalRect *r = &effect->geometry.data.rect;
  x -= effect->world_x;
  y -= effect->world_y;
  return x >= r->x0 - margin && x <= r->x1 + margin &&
      y >= r->y0 - margin && y <= r->y1 + margin;
}

static bool CaveClip(const ActionEffectInstance *effect, ActionEffectClipBoundsFn clip_bounds,
    void *userdata, ActionEffectLocalRect *clip) {
  *clip = effect->geometry.data.rect;
  if (clip_bounds && !clip_bounds(userdata, effect, clip)) return false;
  if (effect->flags & kActionEffectFlag_ClipToRect) {
    clip->x0 = fmaxf(clip->x0, effect->clip_rect.x0);
    clip->y0 = fmaxf(clip->y0, effect->clip_rect.y0);
    clip->x1 = fminf(clip->x1, effect->clip_rect.x1);
    clip->y1 = fminf(clip->y1, effect->clip_rect.y1);
  }
  return RectIsSane(clip) && clip->x0 < clip->x1 && clip->y0 < clip->y1;
}

static bool CaveQuad(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ArRenderPointF *world, ArRenderColorF color,
    ActionEffectProjectPointFn project_point, void *userdata) {
  ArRenderPointF points[4];
  for (int i = 0; i < 4; i++)
    if (!project_point(userdata, effect, world[i].x - effect->world_x,
            world[i].y - effect->world_y, &points[i]))
      return true;
  if (!Reserve(writer, 4, 6)) return false;
  const int base = writer->vertex_count;
  for (int i = 0; i < 4; i++)
    writer->vertices[writer->vertex_count++] = (ArRenderVertex2D){points[i], color, {0,0}};
  const int corners[] = {0,1,2,0,2,3};
  for (int i = 0; i < 6; i++) writer->indices[writer->index_count++] = base + corners[i];
  return true;
}

static bool CaveDiamond(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionWaterField *p,
    float x, float y, float width, float height, float alpha,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (!InCaveField(effect, x, y, 4) || alpha <= .005f) return true;
  const ArRenderPointF points[] = {{x-width,y}, {x,y-height}, {x+width,y}, {x,y+height}};
  return CaveQuad(writer, effect, points, (ArRenderColorF){p->GlintColor[0],p->GlintColor[1],p->GlintColor[2],fminf(1,alpha)},
      project_point, userdata);
}

static bool CaveRipple(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionWaterField *p,
    float x, float y, float age, float strength,
    ActionEffectProjectPointFn project_point, void *userdata) {
  if (!InCaveField(effect, x, y, 16) || age < 0 || age > 1) return true;
  const float rx = p->RippleShape[0] + age * p->RippleShape[1], ry = p->RippleShape[2] + age * p->RippleShape[3];
  const float alpha = strength * (1-age) * fminf(1, age * p->RippleShape[6]);
  for (int i = 0; i < 32; i += 2) {
    const int j = (i + 2) & 31;
    const ArRenderPointF points[] = {
      {x+kCircle32[i][0]*rx, y+kCircle32[i][1]*ry},
      {x+kCircle32[j][0]*rx, y+kCircle32[j][1]*ry},
      {x+kCircle32[j][0]*(rx+p->RippleShape[4]), y+kCircle32[j][1]*(ry+p->RippleShape[5])},
      {x+kCircle32[i][0]*(rx+p->RippleShape[4]), y+kCircle32[i][1]*(ry+p->RippleShape[5])},
    };
    if (!CaveQuad(writer, effect, points, (ArRenderColorF){p->RippleColor[0],p->RippleColor[1],p->RippleColor[2],fminf(1,alpha)},
            project_point, userdata))
      return false;
  }
  return true;
}

static bool CaveWater(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionWaterField *p,
                      const ActionNativeMembers *members, ActionEffectProjectPointFn project_point,
                      void *userdata) {
  for (unsigned pool = 0; pool < (unsigned)p->Counts[0]; pool++) {
    const ActionNativeMember *member = ActionEffectMembers_Find(members, effect->kind, pool);
    if (member && !member->enabled) continue;
    const int first_vertex = writer->vertex_count;
    ActionCaveWaterRegion region = ActionWaterField_Pool(p,pool);
    if (member) {
      const float centre = (region.left + region.right) * .5f;
      region.left = (int16_t)lroundf(centre + member->offset_x +
                                     (region.left - centre) * member->width_scale);
      region.right = (int16_t)lroundf(centre + member->offset_x +
                                      (region.right - centre) * member->width_scale);
      region.surface_y += (int16_t)lroundf(member->offset_y);
    }
    const ActionCaveWaterRegion *water = &region;
    if (!InCaveField(effect, effect->world_x, water->surface_y, 12)) continue;
    for (int x = (int)water->left + (int)p->PoolInsets[0]; x < (int)water->right - (int)p->PoolInsets[1]; x += (int)p->GlintMotion[3]) {
      if (!InCaveField(effect, (float)x, water->surface_y, 12)) continue;
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)x ^ (pool + 1) * 0x9E3779B9u);
      const float t = ((effect->phase_ticks + seed) & ((unsigned)p->GlintMotion[4]-1)) / p->GlintMotion[4];
      const float shimmer = sinf(t * 3.14159265f);
      const float y = water->surface_y + p->GlintMotion[0] + p->GlintMotion[1] * HashUnit(seed ^ 0x32u);
      if (!CaveDiamond(writer, effect, p, x + p->GlintMotion[2] * sinf(t * 6.2831853f), y,
              p->GlintShape[0] + p->GlintShape[1] * HashUnit(seed), p->GlintShape[2], p->GlintShape[3] * shimmer * shimmer,
              project_point, userdata))
        return false;
    }
    const uint32_t seed = DeterministicHash_Mix32((pool + 1) * 0x85EBCA6Bu);
    const float age = ((effect->phase_ticks + seed) & ((unsigned)p->GlintMotion[4]-1)) / p->RippleShape[7];
    const float x = water->left + p->PoolRipple[0] + (water->right - water->left - 2*p->PoolRipple[0]) * HashUnit(seed);
    if (!CaveRipple(writer, effect, p, x, water->surface_y + p->PoolRipple[1], age, p->PoolRipple[2],
            project_point, userdata))
      return false;
    ActionEffectMembers_Tint(member, writer->vertices, first_vertex, writer->vertex_count, false);
  }
  for (unsigned fall = 0; fall < (unsigned)p->Counts[1]; fall++) {
    const ActionNativeMember *member =
        ActionEffectMembers_Find(members, effect->kind, (unsigned)p->Counts[0] + fall);
    if (member && !member->enabled) continue;
    const int first_vertex = writer->vertex_count;
    ActionCaveWaterfall end = ActionWaterField_Fall(p,fall);
    if (member) {
      end.x += (int16_t)lroundf(member->offset_x);
      end.y += (int16_t)lroundf(member->offset_y);
    }
    for (int row = 0; row < (int)end.y; row += (int)p->FallMotion[0]) {
      if (!InCaveField(effect, end.x, row + p->FallMotion[0]*.5f, 28)) continue;
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)row ^ (fall + 1) * 0xC2B2AE35u);
      const float t = ((effect->phase_ticks + seed) & ((unsigned)p->FallMotion[1]-1)) / p->FallMotion[1];
      const float x = end.x + (p->FallMotion[2] * HashUnit(seed) + p->FallMotion[3]) * (member ? member->width_scale : 1);
      const float y = row + t * p->FallMotion[0];
      if (y >= end.y - p->FallMotion[4]) continue;
      if (!CaveDiamond(writer, effect, p, x, y, p->FallGlint[0], p->FallGlint[1],
              p->FallGlint[2] * sinf(t * 3.14159265f), project_point, userdata))
        return false;
    }
    if (!InCaveField(effect, end.x, end.y, 24)) {
      ActionEffectMembers_Tint(member, writer->vertices, first_vertex, writer->vertex_count, false);
      continue;
    }
    const float pulse = p->FallMotion[5] + p->FallMotion[6] * sinf((effect->phase_ticks & ((unsigned)p->FallPulse[0]-1)) * p->FallPulse[1] + fall);
    const ActionEffectGlowStyle glow = {
        .radius_x = p->FallGlow[0] * (member ? member->width_scale : 1),
        .radius_y = p->FallGlow[1],
        .ring_scale = {p->FallRings[0],p->FallRings[1],p->FallRings[2]},
        .axis_x = p->FallGlow[3],
        .centre = ActionWaterField_Color(p->FallCenter),
        .ring = {ActionWaterField_Color(p->FallRing1),ActionWaterField_Color(p->FallRing2),ActionWaterField_Color(p->FallRing3)},
    };
    if (!AppendGlow(writer, effect, &glow, pulse, end.x - effect->world_x,
            end.y + p->FallGlow[2] - effect->world_y, project_point, userdata))
      return false;
    ActionEffectMembers_Tint(member, writer->vertices, first_vertex, writer->vertex_count, false);
  }
  return true;
}

static bool CaveDrips(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionWaterField *p,
                      const ActionNativeMembers *members, ActionEffectProjectPointFn project_point,
                      void *userdata) {
  for (unsigned i = 0; i < (unsigned)p->Counts[2]; i++) {
    const ActionNativeMember *member = ActionEffectMembers_Find(members, effect->kind, i);
    if (member && !member->enabled) continue;
    const int first_vertex = writer->vertex_count;
    ActionCaveWetSource edited = ActionWaterField_Wet(p,i);
    if (member) {
      edited.x += (int16_t)lroundf(member->offset_x);
      edited.landing_y =
          (int16_t)lroundf(edited.ceiling_y + member->offset_y +
                           (edited.landing_y - edited.ceiling_y) * member->length_scale);
      edited.ceiling_y += (int16_t)lroundf(member->offset_y);
    }
    const ActionCaveWetSource *source = &edited;
    if (!(effect->source_mask & (1u << i))) continue;
    const uint32_t seed = DeterministicHash_Mix32((i + 1) * 0x85EBCA6Bu);
    const unsigned mask = (unsigned)((seed & 1u) ? p->DripTiming[1] : p->DripTiming[0])-1;
    const unsigned age = (effect->phase_ticks + seed) & mask;
    if (age < p->DripTiming[2]) {
      if (!CaveDiamond(writer, effect, p, source->x, source->ceiling_y,
              p->DripGather[0] + age * p->DripGather[1], p->DripGather[2] + age * p->DripGather[3], p->DripGather[4],
              project_point, userdata)) return false;
    } else if (age < p->DripTiming[2]+p->DripTiming[3]) {
      const float t = (age - (unsigned)p->DripTiming[2]) / p->DripTiming[3];
      const float y = source->ceiling_y + t * t * (source->landing_y - source->ceiling_y);
      if (!CaveDiamond(writer, effect, p, source->x, y, p->DripFall[0], p->DripFall[1] + p->DripFall[2]*t, p->DripFall[3],
              project_point, userdata)) return false;
    } else if (age < p->DripTiming[2]+p->DripTiming[3]+p->DripTiming[4]) {
      const float t = (age - (unsigned)(p->DripTiming[2]+p->DripTiming[3])) / p->DripTiming[4];
      if (source->water) {
        if (!CaveRipple(writer, effect, p, source->x, source->landing_y + p->DripContact[0], t, p->DripContact[1],
                project_point, userdata)) return false;
      } else {
        for (int side = -1; side <= 1; side += 2)
          if (!CaveDiamond(writer, effect, p, source->x + side * t * p->DripContact[2],
                  source->landing_y - p->DripContact[3]*t*(1-t), p->DripContact[4], p->DripContact[5], p->DripContact[6]*(1-t),
                  project_point, userdata)) return false;
      }
    }
    ActionEffectMembers_Tint(member, writer->vertices, first_vertex, writer->vertex_count, false);
  }
  return true;
}

static bool TempleAir(const ActionAtmosphereField *p, float x, float y) {
  for(unsigned i=0;i<(unsigned)p->Counts[1];++i) {
    const float *v=ActionAtmosphereField_Area(p,i);
    if(x>v[0]&&y>v[1]&&x<v[2]&&y<v[3])return true;
  }
  return false;
}

static float AmbientFalloff(float offset) {
  const float q = fmaxf(0, 1-offset*offset);
  return q*q*q;
}

static float TempleExposure(const ActionAtmosphereField *p, float x, float y) {
  float light = 0;
  for (unsigned i=0;i<(unsigned)p->Counts[0];++i) {
    const float *s=ActionAtmosphereField_Ambient(p,i);
    const float dy=y-s[1];
    const float along=AmbientFalloff(dy/s[3]);
    const float across=AmbientFalloff((x-s[0]-dy*s[4])/s[2]);
    light=fmaxf(light,along*across*s[5]);
  }
  return light;
}
/* A borrowed style view shares one mesh kernel without coupling atmosphere
 * definitions to the water recipe or copying either complete field per draw. */
typedef struct CaveMistStyle {
  const float *MistVolume,*MistBillow,*MistDensity,*MistLighting,*MistColor,*MistGain;
} CaveMistStyle;
#define MIST_STYLE(p) ((CaveMistStyle){(p)->MistVolume,(p)->MistBillow,(p)->MistDensity,(p)->MistLighting,(p)->MistColor,(p)->MistGain})

_Static_assert(kActionTempleMistMaxSpans*3*32*7 <= kActionSceneEffectRenderMaxVertices &&
               kActionTempleMistMaxSpans*3*32*15 <= kActionSceneEffectRenderMaxIndices,
               "layered temple mist must fit the shared alpha batch");

/* Three translucent density slices suggest depth and internal scattering.
 * Continuous, independent drift replaces the old four-tick silhouette wobble.
 * Every vertex is projected in world space; no screen-sized cloud minimum or
 * perspective-dependent expansion. Shared by grounded fog and splash spray. */
static bool CaveMistVolume(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    CaveMistStyle style,
    const ActionEffectLocalRect *clip, float left, float right, float floor, float height,
    unsigned seed, float illumination, ActionEffectProjectPointFn project_point, void *userdata) {
  if (right <= clip->x0 || left >= clip->x1 || floor < clip->y0 || floor-height > clip->y1)
    return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float half = (right-left)*.5f;
  for (unsigned slice = 0; slice < (unsigned)style.MistVolume[0]; slice++) {
    const unsigned mask = ((unsigned)style.MistVolume[1]<<slice)-1;
    const float phase = ((effect->phase_ticks+seed)&mask)*(6.2831853f/(mask+1));
    const float center = (left+right)*.5f + half*style.MistVolume[2]*sinf(phase+slice);
    const float width = half*(style.MistVolume[3]-style.MistVolume[4]*slice);
    ArRenderVertex2D vertices[25];
    int mapped[25];
    for (int col = 0; col < 5; col++) {
      const float u = (col-2)*.5f;
      const float billow = style.MistBillow[0]+style.MistBillow[1]*sinf(u*style.MistBillow[3]+phase+slice)+style.MistBillow[2]*cosf(u*style.MistBillow[4]-phase*style.MistBillow[5]);
      const float top = height*(style.MistVolume[5]+style.MistVolume[6]*billow-style.MistVolume[7]*slice);
      for (int row = 0; row < 5; row++) {
        const float rise = 1-row*.25f;
        const float across = 1-u*u*u*u;
        const float density = (style.MistDensity[0]+style.MistDensity[1]*billow)*across*across*AmbientFalloff(rise);
        const float light = style.MistLighting[0]+illumination+style.MistLighting[1]*rise+style.MistLighting[2]*billow;
        const int at = row*5+col;
        vertices[at] = (ArRenderVertex2D){
          {center+u*width,floor-top*rise},
          {fminf(1,style.MistColor[0]+style.MistGain[0]*light),fminf(1,style.MistColor[1]+style.MistGain[1]*light),fminf(1,style.MistColor[2]+style.MistGain[2]*light),fminf(1,density)},{0,0},
        };
        mapped[at] = -1;
      }
    }
    for (int row = 0; row < 4; row++) for (int col = 0; col < 4; col++) {
      const int a = row*5+col, b = a+5;
      const int triangles[] = {a,a+1,b,a+1,b+1,b};
      for (int t = 0; t < 6; t += 3)
        if (!AppendSceneClippedTriangle(writer,&mesh,vertices,mapped,&triangles[t],clip,
                project_point,userdata)) return false;
    }
  }
  return true;
}

static bool TempleGroundMist(ActionEffectGeometryWriter *writer,
    const ActionEffectInstance *effect, const ActionAtmosphereField *p, ActionEffectProjectPointFn project_point,
    ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  ActionEffectLocalRect clip;
  if (!CaveClip(effect,clip_bounds,userdata,&clip)) return true;
  const float width = effect->geometry.data.rect.x1;
  const unsigned seed = DeterministicHash_Mix32((uint16_t)effect->world_x*0x9E3779B9u);
  const float light = p->FloorStyle[1]*TempleExposure(p,
      effect->world_x+width*.5f,effect->world_y-p->FloorStyle[2]);
  return CaveMistVolume(writer,effect,MIST_STYLE(p),&clip,0,width,0,p->FloorStyle[0],seed,light,project_point,userdata);
}

static bool CaveMist(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionWaterField *p,
                     const ActionNativeMembers *members, ActionEffectProjectPointFn project_point,
                     ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  ActionEffectLocalRect clip;
  if (!CaveClip(effect,clip_bounds,userdata,&clip)) return true;
  for (unsigned i = 0; i < (unsigned)p->Counts[1]; i++) {
    const ActionNativeMember *member = ActionEffectMembers_Find(members, effect->kind, i);
    if (member && !member->enabled) continue;
    const int first_vertex = writer->vertex_count;
    ActionCaveWaterfall contact = ActionWaterField_Fall(p,i);
    if (member) {
      contact.x += (int16_t)lroundf(member->offset_x);
      contact.y += (int16_t)lroundf(member->offset_y);
    }
    const float width = p->SprayShape[0] * (member ? member->width_scale : 1),
                height = p->SprayShape[1] * (member ? member->length_scale : 1);
    const float x = contact.x-effect->world_x, y = contact.y+p->SprayShape[2]-effect->world_y;
    const unsigned seed = DeterministicHash_Mix32((i+1)*0x9E3779B9u);
    if (!CaveMistVolume(writer, effect, MIST_STYLE(p), &clip, x - width, x + width, y, height, seed, p->SprayShape[3],
                        project_point, userdata))
      return false;
    ActionEffectMembers_Tint(member, writer->vertices, first_vertex, writer->vertex_count, false);
  }
  return true;
}

static bool CaveSheen(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionWaterField *p,
                      const ActionNativeMembers *members, ActionEffectProjectPointFn project_point,
                      void *userdata) {
  for (unsigned i = 0; i < (unsigned)p->Counts[2]; i++) {
    const ActionNativeMember *member = ActionEffectMembers_Find(members, effect->kind, i);
    if (member && !member->enabled) continue;
    const int first_vertex = writer->vertex_count;
    ActionCaveWetSource edited = ActionWaterField_Wet(p,i);
    if (member) {
      edited.x += (int16_t)lroundf(member->offset_x);
      edited.landing_y += (int16_t)lroundf(member->offset_y);
    }
    const ActionCaveWetSource *s = &edited;
    const float scale = member ? member->width_scale : 1;
    if (!(effect->source_mask & (1u << i)) || s->water ||
        !InCaveField(effect, s->x, s->landing_y, 16)) continue;
    const uint32_t seed = DeterministicHash_Mix32((i+1)*0xC2B2AE35u);
    const float phase = ((effect->phase_ticks + seed) & ((unsigned)p->SheenMotion[0]-1))*p->SheenMotion[1];
    /* A continuous wet patch inside the measured rock silhouette, with a
     * narrow bright lip and a deeper blue body. Never bridge an open column
     * or stretch a horizontal streak across a sloping edge. */
    for (unsigned column = 0; column < kActionWaterContourColumns; column++) {
      if (s->contour[column] == 127) continue;
      const float edge = fmaxf(0,1-fabsf(((float)column-p->SheenShape[0])/p->SheenShape[1]));
      const float x = s->x + ((int)column - p->SheenShape[0]) * scale;
      const float y = s->landing_y+s->contour[column]+p->SheenShape[2];
      const float shimmer = p->SheenMotion[4]+p->SheenMotion[5]*sinf(phase+column*p->SheenMotion[2]+i*p->SheenMotion[3]);
      const float depth = (p->SheenShape[3] + p->SheenShape[4] * edge) * (member ? member->length_scale : 1);
      const ArRenderPointF body[] = {
          {x, y}, {x + scale, y}, {x + scale, y + depth}, {x, y + depth}};
      if (!CaveQuad(writer,effect,body,(ArRenderColorF){p->SheenBody[0],p->SheenBody[1],p->SheenBody[2],fminf(1,fmaxf(0,edge*p->SheenBody[3]))},
              project_point,userdata)) return false;
      const ArRenderPointF lip[] = {{x, y}, {x + scale, y}, {x + scale, y + p->SheenShape[5]}, {x, y + p->SheenShape[5]}};
      if (!CaveQuad(writer,effect,lip,
              (ArRenderColorF){p->SheenLip[0],p->SheenLip[1],p->SheenLip[2],fminf(1,fmaxf(0,edge*(p->SheenLip[3]+p->SheenLip[4]*shimmer*shimmer)))},
              project_point,userdata)) return false;
    }
    ActionEffectMembers_Tint(member, writer->vertices, first_vertex, writer->vertex_count, false);
  }
  return true;
}

/* Fixed-size grains, each with its own lifetime and ballistic arc. The burst
 * spreads through motion, not by scaling a translucent cloud mesh. */
static bool CaveDustGrains(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionAtmosphereField *p,
    float origin_x, float origin_y, uint32_t burst, float ticks, float strength, unsigned count,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned i = 0; i < count; i++) {
    const uint32_t seed = DeterministicHash_Mix32(burst ^ (i+1)*0x9E3779B9u);
    const float delay = p->GrainTiming[0]*HashUnit(seed ^ 0xD3u);
    const float lifetime = p->GrainTiming[1]+p->GrainTiming[2]*HashUnit(seed ^ 0xB5u);
    const float age = (ticks-delay)/lifetime;
    if (age <= 0 || age >= 1) continue;
    const float direction = 2*HashUnit(seed ^ 0x93u)-1;
    const float spread = direction*(p->GrainMotion[0]+(p->GrainMotion[1]+p->GrainMotion[2]*HashUnit(seed))*age)*strength;
    const float lift = (p->GrainMotion[3]+p->GrainMotion[4]*HashUnit(seed ^ 0xAFu))*sinf(age*3.14159265f)*strength;
    const float x = origin_x+spread+p->GrainMotion[5]*sinf(age*p->GrainMotion[6]+i);
    const float radius = p->GrainShape[0]+p->GrainShape[1]*HashUnit(seed ^ 0x61u);
    const float y = origin_y-radius-p->GrainMotion[7]-lift;
    const float fade = fminf(1,age*p->GrainTiming[3])*(1-age)*(1-age);
    const ArRenderPointF points[] = {{x-radius,y},{x,y-radius*p->GrainShape[2]},
                                    {x+radius,y},{x,y+radius*p->GrainShape[2]}};
    if (!CaveQuad(writer,effect,points,
            (ArRenderColorF){p->GrainColor[0],p->GrainColor[1],p->GrainColor[2],(p->GrainShape[3]+p->GrainShape[4]*HashUnit(seed))*fade},
            project_point,userdata)) return false;
  }
  return true;
}

static bool TempleGrit(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionAtmosphereField *p,
    ActionEffectProjectPointFn project_point, void *userdata) {
  for (unsigned i = 0; i < (unsigned)p->Counts[2]; i++) {
    const float *s=ActionAtmosphereField_Grit(p,i);
    const uint32_t seed = DeterministicHash_Mix32((uint32_t)s[3]*0x85EBCA6Bu);
    const unsigned age = (effect->phase_ticks + seed) & ((unsigned)p->GritTiming[0]-1);
    for (unsigned grain = 0; grain < (unsigned)p->GritTiming[1]; grain++) {
      if (age < grain*(unsigned)p->GritTiming[2] || age >= grain*(unsigned)p->GritTiming[2]+(unsigned)p->GritTiming[3]) continue;
      const float t = (age-grain*(unsigned)p->GritTiming[2])/p->GritTiming[3];
      const float x = s[0] + (int)grain - p->GritShape[0];
      const float y = s[1] + (s[2]-s[1])*t*t;
      if (!InCaveField(effect, x, y, p->GritShape[5])) continue;
      const float r = p->GritShape[1] + grain*p->GritShape[2];
      const ArRenderPointF points[] = {{x-r,y}, {x,y-p->GritShape[3]}, {x+r,y}, {x,y+p->GritShape[4]}};
      if (!CaveQuad(writer, effect, points, ActionWaterField_Color(p->GritColor),
              project_point, userdata)) return false;
    }
    if (age >= p->GritTiming[3] && age < p->GritTiming[4] && InCaveField(effect,s[0],s[2],p->GritBurst[2]) &&
        !CaveDustGrains(writer,effect,p,s[0],s[2],seed,age-(unsigned)p->GritTiming[3],p->GritBurst[0],(unsigned)p->GritBurst[1],
            project_point,userdata)) return false;
  }
  return true;
}

_Static_assert((8*72+3*32)*7 <= kActionSceneEffectRenderMaxVertices &&
               (8*72+3*32)*15 <= kActionSceneEffectRenderMaxIndices,
               "all clipped ambient pools must fit the shared surface-light batch");

static bool CaveAmbientLight(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionAtmosphereField *p,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ActionEffectLocalRect clip;
  if (!CaveClip(effect, clip_bounds, userdata, &clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float phase = (effect->phase_ticks & ((unsigned)p->AmbientMotion[0]-1))*p->AmbientMotion[1];
  for (unsigned i = 0; i < (unsigned)p->Counts[0]; i++) {
    const float *s=ActionAtmosphereField_Ambient(p,i);
    const float cx = s[0]-effect->world_x, cy = s[1]-effect->world_y;
    const float reach_x = s[2] + fabsf(s[4])*s[3];
    if (cx+reach_x <= clip.x0 || cx-reach_x >= clip.x1 ||
        cy+s[3] <= clip.y0 || cy-s[3] >= clip.y1) continue;
    /* Nearly steady light; only a very slow, small intensity variation. */
    const float drift=p->AmbientMotion[2]+p->AmbientMotion[3]*sinf(phase+s[7]*p->AmbientMotion[4]);
    const ArRenderColorF tint={p->AmbientColor[0],p->AmbientColor[1],p->AmbientColor[2],1};
    ArRenderVertex2D vertices[49];
    int mapped[49];
    for (int row = 0; row < 7; row++) {
      const float v = (row-3)/3.0f, dy = v*s[3];
      const float response = s[6];
      const float along = AmbientFalloff(v)*s[5]*drift*response;
      for (int col = 0; col < 7; col++) {
        const float u = (col-3)/3.0f;
        const float amount = along*AmbientFalloff(u);
        const int at = row*7+col;
        vertices[at] = (ArRenderVertex2D){
          {cx + dy*s[4] + u*s[2], cy + dy},
          {tint.r*amount,tint.g*amount,tint.b*amount,1}, {0,0},
        };
        mapped[at] = -1;
      }
    }
    for (int row = 0; row < 6; row++) {
      for (int col = 0; col < 6; col++) {
        const int a = row*7+col, b = a+7;
        const int triangles[] = {a,a+1,b,a+1,b+1,b};
        for (int t = 0; t < 6; t += 3)
          if (!AppendSceneClippedTriangle(writer, &mesh, vertices, mapped, &triangles[t],
                  &clip, project_point, userdata)) return false;
      }
    }
  }
  return true;
}

static bool TempleDust(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionAtmosphereField *p,
    ActionEffectProjectPointFn project_point, void *userdata) {
  /* A narrow shaft needs several readable flecks at once. Jittered cells
   * retain world anchoring while varied size/opacity avoid a uniform snowfield.
   * One quad per cell keeps the denser field inside the existing batch. */
  const int first_x = (int)floorf((effect->world_x - p->DustGrid[4]) / p->DustGrid[0]);
  const int first_y = (int)floorf(effect->world_y / p->DustGrid[1]);
  for (int iy = 0; iy < (int)p->DustGrid[3]; iy++) {
    for (int ix = 0; ix < (int)p->DustGrid[2]; ix++) {
      const int cx = first_x + ix, cy = first_y + iy;
      const uint32_t seed = DeterministicHash_Mix32((uint32_t)cx * 0x9E3779B9u ^
          (uint32_t)cy * 0x85EBCA6Bu ^ effect->pulse_generation);
      if ((seed & (unsigned)p->DustSkip[0]) == 0) continue;
      const float t = ((effect->phase_ticks + seed) & ((unsigned)p->DustMotion[0]-1)) / p->DustMotion[0];
      const float x = cx * (int)p->DustGrid[0] + p->DustMotion[1] + p->DustMotion[2] * HashUnit(seed) + p->DustMotion[3] * sinf(t * 6.2831853f);
      const float y = cy * (int)p->DustGrid[1] + p->DustMotion[4] + p->DustMotion[5] * HashUnit(seed ^ 0x17u) - p->DustMotion[6]*t;
      if (!TempleAir(p, x, y)) continue;
      const float fade = sinf(t * 3.14159265f);
      const float radius = p->DustShape[0] + p->DustShape[1] * HashUnit(seed ^ 0x59u);
      const float exposure = TempleExposure(p, x, y);
      const float alpha = fminf(1, p->DustShape[3] + p->DustShape[4]*HashUnit(seed ^ 0xA3u) + p->DustShape[5]*exposure) *
          fade * fade;
      if (p->DustDensity[0]!=0) {
        if (!InCaveField(effect, x, y, p->DustDensity[3])) continue;
        const ArRenderPointF points[] = {
          {x-radius,y}, {x,y-radius*p->DustShape[2]}, {x+radius,y}, {x,y+radius*p->DustShape[2]},
        };
        /* Warm mineral dust reads against the cool masonry; the damp lower hall
         * has fewer bright flecks competing with its ground mist. */
        const float density = y > p->DustDensity[1] ? p->DustDensity[2] : 1;
        if (!CaveQuad(writer, effect, points, (ArRenderColorF){p->DustColor[0],p->DustColor[1],p->DustColor[2],alpha*density},
                project_point, userdata)) return false;
        continue;
      }
      const ArRenderPointF points[] = {
        {x-radius,y}, {x,y-radius*p->DustShape[2]}, {x+radius,y}, {x,y+radius*p->DustShape[2]},
      };
      if (!CaveQuad(writer, effect, points, (ArRenderColorF){p->DustColor[0],p->DustColor[1],p->DustColor[2],alpha},
              project_point, userdata)) return false;
    }
  }
  return true;
}

static bool LandingDust(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    ActionEffectProjectPointFn project_point, void *userdata) {
  const uint32_t burst = DeterministicHash_Mix32(effect->generation ^
      ((uint32_t)(uint16_t)effect->world_x << 16) ^ (uint16_t)effect->world_y);
  const float strength = .85f+effect->dust_strength*.25f;
  const unsigned count = 20+(burst % (kCaveDustMaxGrains-19));
  return CaveDustGrains(writer,effect,ActionAtmosphereField_Bundled(2),effect->world_x,effect->world_y,burst,
      effect->phase_ticks,strength,count,project_point,userdata);
}

static bool TowerLight(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
    const ActionAtmosphereField *p,
    ActionEffectProjectPointFn project_point, ActionEffectClipBoundsFn clip_bounds,
    void *userdata) {
  ActionEffectLocalRect clip;
  if (!CaveClip(effect, clip_bounds, userdata, &clip)) return true;
  ActionEffectInstance mesh = *effect;
  mesh.flags |= kActionEffectFlag_ClippedMesh;
  const float phase = (effect->phase_ticks & ((unsigned)p->TowerMotion[0]-1))*p->TowerMotion[1];
  for (int window = 0; window < (int)p->Counts[3]; window++) {
    /* Slow common cloud shadow, with a smaller staggered window response.
     * Neither the ray position nor the battle-floor footprint moves. */
    const float clouds = p->TowerMotion[2] + p->TowerMotion[3]*sinf(phase) + p->TowerMotion[4]*sinf(p->TowerMotion[5]*phase+window*p->TowerMotion[6]);
    const float *s=ActionAtmosphereField_Tower(p,(unsigned)window);
    ArRenderVertex2D source[25];
    int mapped[25];
    for (int row = 0; row < 5; row++) {
      for (int col = 0; col < 5; col++) {
        const float across = p->TowerAcross[col];
        const float amount = p->TowerStrengths[row] * across * clouds;
        const float y = p->TowerRows[row], x = s[0] + (y-s[1]) * s[2];
        const int index = row * 5 + col;
        source[index] = (ArRenderVertex2D){
          {x + (col*.5f-1)*p->TowerWidths[row] - effect->world_x, y - effect->world_y},
          {p->TowerColor[0]*amount,p->TowerColor[1]*amount,p->TowerColor[2]*amount,1}, {0,0},
        };
        mapped[index] = -1;
      }
    }
    for (int row = 0; row < 4; row++) {
      for (int col = 0; col < 4; col++) {
        const int a = row*5+col, b = a+5;
        const int triangles[] = {a,a+1,b,a+1,b+1,b};
        for (int i = 0; i < 6; i += 3)
          if (!AppendSceneClippedTriangle(writer, &mesh, source, mapped, &triangles[i],
                  &clip, project_point, userdata)) return false;
      }
    }
  }
  return true;
}

bool AppendCaveEnvironment(ActionEffectGeometryWriter *writer, const ActionEffectInstance *effect,
                           const ActionWaterField *definition, const ActionAtmosphereField *atmosphere, const ActionNativeMembers *members,
                           ActionEffectProjectPointFn project_point,
                           ActionEffectClipBoundsFn clip_bounds, void *userdata) {
  const bool water=effect->kind==kActionEffect_CaveWater||effect->kind==kActionEffect_CaveDrips||
      effect->kind==kActionEffect_CaveMist||effect->kind==kActionEffect_CaveSheen;
  const ActionWaterField *p=water?(definition?definition:ActionWaterField_Bundled()):NULL;
  const ActionAtmosphereField *a=water||effect->kind==kActionEffect_LandingDust?NULL:
      (atmosphere?atmosphere:ActionAtmosphereField_Bundled(effect->environment_room));
  if(water&&!p)return false;
  switch (effect->kind) {
    case kActionEffect_TempleGroundMist:
      return a && TempleGroundMist(writer, effect, a, project_point, clip_bounds, userdata);
    case kActionEffect_CaveMist:
      return CaveMist(writer, effect, p, members, project_point, clip_bounds, userdata);
    case kActionEffect_CaveSheen:
      return CaveSheen(writer, effect, p, members, project_point, userdata);
    case kActionEffect_TempleGrit: return a && TempleGrit(writer, effect, a, project_point, userdata);
    case kActionEffect_CaveAmbientLight:
      return a && CaveAmbientLight(writer, effect, a, project_point, clip_bounds, userdata);
    case kActionEffect_LandingDust: return LandingDust(writer, effect, project_point, userdata);
    case kActionEffect_CaveWater:
      return CaveWater(writer, effect, p, members, project_point, userdata);
    case kActionEffect_CaveDrips:
      return CaveDrips(writer, effect, p, members, project_point, userdata);
    case kActionEffect_TempleDust: return a && TempleDust(writer, effect, a, project_point, userdata);
    case kActionEffect_TowerWindowLight:
      return a && TowerLight(writer, effect, a, project_point, clip_bounds, userdata);
    default: return false;
  }
}
