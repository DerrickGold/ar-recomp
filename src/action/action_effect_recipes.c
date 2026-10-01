#include "action_effect_recipes.h"
#include "action_light_kinds.h"
#include "action_floor_support.h"
#include "action_authored_particles.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { unsigned kind; const char *name; } kSources[] = {
#define SOURCE(kind, name) {kActionEffect_##kind, name}
  SOURCE(AuthoredLight,"soft-light"), SOURCE(AuthoredMotes,"motes"), SOURCE(AuthoredMist,"free-mist"),
  SOURCE(AuthoredFloorMist,"ground-mist"), SOURCE(AuthoredParticleArea,"particle-area"),
  SOURCE(AuthoredFan,"light-fan"), SOURCE(AuthoredWater,"water-surface"),
  SOURCE(AuthoredDrips,"drips"), SOURCE(AuthoredSpray,"waterfall-spray"), SOURCE(AuthoredCloud,"cloud-bank"), SOURCE(AuthoredExposure,"exposure"), SOURCE(AuthoredContour,"wet-contour"),
  SOURCE(EnemyFireball,"enemy-fireball"), SOURCE(LightningTrap,"lightning-trap"),
  SOURCE(BloodpoolBossLightning,"boss-lightning"), SOURCE(SwordBeam,"sword-beam"),
  SOURCE(MarahnaFireball,"jungle-fireball"), SOURCE(MarahnaLightningLink,"lightning-link"),
  SOURCE(MarahnaBossLightning,"viper-lightning"), SOURCE(AitosLavaFireball,"lava-fireball"),
  SOURCE(AitosStatueFire,"statue-fire"), SOURCE(AitosMoltenRock,"molten-rock"),
  SOURCE(MinotaurAxe,"minotaur-axe"), SOURCE(FlamingWheel,"flaming-wheel"),
  SOURCE(FlamingWheelProjectile,"wheel-projectile"), SOURCE(IceDragonIceBall,"ice-ball"),
  SOURCE(TanzaraProjectile,"tanzara-projectile"), SOURCE(CentaurLightning,"centaur-lightning"),
  SOURCE(NorthwallBossMagic,"northwall-magic"), SOURCE(LandingDust,"landing-dust"),
  SOURCE(FillmoreStatueOrb,"statue-orb"),
  SOURCE(WallTorch,"wall-torch"), SOURCE(AitosLavaPit,"lava-pit"),
  SOURCE(AitosWaterSplash,"water-splash"), SOURCE(AitosWaterfall,"waterfall"),
  SOURCE(AitosWaterfallMist,"waterfall-mist"), SOURCE(AitosLavaReservoir,"lava-lake"),
  SOURCE(ForestCanopyLight,"forest-canopy"), SOURCE(ForestLeaves,"forest-leaves"),
  SOURCE(ForestForwardLight,"forest-forward"), SOURCE(CaveWater,"cave-water"),
  SOURCE(CaveDrips,"cave-drips"), SOURCE(TempleDust,"temple-dust"),
  SOURCE(TowerWindowLight,"tower-window"), SOURCE(CaveMist,"cave-mist"),
  SOURCE(CaveSheen,"cave-sheen"), SOURCE(CaveAmbientLight,"cave-light"),
  SOURCE(TempleGrit,"temple-grit"), SOURCE(TempleGroundMist,"floor-mist"),
  SOURCE(BloodpoolWater,"blood-water"), SOURCE(BloodpoolMist,"blood-mist"),
  SOURCE(BloodpoolMoonlight,"moonlight"), SOURCE(BloodpoolMoonReflection,"moon-reflection"),
  SOURCE(BloodpoolTimber,"wet-timber"), SOURCE(BloodpoolAir,"marsh-air"),
  SOURCE(BloodpoolCloud,"moon-cloud"), SOURCE(CastleLight,"castle-light"),
  SOURCE(CastleSky,"castle-sky"), SOURCE(CastleMist,"castle-mist"),
  SOURCE(CastleWater,"castle-water"),
#undef SOURCE
};
const char *ActionEffectRecipes_KindName(unsigned kind) {
  for (unsigned i = 0; i < sizeof(kSources)/sizeof(kSources[0]); ++i)
    if (kSources[i].kind == kind) return kSources[i].name;
  return NULL;
}
bool ActionEffectRecipes_ReachSupported(unsigned kind) { return kind == kActionEffect_WallTorch; }
bool ActionEffectRecipes_IsEmitter(unsigned kind) {
  return kind == kActionEffect_AuthoredLight || kind == kActionEffect_AuthoredMotes ||
      kind == kActionEffect_AuthoredMist || kind == kActionEffect_AuthoredFloorMist ||
      (kind >= kActionEffect_AuthoredParticleArea && kind <= kActionEffect_AuthoredContour);
}
static const char *const kPatterns[]={"motes","dust","leaves","snow","sand","insects","scarabs","sparks"};
const char *ActionEffectRecipes_PatternName(unsigned pattern) {
  return pattern < kActionParticle_Count ? kPatterns[pattern] : NULL;
}
bool ActionEffectRecipes_ParticleSupported(unsigned kind) {
  return kind==kActionEffect_AuthoredMotes || kind==kActionEffect_AuthoredParticleArea ||
      kind==kActionEffect_AuthoredWater || kind==kActionEffect_AuthoredDrips || kind==kActionEffect_AuthoredSpray;
}
static bool LightSupported(unsigned kind) {
  return ActionLightKind_Supported(kind);
}
static unsigned RecipeCost(const ActionEffectRecipe *r) {
  if (!r->emitter || !r->enabled) return 0;
  switch (r->kind) {
    case kActionEffect_AuthoredFloorMist: return ((unsigned)ceilf(r->width/16)+1)*kActionAuthoredFloorVerticesPerSpan;
    case kActionEffect_AuthoredLight: return 97;
    case kActionEffect_AuthoredMotes: case kActionEffect_AuthoredDrips: case kActionEffect_AuthoredSpray: return 4*r->particles;
    case kActionEffect_AuthoredParticleArea: return 4*kActionParticleMaxCells*r->particles;
    /* Eight clipped triangles per strand and 48 for the soft backdrop. */
    case kActionEffect_AuthoredFan: return 56*r->field_style.strands+336;
    case kActionEffect_AuthoredWater: return 112+4*r->particles;
    case kActionEffect_AuthoredCloud: return 37*r->field_style.strands;
    case kActionEffect_AuthoredExposure: return 4;
    case kActionEffect_AuthoredContour: return 14*(r->field_style.point_count-1);
    default: return 8*37;
  }
}
static unsigned Kind(const char *name) {
  for (unsigned i = 0; i < sizeof(kSources)/sizeof(kSources[0]); ++i)
    if (!strcmp(kSources[i].name,name)) return kSources[i].kind;
  return 0;
}
static char *Trim(char *s) {
  while (isspace((unsigned char)*s)) ++s;
  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1])) --end;
  *end = 0; return s;
}
static bool Number(const char *s, float low, float high, float *out) {
  char *end; float value = strtof(s,&end);
  if (end == s || *end || !isfinite(value) || value < low || value > high) return false;
  *out = value; return true;
}
static bool UnsignedNumber(const char *s, uint32_t *out) {
  if (!*s) return false;
  uint32_t value=0;
  for (;*s;++s) {
    if (*s<'0' || *s>'9') return false;
    const unsigned digit=(unsigned)(*s-'0');
    if (value>(UINT32_MAX-digit)/10) return false;
    value=value*10+digit;
  }
  *out=value;return true;
}
static bool Color(const char *s, uint32_t *out) {
  if (strlen(s)!=6) return false;
  for (unsigned i=0;i<6;++i) if (!isxdigit((unsigned char)s[i])) return false;
  *out=(uint32_t)strtoul(s,NULL,16);return true;
}
static bool Points(const char *text,ActionEffectFieldStyle *style) {
  style->point_count=0;
  while(*text) {
    if(style->point_count==kActionContourMaxPoints)return false;
    char *end;const float x=strtof(text,&end);
    if(end==text||*end!=','||!isfinite(x)||fabsf(x)>256)return false;
    text=end+1;const float y=strtof(text,&end);
    if(end==text||(*end && *end!=' ')||!isfinite(y)||fabsf(y)>256)return false;
    const unsigned i=style->point_count++;
    if(i && x==style->points[i-1].x && y==style->points[i-1].y)return false;
    style->points[i].x=x;style->points[i].y=y;
    text=end;while(*text==' ')++text;
  }
  return style->point_count>=2;
}
static bool SameSource(const ActionEffectRecipe *a, const ActionEffectRecipe *b) {
  return a->group == b->group && a->room == b->room && a->terrain == b->terrain &&
      a->kind == b->kind && a->source == b->source;
}
bool ActionEffectRecipes_Parse(ActionEffectRecipes *table, const char *text,
    size_t size, unsigned *error_line) {
  if (error_line) *error_line = 0;
  if (!table || !text || size > kActionEffectRecipeMaxBytes || memchr(text,0,size)) return false;
  ActionEffectRecipes *next = calloc(1,sizeof(*next));
  if (!next) return false;
  bool ok = true, header = false, version = false;
  unsigned line_number = 0; uint64_t fields = 0;
  ActionEffectRecipe *current = NULL;
  for (size_t at = 0; at < size && ok;) {
    ++line_number;
    size_t n = 0; while (at+n < size && text[at+n] != '\n') ++n;
    char line[256];
    if (n >= sizeof(line)) { ok = false; break; }
    memcpy(line,text+at,n); line[n] = 0; at += n; if (at < size) ++at;
    line[strcspn(line,";#")] = 0;
    char *s = Trim(line); if (!*s) continue;
    if (*s == '[') {
      if (!strcmp(s,"[effects]")) {
        ok = !header; header = true; current = NULL; continue;
      }
      unsigned group,room,terrain,source; char name[40], mode[8]; int used = 0;
      if (!version || next->count == kActionEffectRecipeMax ||
          sscanf(s,"[%7[a-z]:%2x:%2x:%1u:%39[a-z-]:%8x]%n",
              mode,&group,&room,&terrain,name,&source,&used) != 6 || !used || s[used] ||
          group < 1 || group > 7 || room < 1 || room > 8 || terrain > 2 || !Kind(name) ||
          (strcmp(mode,"source") && strcmp(mode,"emitter")) ||
          ((!strcmp(mode,"emitter")) != ActionEffectRecipes_IsEmitter(Kind(name)))) {
        ok = false; break;
      }
      ActionEffectRecipe record = {.group = group, .room = room, .terrain = terrain,
        .kind = Kind(name), .source = source, .enabled = true,
        .emitter = !strcmp(mode,"emitter"), .width = 96, .height = 64, .mist_height = 26, .particles = 24, .lifetime = 240,
        .tuning = {.intensity = 1, .reach = 1, .color = 0xffffff, .active = 1, .light_receivers=1, .dim_receivers=1}};
      record.field_style=(ActionEffectFieldStyle){.fan=36,.softness=.35f,.amplitude=2,.strands=8,.placement=2};
      if (record.kind==kActionEffect_AuthoredParticleArea) record.particles=4;
      if(record.kind==kActionEffect_AuthoredContour) {
        record.field_style.point_count=2;record.field_style.points[0].x=-32;record.field_style.points[1].x=32;
        record.field_style.placement=1;
      }
      if (record.kind==kActionEffect_AuthoredExposure) record.tuning.intensity=.35f;
      record.particle_style=ActionAuthoredParticles_Default(record.kind==kActionEffect_AuthoredParticleArea?96:record.height,source);
      for (unsigned i = 0; i < next->count; ++i)
        if (SameSource(&next->records[i],&record)) ok = false;
      if (!ok) break;
      current = &next->records[next->count++]; *current = record; fields = 0;
      continue;
    }
    char *equals = strchr(s,'=');
    if (!equals) { ok = false; break; }
    *equals = 0; char *key = Trim(s), *value = Trim(equals+1);
    if (!current) {
      ok = header && !version && !strcmp(key,"version") && !strcmp(value,"1");
      version = ok; continue;
    }
    uint64_t bit = 0;
    if (!strcmp(key,"enabled")) {
      bit = 1; ok = !strcmp(value,"0") || !strcmp(value,"1");
      current->enabled = !strcmp(value,"1");
    } else if (!strcmp(key,"intensity")) {
      bit = 2; ok = Number(value,0,4,&current->tuning.intensity);
    } else if (!strcmp(key,"reach")) {
      bit = 4; ok = ActionEffectRecipes_ReachSupported(current->kind) &&
          Number(value,.25f,4,&current->tuning.reach);
    } else if (!strncmp(key,"light-",6) || !strncmp(key,"dim-",4)) {
      const bool light=!strncmp(key,"light-",6);const char *target=key+(light?6:4);
      const unsigned receiver=!strcmp(target,"scenery")?1:!strcmp(target,"player")?2:!strcmp(target,"enemies")?4:0;
      bit=receiver?UINT64_C(1)<<(27+(light?0:3)+(receiver==1?0:receiver==2?1:2)):0;
      ok=receiver && (!strcmp(value,"0")||!strcmp(value,"1")) && (light?LightSupported(current->kind):
          current->kind==kActionEffect_AuthoredExposure||current->kind==kActionEffect_CaveAmbientLight||current->kind==kActionEffect_CastleLight);
      uint8_t *mask=light?&current->tuning.light_receivers:&current->tuning.dim_receivers;
      if(!strcmp(value,"1"))*mask|=receiver;else *mask&=(uint8_t)~receiver;
      if(light)current->tuning.light_receivers_set=1;else current->tuning.dim_receivers_set=1;
    } else if (!strcmp(key,"color")) {
      bit = 8; ok = Color(value,&current->tuning.color);
    } else if (current->emitter) {
      float value_number = 0;
      if (!strcmp(key,"x")) { bit = 16; ok = Number(value,-2048,16384,&current->x) && current->x == floorf(current->x); }
      else if (!strcmp(key,"y")) { bit = 32; ok = Number(value,-2048,16384,&current->y) && current->y == floorf(current->y); }
      else if (!strcmp(key,"width")) { bit = 64; ok = Number(value,4,(current->kind==kActionEffect_AuthoredParticleArea||current->kind==kActionEffect_AuthoredExposure)?16384:512,&current->width); }
      else if (!strcmp(key,"height")) { bit = 128; ok = Number(value,4,(current->kind==kActionEffect_AuthoredParticleArea||current->kind==kActionEffect_AuthoredExposure)?16384:512,&current->height); }
      else if (!strcmp(key,"particles") && ActionEffectRecipes_ParticleSupported(current->kind)) {
        bit = 256; ok = Number(value,1,current->kind==kActionEffect_AuthoredParticleArea?16:128,&value_number) && value_number == floorf(value_number);
        current->particles = (uint16_t)value_number;
      } else if (!strcmp(key,"mist-height") && current->kind == kActionEffect_AuthoredFloorMist) {
        bit = 1024; ok = Number(value,4,64,&current->mist_height);
      } else if (!strcmp(key,"lifetime")) {
        bit = 512; ok = Number(value,16,4096,&value_number) && value_number == floorf(value_number);
        current->lifetime = (uint16_t)value_number;
      } else if (!strcmp(key,"points") && current->kind==kActionEffect_AuthoredContour) {
        bit=UINT64_C(1)<<33;ok=Points(value,&current->field_style);
      } else if (!strcmp(key,"placement") && current->kind>=kActionEffect_AuthoredParticleArea && current->kind!=kActionEffect_AuthoredExposure) {
        bit=UINT64_C(1)<<19;ok=false;
        const char *names[]={"background","playfield","foreground"};
        for(unsigned i=0;i<3;++i)if(!strcmp(value,names[i])){current->field_style.placement=i;ok=true;}
      } else if (!strcmp(key,"pattern") && current->kind==kActionEffect_AuthoredParticleArea) {
        bit=UINT64_C(1)<<20;ok=false;
        for(unsigned i=0;i<kActionParticle_Count;++i)if(!strcmp(value,kPatterns[i])){current->field_style.pattern=i;ok=true;}
      } else if (!strcmp(key,"angle") && current->kind==kActionEffect_AuthoredFan) {
        bit=UINT64_C(1)<<21;ok=Number(value,-180,180,&current->field_style.angle);
      } else if (!strcmp(key,"fan") && current->kind==kActionEffect_AuthoredFan) {
        bit=UINT64_C(1)<<22;ok=Number(value,0,120,&current->field_style.fan);
      } else if (!strcmp(key,"softness") && (current->kind==kActionEffect_AuthoredFan || current->kind==kActionEffect_AuthoredCloud)) {
        bit=UINT64_C(1)<<23;ok=Number(value,0,1,&current->field_style.softness);
      } else if (!strcmp(key,"strands") && (current->kind==kActionEffect_AuthoredFan || current->kind==kActionEffect_AuthoredCloud)) {
        bit=UINT64_C(1)<<24;ok=Number(value,1,current->kind==kActionEffect_AuthoredCloud?24:32,&value_number)&&value_number==floorf(value_number);
        current->field_style.strands=(uint8_t)value_number;
      } else if (!strcmp(key,"drift") && (current->kind==kActionEffect_AuthoredWater || current->kind==kActionEffect_AuthoredCloud)) {
        bit=UINT64_C(1)<<25;ok=Number(value,-128,128,&current->field_style.drift);
      } else if (!strcmp(key,"amplitude") && (current->kind==kActionEffect_AuthoredWater || current->kind==kActionEffect_AuthoredCloud)) {
        bit=UINT64_C(1)<<26;ok=Number(value,0,32,&current->field_style.amplitude);
      } else if (ActionEffectRecipes_ParticleSupported(current->kind)) {
        ActionEffectParticleStyle *style=&current->particle_style;
        if (!strcmp(key,"size-min")) {bit=2048;ok=Number(value,.1f,4,&style->size_min);}
        else if (!strcmp(key,"size-max")) {bit=4096;ok=Number(value,.1f,4,&style->size_max);}
        else if (!strcmp(key,"travel-x") && (current->kind==kActionEffect_AuthoredMotes||current->kind==kActionEffect_AuthoredParticleArea)) {bit=8192;ok=Number(value,-512,512,&style->travel_x);}
        else if (!strcmp(key,"travel-y") && (current->kind==kActionEffect_AuthoredMotes||current->kind==kActionEffect_AuthoredParticleArea)) {bit=16384;ok=Number(value,-512,512,&style->travel_y);current->travel_y_set=1;}
        else if (!strcmp(key,"wander") && (current->kind==kActionEffect_AuthoredMotes||current->kind==kActionEffect_AuthoredParticleArea)) {bit=32768;ok=Number(value,0,32,&style->wander);}
        else if (!strcmp(key,"spread") && (current->kind==kActionEffect_AuthoredMotes||current->kind==kActionEffect_AuthoredParticleArea)) {bit=65536;ok=Number(value,0,1,&style->spread);}
        else if (!strcmp(key,"seed")) {bit=131072;ok=UnsignedNumber(value,&style->seed);}
        else if (!strcmp(key,"color-end")) {bit=262144;ok=Color(value,&style->color_end);current->color_end_set=1;}
        else ok=false;
      } else ok = false;
    } else ok = false;
    if (fields & bit) ok = false;
    fields |= bit;
  }
  ok = ok && version;
  for (unsigned i=0;ok && i<next->count;++i) {
    ActionEffectRecipe *r=&next->records[i];
    if(r->kind==kActionEffect_AuthoredExposure && r->tuning.intensity>1){ok=false;break;}
    if(r->kind==kActionEffect_AuthoredContour)for(unsigned j=0;j<r->field_style.point_count;++j)
      if(fabsf(r->field_style.points[j].x)>r->width*.5f || fabsf(r->field_style.points[j].y)>r->height*.5f)ok=false;
    if (!ActionEffectRecipes_ParticleSupported(r->kind)) continue;
    if (!r->travel_y_set) r->particle_style.travel_y=r->kind==kActionEffect_AuthoredParticleArea?-96:-r->height;
    if (!r->color_end_set) r->particle_style.color_end=r->tuning.color;
    ok=ActionAuthoredParticles_Valid(&r->particle_style);
  }
  /* Bound authored cost independently of native decoration/actor storage.
   * Conservative whole-room limits eliminate camera-dependent eviction. */
  for (unsigned i = 0; ok && i < next->count; ++i) {
    unsigned instances = 0, vertices = 0;
    const ActionEffectRecipe *r = &next->records[i];
    for (unsigned j = 0; j < next->count; ++j) {
      const ActionEffectRecipe *other = &next->records[j];
      if (other->group != r->group || other->room != r->room || other->terrain != r->terrain ||
          !other->emitter || !other->enabled) continue;
      ++instances; vertices += RecipeCost(other);
    }
    if (instances > kActionAuthoredMaxInstances || vertices > kActionAuthoredMaxVertices) ok = false;
  }
  if (ok) *table = *next;
  else if (error_line) *error_line = line_number;
  free(next); return ok;
}
void ActionEffectRecipes_Apply(const ActionEffectRecipes *table, unsigned group,
    unsigned room, unsigned terrain, uint16_t clock, const ActionEnvironmentScene *scene, ActionSceneEffectFrame *frame) {
  if (!table || table->count > kActionEffectRecipeMax || !frame || frame->decoration_overflow ||
      frame->decoration_count > kActionSceneDecorationMaxInstances) return;
  frame->authored_count = 0;
  memset(frame->authored_floor,0,sizeof(frame->authored_floor));
  for (unsigned j = 0; j < table->count; ++j) {
    const ActionEffectRecipe *r = &table->records[j];
    if (!r->emitter || !r->enabled || r->group != group || r->room != room || r->terrain != terrain) continue;
    if (frame->authored_count == kActionAuthoredMaxInstances) { frame->authored_count = 0; return; }
    ActionEffectInstance *e = &frame->authored[frame->authored_count++];
    *e = (ActionEffectInstance){.kind = r->kind, .generation = r->source,
      .pulse_generation = r->source, .world_x = (int16_t)r->x, .world_y = (int16_t)r->y,
      .flags = kActionEffectFlag_Visible, .obj_priority = 2,
      .projection_plane = kActionEffectProjectionPlane_Bg1,
      .render_layer = r->kind == kActionEffect_AuthoredFloorMist ? kActionEffectRenderLayer_Bg1Mist :
          r->kind == kActionEffect_AuthoredMist ? kActionEffectRenderLayer_WorldDust :
          kActionEffectRenderLayer_WorldOverlay,
      .age_ticks = clock, .pulse_ticks = clock, .tuning = r->tuning,
      .particle_count = r->particles, .particle_lifetime = r->lifetime,
      .particle_style = r->particle_style, .field_style=r->field_style,
      .geometry = {.kind = kActionEffectGeometry_Rect,
        .data.rect = {-r->width/2,-r->height/2,r->width/2,r->height/2}}};
    if (r->kind>=kActionEffect_AuthoredParticleArea) {
      const bool alpha=r->kind==kActionEffect_AuthoredCloud || r->kind==kActionEffect_AuthoredSpray ||
          (r->kind==kActionEffect_AuthoredParticleArea && r->field_style.pattern>=kActionParticle_Dust &&
           r->field_style.pattern<=kActionParticle_Scarabs);
      e->render_layer=r->field_style.placement==0 ? kActionEffectRenderLayer_Bg2Alpha :
          r->field_style.placement==1 ? (alpha?kActionEffectRenderLayer_Bg1Mist:kActionEffectRenderLayer_Bg1Plane) :
          (alpha?kActionEffectRenderLayer_WorldDust:kActionEffectRenderLayer_WorldOverlay);
      e->projection_plane=kActionEffectProjectionPlane_Bg1;
    }
    if(r->kind==kActionEffect_AuthoredExposure) {
      e->render_layer=kActionEffectRenderLayer_Bg1Mist;e->tuning.dim_receivers_set=1;
    }
    if (r->kind == kActionEffect_AuthoredFloorMist) {
      const ActionEffectLocalRect area={r->x-r->width/2,r->y-r->height/2,r->x+r->width/2,r->y+r->height/2};
      ActionEffectFloorField *floor=&frame->authored_floor[frame->authored_count-1];
      ActionFloorSupport_Resolve(scene,&area,r->mist_height,floor);
      if (!floor->count) e->flags &= (uint8_t)~kActionEffectFlag_Visible;
    }
  }
  for(unsigned list=0;list<2;++list) {
    const unsigned count=list?frame->effect_count:frame->decoration_count;
    if(count>(list?kActionSceneEffectMaxInstances:kActionSceneDecorationMaxInstances))continue;
    ActionEffectInstance *entries=list?frame->effects:frame->decorations;
    unsigned visible=0;
    for(unsigned i=0;i<count;++i) {
      ActionEffectInstance *effect=&entries[i];
      for(unsigned j=0;j<table->count;++j) {
        const ActionEffectRecipe *r=&table->records[j];
        if(r->emitter||r->group!=group||r->room!=room||r->terrain!=terrain||r->kind!=effect->kind||
            (r->source!=effect->generation && !((list||effect->kind==kActionEffect_LandingDust)&&r->source==0)))continue;
        effect->tuning=r->tuning;if(!r->enabled)effect->flags&=(uint8_t)~kActionEffectFlag_Visible;break;
      }
      if(effect->flags&kActionEffectFlag_Visible)++visible;
    }
    if(list)frame->visible_count=visible;else frame->decoration_visible_count=visible;
  }
}
