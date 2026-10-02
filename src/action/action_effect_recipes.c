#include "action_effect_recipes.h"
#include "action_light_kinds.h"
#include "action_effect_members.h"
#include "action_floor_support.h"
#include "action_authored_particles.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { unsigned kind; const char *name; } kSources[] = {
#define SOURCE(kind, name) {kActionEffect_##kind, name}
  SOURCE(AitosLavaPit,"lava-pit-field"),SOURCE(AitosLavaReservoir,"lava-lake-field"),SOURCE(AitosWaterSplash,"splash-field"),SOURCE(AitosWaterfall,"waterfall-field"),SOURCE(AitosWaterfallMist,"waterfall-mist-field"),
  SOURCE(EnemyFireball,"fireball-field"), SOURCE(FillmoreStatueOrb,"orb-field"), SOURCE(MarahnaFireball,"jungle-fire-field"), SOURCE(AitosLavaFireball,"lava-fire-field"), SOURCE(LightningTrap,"trap-field"), SOURCE(BloodpoolBossLightning,"bolt-field"), SOURCE(CentaurLightning,"centaur-field"), SOURCE(WallTorch,"glow-field"), SOURCE(CastleLight,"castle-field"), SOURCE(BloodpoolWater,"marsh-field"), SOURCE(BloodpoolMoonlight,"moon-field"), SOURCE(TempleDust,"atmosphere-field"), SOURCE(CaveWater,"water-field"), SOURCE(ForestCanopyLight,"ray-field"), SOURCE(AuthoredLight,"soft-light"), SOURCE(AuthoredMotes,"motes"), SOURCE(AuthoredMist,"free-mist"),
  SOURCE(AuthoredFloorMist,"ground-mist"), SOURCE(AuthoredParticleArea,"particle-area"),
  SOURCE(AuthoredFan,"light-fan"), SOURCE(AuthoredWater,"water-surface"),
  SOURCE(AuthoredDrips,"drips"), SOURCE(AuthoredSpray,"waterfall-spray"), SOURCE(AuthoredCloud,"cloud-bank"), SOURCE(AuthoredExposure,"exposure"), SOURCE(AuthoredContour,"wet-contour"), SOURCE(AuthoredTorch,"torch"), SOURCE(AuthoredFlame,"flame"), SOURCE(AuthoredHalo,"halo"), SOURCE(AuthoredGradient,"light-gradient"),
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
static bool SurfaceName(const char *name){return !strcmp(name,"lava-pit-field")||!strcmp(name,"lava-lake-field")||!strcmp(name,"splash-field")||!strcmp(name,"waterfall-field")||!strcmp(name,"waterfall-mist-field");}
static bool ProjectileName(const char *name){return !strcmp(name,"fireball-field")||!strcmp(name,"orb-field")||!strcmp(name,"jungle-fire-field")||!strcmp(name,"lava-fire-field");}
static bool ArcName(const char *name){return !strcmp(name,"trap-field")||!strcmp(name,"bolt-field")||!strcmp(name,"centaur-field");}
const char *ActionEffectRecipes_KindName(unsigned kind) {
  for (unsigned i = 0; i < sizeof(kSources)/sizeof(kSources[0]); ++i)
    if (kSources[i].kind == kind && !ArcName(kSources[i].name) && !ProjectileName(kSources[i].name) && !SurfaceName(kSources[i].name) && strcmp(kSources[i].name,"ray-field") && strcmp(kSources[i].name,"water-field") && strcmp(kSources[i].name,"atmosphere-field") && strcmp(kSources[i].name,"moon-field") && strcmp(kSources[i].name,"marsh-field") && strcmp(kSources[i].name,"castle-field") && strcmp(kSources[i].name,"glow-field")) return kSources[i].name;
  return NULL;
}
bool ActionEffectRecipes_ReachSupported(unsigned kind) { return kind == kActionEffect_WallTorch || kind==kActionEffect_AuthoredTorch; }
bool ActionEffectRecipes_IsEmitter(unsigned kind) {
  return kind == kActionEffect_AuthoredLight || kind == kActionEffect_AuthoredMotes ||
      kind == kActionEffect_AuthoredMist || kind == kActionEffect_AuthoredFloorMist ||
      (kind >= kActionEffect_AuthoredParticleArea && kind <= kActionEffect_AuthoredTorch);
}
bool ActionEffectRecipes_ReplacesRayField(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain) {
  if(!table||table->count>kActionEffectRecipeMax)return false;
  for(unsigned i=0;i<table->count;++i) {
    const ActionEffectRecipe *r=&table->records[i];
    if(r->field&&r->group==group&&r->room==room&&r->terrain==terrain)return true;
  }
  return false;
}
bool ActionEffectRecipes_ReplacesWaterField(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain) {
  if(!table||table->count>kActionEffectRecipeMax)return false;
  for(unsigned i=0;i<table->count;++i) {
    const ActionEffectRecipe *r=&table->records[i];
    if(r->water_field&&r->group==group&&r->room==room&&r->terrain==terrain)return true;
  }
  return false;
}
bool ActionEffectRecipes_ReplacesGlowField(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain) {
  if(!table||table->count>kActionEffectRecipeMax)return false;
  for(unsigned i=0;i<table->count;++i) {
    const ActionEffectRecipe *r=&table->records[i];
    if(r->glow_field&&r->group==group&&r->room==room&&r->terrain==terrain)return true;
  }
  return false;
}
bool ActionEffectRecipes_ReplacesCastleField(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain) {
  if(!table||table->count>kActionEffectRecipeMax)return false;
  for(unsigned i=0;i<table->count;++i) {
    const ActionEffectRecipe *r=&table->records[i];
    if(r->castle_field&&r->group==group&&r->room==room&&r->terrain==terrain)return true;
  }
  return false;
}
bool ActionEffectRecipes_ReplacesMarshField(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain) {
  if(!table||table->count>kActionEffectRecipeMax)return false;
  for(unsigned i=0;i<table->count;++i) {
    const ActionEffectRecipe *r=&table->records[i];
    if(r->marsh_field&&r->group==group&&r->room==room&&r->terrain==terrain)return true;
  }
  return false;
}
bool ActionEffectRecipes_ReplacesMoonField(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain) {
  if(!table||table->count>kActionEffectRecipeMax)return false;
  for(unsigned i=0;i<table->count;++i) {
    const ActionEffectRecipe *r=&table->records[i];
    if(r->moon_field&&r->group==group&&r->room==room&&r->terrain==terrain)return true;
  }
  return false;
}
bool ActionEffectRecipes_ReplacesAtmosphereField(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain) {
  if(!table||table->count>kActionEffectRecipeMax)return false;
  for(unsigned i=0;i<table->count;++i) {
    const ActionEffectRecipe *r=&table->records[i];
    if(r->atmosphere_field&&r->group==group&&r->room==room&&r->terrain==terrain)return true;
  }
  return false;
}
bool ActionEffectRecipes_NeedsBgMask(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain,unsigned bg) {
  if(!table||table->count>kActionEffectRecipeMax||bg>1)return false;
  for(unsigned i=0;i<table->count;++i) {
    const ActionEffectRecipe *r=&table->records[i];
    if(!r->enabled||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    /* Capture only the source/attachment masks actually used by this emitter.
     * Complete fields keep their native contract when exported. */
    if(r->emitter && (bg==(unsigned)(r->anchor!=kActionEffectAnchor_Bg1) ||
        (bg==1 && r->field_style.placement==0) ||
        (bg==0 && r->field_style.placement==1)))return true;
    if(r->surface_field&&((unsigned)table->surface_fields[r->surface_field-1].Components[0]&1))return true;
    if(r->projectile_field&&!bg&&((unsigned)table->projectile_fields[r->projectile_field-1].Receivers[0]&1))return true;
    if(r->arc_field&&!bg&&((unsigned)table->arc_fields[r->arc_field-1].Receivers[0]&1))return true;
    if(r->glow_field&&!bg&&((unsigned)table->glow_fields[r->glow_field-1].Components[0]&1))return true;
    if(r->castle_field&&((unsigned)table->castle_fields[r->castle_field-1].Components[0]&(bg?4:11)))return true;
    if(r->marsh_field&&((unsigned)table->marsh_fields[r->marsh_field-1].Components[0]&(bg?10:15)))return true;
    if(r->moon_field&&(unsigned)table->moon_fields[r->moon_field-1].Components[0])return true; /* BG1 casters and BG2 alpha attachment. */
    if(r->field&&bg==1&&(table->fields[r->field-1].components&7))return true;
    if(r->water_field&&((unsigned)table->water_fields[r->water_field-1].Components[0]&(bg?1:8)))return true;
    if(r->atmosphere_field&&bg==0&&((unsigned)table->atmosphere_fields[r->atmosphere_field-1].Components[0]&16))return true;
    if(bg==0&&r->tuning.light_receivers_set&&(r->tuning.light_receivers&kActionReceiver_Scenery))return true;
  }
  return false;
}
static const char *const kPatterns[]={"motes","dust","leaves","snow","sand","insects","scarabs","sparks"};
const char *ActionEffectRecipes_PatternName(unsigned pattern) {
  return pattern < kActionParticle_Count ? kPatterns[pattern] : NULL;
}
bool ActionEffectRecipes_ParticleSupported(unsigned kind) {
  return kind==kActionEffect_AuthoredFlame || kind==kActionEffect_AuthoredMotes || kind==kActionEffect_AuthoredParticleArea ||
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
    case kActionEffect_AuthoredTorch: return 222;
    case kActionEffect_AuthoredFlame: return 194+4*r->particles;
    case kActionEffect_AuthoredHalo: return 4*32*2*7; /* clipped ring strips */
    case kActionEffect_AuthoredGradient: return 4*4*2*7; /* clipped 5x5 grid */
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
         a->kind == b->kind && a->source == b->source && a->member == b->member && a->field == b->field && a->water_field == b->water_field && a->atmosphere_field == b->atmosphere_field && a->moon_field == b->moon_field && a->marsh_field == b->marsh_field && a->castle_field == b->castle_field && a->glow_field == b->glow_field && a->arc_field == b->arc_field && a->projectile_field == b->projectile_field && a->surface_field == b->surface_field;
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
    char line[1024];
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
          sscanf(s, "[%7[a-z]:%2x:%2x:%1u:%39[a-z-]:%8x]%n", mode, &group, &room, &terrain, name,
                 &source, &used) != 6 ||
          !used || s[used] || group < 1 || group > 7 || room < 1 || room > 8 || terrain > 2 ||
          !Kind(name) ||
          (strcmp(mode, "source") && strcmp(mode, "emitter") && strcmp(mode, "member") && strcmp(mode,"field")) ||
          (!strcmp(mode,"field") &&
           ((strcmp(name,"ray-field") && strcmp(name,"water-field") && strcmp(name,"atmosphere-field") && strcmp(name,"moon-field") && strcmp(name,"marsh-field") && strcmp(name,"castle-field") && strcmp(name,"glow-field") && !ArcName(name) && !ProjectileName(name) && !SurfaceName(name)) ||
            (!strcmp(name,"ray-field")&&next->field_count==kActionRayFieldMaxDefinitions) ||
            (!strcmp(name,"water-field")&&next->water_field_count==kActionWaterFieldMaxDefinitions) ||
            (!strcmp(name,"atmosphere-field")&&next->atmosphere_field_count==kActionAtmosphereFieldMaxDefinitions) ||
            (!strcmp(name,"moon-field")&&next->moon_field_count==kActionMoonFieldMaxDefinitions) ||
            (!strcmp(name,"marsh-field")&&next->marsh_field_count==kActionMarshFieldMaxDefinitions)||
            (!strcmp(name,"castle-field")&&next->castle_field_count==kActionCastleFieldMaxDefinitions)||
            (!strcmp(name,"glow-field")&&next->glow_field_count==kActionGlowFieldMaxDefinitions)||
            (ArcName(name)&&(next->arc_field_count==kActionArcFieldMaxDefinitions||source!=0))||
            (ProjectileName(name)&&(next->projectile_field_count==kActionProjectileFieldMaxDefinitions||source!=0))||
            (SurfaceName(name)&&(next->surface_field_count==kActionSurfaceFieldMaxDefinitions||source!=0)))) ||
          (strcmp(mode,"field") && (!strcmp(name,"ray-field")||!strcmp(name,"water-field")||!strcmp(name,"atmosphere-field")||!strcmp(name,"moon-field")||!strcmp(name,"marsh-field")||!strcmp(name,"castle-field")||!strcmp(name,"glow-field")||ArcName(name)||ProjectileName(name)||SurfaceName(name))) ||
          (!strcmp(mode, "member") &&
           (!source || source > ActionEffectMembers_Count(Kind(name), group, room))) ||
          ((!strcmp(mode, "emitter")) != ActionEffectRecipes_IsEmitter(Kind(name)))) {
        ok = false; break;
      }
      ActionEffectRecipe record = {.group = group,
                                   .room = room,
                                   .terrain = terrain,
                                   .kind = Kind(name),
                                   .source = source,
                                   .enabled = true,
                                   .emitter = !strcmp(mode, "emitter"),
                                   .member = !strcmp(mode, "member"),
                                   .field = !strcmp(mode,"field")&&!strcmp(name,"ray-field") ? next->field_count+1 : 0,
                                   .water_field = !strcmp(mode,"field")&&!strcmp(name,"water-field") ? next->water_field_count+1 : 0,
                                   .surface_field = !strcmp(mode,"field")&&SurfaceName(name) ? next->surface_field_count+1 : 0,
                                   .projectile_field = !strcmp(mode,"field")&&ProjectileName(name) ? next->projectile_field_count+1 : 0,
                                   .arc_field = !strcmp(mode,"field")&&ArcName(name) ? next->arc_field_count+1 : 0,
                                   .glow_field = !strcmp(mode,"field")&&!strcmp(name,"glow-field") ? next->glow_field_count+1 : 0,
                                   .castle_field = !strcmp(mode,"field")&&!strcmp(name,"castle-field") ? next->castle_field_count+1 : 0,
                                   .marsh_field = !strcmp(mode,"field")&&!strcmp(name,"marsh-field") ? next->marsh_field_count+1 : 0,
                                   .moon_field = !strcmp(mode,"field")&&!strcmp(name,"moon-field") ? next->moon_field_count+1 : 0,
                                   .atmosphere_field = !strcmp(mode,"field")&&!strcmp(name,"atmosphere-field") ? next->atmosphere_field_count+1 : 0,
                                   .actor = {.limit=4},
                                   .width = 96,
                                   .height = 64,
                                   .mist_height = 26,
                                   .particles = 24,
                                   .lifetime = 240,
                                   .tuning = {.intensity = 1,
                                              .reach = 1,
                                              .color = 0xffffff,
                                              .active = 1,
                                              .light_receivers = 1,
                                              .dim_receivers = 1}};
      record.shape = (ActionNativeMember){.kind = record.kind,
                                          .index = source ? source - 1 : 0,
                                          .enabled = 1,
                                          .width_scale = 1,
                                          .length_scale = 1,
                                          .intensity = 1,
                                          .color = 0xffffff};
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
      if(record.surface_field)++next->surface_field_count;
      if(record.projectile_field)++next->projectile_field_count;
      if(record.arc_field)++next->arc_field_count;
      if(record.glow_field)++next->glow_field_count;
      if(record.castle_field)++next->castle_field_count;
      if(record.marsh_field)++next->marsh_field_count;
      if(record.moon_field)++next->moon_field_count;
      if(record.field)++next->field_count;
      if(record.water_field)++next->water_field_count;
      if(record.atmosphere_field)++next->atmosphere_field_count;
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
    } else if(current->surface_field) {
      ok=ActionSurfaceField_Set(&next->surface_fields[current->surface_field-1],key,value);
    } else if(current->projectile_field) {
      ok=ActionProjectileField_Set(&next->projectile_fields[current->projectile_field-1],key,value);
    } else if(current->arc_field) {
      ok=ActionArcField_Set(&next->arc_fields[current->arc_field-1],key,value);
    } else if(current->glow_field) {
      ok=ActionGlowField_Set(&next->glow_fields[current->glow_field-1],key,value);
    } else if(current->castle_field) {
      ok=ActionCastleField_Set(&next->castle_fields[current->castle_field-1],key,value);
    } else if(current->marsh_field) {
      ok=ActionMarshField_Set(&next->marsh_fields[current->marsh_field-1],key,value);
    } else if(current->moon_field) {
      ok=ActionMoonField_Set(&next->moon_fields[current->moon_field-1],key,value);
    } else if(current->field) {
      ok=ActionRayField_Set(&next->fields[current->field-1],key,value);
    } else if(current->atmosphere_field) {
      ok=ActionAtmosphereField_Set(&next->atmosphere_fields[current->atmosphere_field-1],key,value);
    } else if(current->water_field) {
      ok=ActionWaterField_Set(&next->water_fields[current->water_field-1],key,value);
    } else if(current->emitter&&!strncmp(key,"actor-",6)) {
      ok=ActionEffectActorSelector_Set(&current->actor,key,value);
    } else if (!strcmp(key,"intensity")) {
      bit = 2; ok = Number(value,0,4,&current->tuning.intensity);
    } else if (!strcmp(key,"reach")) {
      bit = 4; ok = ActionEffectRecipes_ReachSupported(current->kind) &&
          Number(value,.25f,4,&current->tuning.reach);
    } else if (!strcmp(key, "color")) {
      bit = 8;
      ok = Color(value, &current->tuning.color);
    } else if (current->member ||
               (!current->emitter && ActionEffectMembers_MovableSource(current->kind) &&
                (!strcmp(key, "offset-x") || !strcmp(key, "offset-y") ||
                 !strcmp(key, "width-scale") || !strcmp(key, "length-scale") ||
                 !strcmp(key, "angle")))) {
      ActionNativeMember *m = &current->shape;
      if (!strcmp(key, "offset-x")) {
        bit = UINT64_C(1) << 34;
        ok = Number(value, -512, 512, &m->offset_x);
      } else if (!strcmp(key, "offset-y")) {
        bit = UINT64_C(1) << 35;
        ok = Number(value, -512, 512, &m->offset_y);
      } else if (!strcmp(key, "width-scale")) {
        bit = UINT64_C(1) << 36;
        ok = current->kind != kActionEffect_CaveDrips && current->kind != kActionEffect_WallTorch &&
             Number(value, .25f, 2, &m->width_scale);
      } else if (!strcmp(key, "length-scale")) {
        bit = UINT64_C(1) << 37;
        ok = current->kind != kActionEffect_CaveWater && current->kind != kActionEffect_WallTorch &&
             Number(value, .25f, 2, &m->length_scale);
      } else if (!strcmp(key, "angle")) {
        bit = UINT64_C(1) << 38;
        ok = current->member &&
             ActionEffectMembers_Angled(current->kind, current->group, current->room,
                                        current->source) &&
             Number(value, -30, 30, &m->angle);
      } else
        ok = false;
    } else if (!strncmp(key, "light-", 6) || !strncmp(key, "dim-", 4)) {
      const bool light=!strncmp(key,"light-",6);const char *target=key+(light?6:4);
      const unsigned receiver=!strcmp(target,"scenery")?1:!strcmp(target,"player")?2:!strcmp(target,"enemies")?4:0;
      bit=receiver?UINT64_C(1)<<(27+(light?0:3)+(receiver==1?0:receiver==2?1:2)):0;
      ok=receiver && (!strcmp(value,"0")||!strcmp(value,"1")) && (light?LightSupported(current->kind):
          current->kind==kActionEffect_AuthoredExposure||current->kind==kActionEffect_CaveAmbientLight||current->kind==kActionEffect_CastleLight);
      uint8_t *mask=light?&current->tuning.light_receivers:&current->tuning.dim_receivers;
      if(!strcmp(value,"1"))*mask|=receiver;else *mask&=(uint8_t)~receiver;
      if(light)current->tuning.light_receivers_set=1;else current->tuning.dim_receivers_set=1;
    } else if (current->emitter) {
      float value_number = 0;
      if (!strcmp(key,"x")) { bit = 16; ok = Number(value,-2048,16384,&current->x) && current->x == floorf(current->x); }
      else if (!strcmp(key,"y")) { bit = 32; ok = Number(value,-2048,16384,&current->y) && current->y == floorf(current->y); }
      else if (!strcmp(key,"width")) { bit = 64; ok = Number(value,4,(current->kind==kActionEffect_AuthoredParticleArea||current->kind==kActionEffect_AuthoredExposure||current->kind==kActionEffect_AuthoredGradient)?16384:512,&current->width); }
      else if (!strcmp(key,"height")) { bit = 128; ok = Number(value,4,(current->kind==kActionEffect_AuthoredParticleArea||current->kind==kActionEffect_AuthoredExposure||current->kind==kActionEffect_AuthoredGradient)?16384:512,&current->height); }
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
      } else if (!strcmp(key,"anchor")) {
        bit=UINT64_C(1)<<39;ok=false;
        const char *names[]={"bg1","bg2-point","bg2-raster"};
        for(unsigned i=0;i<3;++i)if(!strcmp(value,names[i])){current->anchor=i;ok=true;}
      } else if (!strcmp(key,"placement") && current->kind!=kActionEffect_AuthoredFloorMist && current->kind!=kActionEffect_AuthoredExposure) {
        bit=UINT64_C(1)<<19;ok=false;
        const char *names[]={"background","playfield","foreground"};
        for(unsigned i=0;i<3;++i)if(!strcmp(value,names[i])){current->field_style.placement=i;ok=true;}
      } else if (!strcmp(key,"pattern") && current->kind==kActionEffect_AuthoredParticleArea) {
        bit=UINT64_C(1)<<20;ok=false;
        for(unsigned i=0;i<kActionParticle_Count;++i)if(!strcmp(value,kPatterns[i])){current->field_style.pattern=i;ok=true;}
      } else if (!strcmp(key,"angle") && (current->kind==kActionEffect_AuthoredFan||current->kind==kActionEffect_AuthoredGradient)) {
        bit=UINT64_C(1)<<21;ok=Number(value,-180,180,&current->field_style.angle);
      } else if (!strcmp(key,"fan") && current->kind==kActionEffect_AuthoredFan) {
        bit=UINT64_C(1)<<22;ok=Number(value,0,120,&current->field_style.fan);
      } else if (!strcmp(key,"softness") && (current->kind==kActionEffect_AuthoredFan || current->kind==kActionEffect_AuthoredCloud || current->kind==kActionEffect_AuthoredHalo || current->kind==kActionEffect_AuthoredGradient)) {
        bit=UINT64_C(1)<<23;ok=Number(value,0,1,&current->field_style.softness);
      } else if (!strcmp(key,"strands") && (current->kind==kActionEffect_AuthoredFan || current->kind==kActionEffect_AuthoredCloud)) {
        bit=UINT64_C(1)<<24;ok=Number(value,1,current->kind==kActionEffect_AuthoredCloud?24:32,&value_number)&&value_number==floorf(value_number);
        current->field_style.strands=(uint8_t)value_number;
      } else if (!strcmp(key,"drift") && (current->kind==kActionEffect_AuthoredWater || current->kind==kActionEffect_AuthoredCloud)) {
        bit=UINT64_C(1)<<25;ok=Number(value,-128,128,&current->field_style.drift);
      } else if (!strcmp(key,"amplitude") && (current->kind==kActionEffect_AuthoredWater || current->kind==kActionEffect_AuthoredCloud)) {
        bit=UINT64_C(1)<<26;ok=Number(value,0,32,&current->field_style.amplitude);
      } else if (!strcmp(key,"color-end") && (current->kind==kActionEffect_AuthoredGradient||current->kind==kActionEffect_AuthoredHalo)) {
        bit=262144;ok=Color(value,&current->particle_style.color_end);current->color_end_set=1;
      } else if (ActionEffectRecipes_ParticleSupported(current->kind)) {
        ActionEffectParticleStyle *style=&current->particle_style;
        if (!strcmp(key,"size-min")) {bit=2048;ok=Number(value,.1f,4,&style->size_min);}
        else if (!strcmp(key,"size-max")) {bit=4096;ok=Number(value,.1f,4,&style->size_max);}
        else if (!strcmp(key,"travel-x") && (current->kind==kActionEffect_AuthoredFlame||current->kind==kActionEffect_AuthoredMotes||current->kind==kActionEffect_AuthoredParticleArea)) {bit=8192;ok=Number(value,-512,512,&style->travel_x);}
        else if (!strcmp(key,"travel-y") && (current->kind==kActionEffect_AuthoredFlame||current->kind==kActionEffect_AuthoredMotes||current->kind==kActionEffect_AuthoredParticleArea)) {bit=16384;ok=Number(value,-512,512,&style->travel_y);current->travel_y_set=1;}
        else if (!strcmp(key,"wander") && (current->kind==kActionEffect_AuthoredFlame||current->kind==kActionEffect_AuthoredMotes||current->kind==kActionEffect_AuthoredParticleArea)) {bit=32768;ok=Number(value,0,32,&style->wander);}
        else if (!strcmp(key,"spread") && (current->kind==kActionEffect_AuthoredFlame||current->kind==kActionEffect_AuthoredMotes||current->kind==kActionEffect_AuthoredParticleArea)) {bit=65536;ok=Number(value,0,1,&style->spread);}
        else if (!strcmp(key,"seed")) {bit=131072;ok=UnsignedNumber(value,&style->seed);}
        else if (!strcmp(key,"color-end")) {bit=262144;ok=Color(value,&style->color_end);current->color_end_set=1;}
        else ok=false;
      } else ok = false;
    } else
      ok = false;
    if (fields & bit) ok = false;
    fields |= bit;
  }
  ok = ok && version;
  for (unsigned i=0;ok && i<next->count;++i) {
    ActionEffectRecipe *r=&next->records[i];
    if(r->field||r->water_field||r->atmosphere_field||r->moon_field||r->marsh_field||r->castle_field||r->glow_field||r->arc_field||r->projectile_field||r->surface_field) {
      if(r->castle_field)ActionCastleField_UpgradeExposure(&next->castle_fields[r->castle_field-1],r->group,r->room);
      if(r->atmosphere_field)ActionAtmosphereField_UpgradeExposure(&next->atmosphere_fields[r->atmosphere_field-1],r->group,r->room);
      ok=r->surface_field?(ActionSurfaceField_Valid(&next->surface_fields[r->surface_field-1])&&next->surface_fields[r->surface_field-1].Kind[0]==ActionSurfaceField_Index(r->kind)):r->projectile_field?ActionProjectileField_Valid(&next->projectile_fields[r->projectile_field-1]):r->arc_field?ActionArcField_Valid(&next->arc_fields[r->arc_field-1]):r->glow_field?ActionGlowField_Valid(&next->glow_fields[r->glow_field-1]):r->castle_field?ActionCastleField_Valid(&next->castle_fields[r->castle_field-1]):r->marsh_field?ActionMarshField_Valid(&next->marsh_fields[r->marsh_field-1]):r->moon_field?ActionMoonField_Valid(&next->moon_fields[r->moon_field-1]):r->field?ActionRayField_Valid(&next->fields[r->field-1]):r->water_field?ActionWaterField_Valid(&next->water_fields[r->water_field-1]):ActionAtmosphereField_Valid(&next->atmosphere_fields[r->atmosphere_field-1]);
      for(unsigned j=0;j<i;++j)if(((r->surface_field&&next->records[j].surface_field&&r->kind==next->records[j].kind)||(r->projectile_field&&next->records[j].projectile_field&&r->kind==next->records[j].kind)||(r->arc_field&&next->records[j].arc_field&&r->kind==next->records[j].kind)||(r->glow_field&&next->records[j].glow_field)||(r->castle_field&&next->records[j].castle_field)||(r->marsh_field&&next->records[j].marsh_field)||(r->moon_field&&next->records[j].moon_field)||(r->field&&next->records[j].field)||(r->water_field&&next->records[j].water_field)||(r->atmosphere_field&&next->records[j].atmosphere_field))&&next->records[j].group==r->group&&
        next->records[j].room==r->room&&next->records[j].terrain==r->terrain)ok=false;
      if(!ok)break;
      if(r->atmosphere_field)ActionAtmosphereField_Prepare(&next->atmosphere_fields[r->atmosphere_field-1]);
      if(r->surface_field)ActionSurfaceField_Prepare(&next->surface_fields[r->surface_field-1]);
      if(r->projectile_field)ActionProjectileField_Prepare(&next->projectile_fields[r->projectile_field-1]);
      if(r->arc_field)ActionArcField_Prepare(&next->arc_fields[r->arc_field-1]);
      if(r->glow_field)ActionGlowField_Prepare(&next->glow_fields[r->glow_field-1]);
      if(r->castle_field)ActionCastleField_Prepare(&next->castle_fields[r->castle_field-1]);
      if(r->marsh_field)ActionMarshField_Prepare(&next->marsh_fields[r->marsh_field-1]);
      if(r->moon_field)ActionMoonField_Prepare(&next->moon_fields[r->moon_field-1]);
      if(r->water_field)ActionWaterField_Prepare(&next->water_fields[r->water_field-1]);
    }
    r->shape.enabled = r->enabled;
    if(!ActionEffectActorSelector_Valid(&r->actor) || (r->actor.target&&
        (r->anchor!=kActionEffectAnchor_Bg1 || r->field_style.placement!=2 ||
         r->kind==kActionEffect_AuthoredFloorMist || r->kind==kActionEffect_AuthoredExposure ||
         r->kind==kActionEffect_AuthoredContour || r->kind==kActionEffect_AuthoredWater ||
         r->kind==kActionEffect_AuthoredParticleArea))) {ok=false;break;}
    r->shape.intensity = r->tuning.intensity;
    r->shape.color = r->tuning.color;
    if(r->kind==kActionEffect_AuthoredExposure && r->tuning.intensity>1){ok=false;break;}
    /* Collision-supported mist, exposure regions and traced playfield edges
     * require the terrain coordinate system. Reject incompatible bindings. */
    if(r->anchor!=kActionEffectAnchor_Bg1 &&
        (r->kind==kActionEffect_AuthoredFloorMist || r->kind==kActionEffect_AuthoredExposure ||
         r->kind==kActionEffect_AuthoredContour)){ok=false;break;}
    if(r->kind==kActionEffect_AuthoredContour)for(unsigned j=0;j<r->field_style.point_count;++j)
      if(fabsf(r->field_style.points[j].x)>r->width*.5f || fabsf(r->field_style.points[j].y)>r->height*.5f)ok=false;
    if ((r->kind==kActionEffect_AuthoredGradient||r->kind==kActionEffect_AuthoredHalo)&&!r->color_end_set)
      r->particle_style.color_end=r->tuning.color;
    if (!ActionEffectRecipes_ParticleSupported(r->kind)) continue;
    if (!r->travel_y_set) r->particle_style.travel_y=r->kind==kActionEffect_AuthoredParticleArea?-96:-r->height;
    if (!r->color_end_set) r->particle_style.color_end=r->tuning.color;
    ok=ActionAuthoredParticles_Valid(&r->particle_style);
  }
  /* Bound authored cost independently of native decoration/actor storage.
   * Conservative whole-room limits eliminate camera-dependent eviction. */
  for (unsigned i = 0; ok && i < next->count; ++i) {
    unsigned instances = 0, vertices = 0, members = 0;
    const ActionEffectRecipe *r = &next->records[i];
    for (unsigned j = 0; j < next->count; ++j) {
      const ActionEffectRecipe *other = &next->records[j];
      if (other->group != r->group || other->room != r->room || other->terrain != r->terrain ||
          (!other->emitter && !other->member))
        continue;
      if (other->member) {
        ++members;
        continue;
      }
      if (!other->enabled) continue;
      const unsigned copies=other->actor.target?other->actor.limit:1;
      instances+=copies; vertices+=copies*RecipeCost(other);
    }
    if (members > kActionNativeMemberMax || instances > kActionAuthoredMaxInstances ||
        vertices > kActionAuthoredMaxVertices)
      ok = false;
  }
  if (ok) *table = *next;
  else if (error_line) *error_line = line_number;
  free(next); return ok;
}
void ActionEffectRecipes_SurfaceFields(const ActionEffectRecipes *table,unsigned group,unsigned room,unsigned terrain,const ActionSurfaceField **fields){
  if(!fields)return;for(unsigned i=0;i<kActionSurfaceFieldKinds;++i)fields[i]=NULL;
  if(!table||table->count>kActionEffectRecipeMax)return;
  for(unsigned i=0;i<table->count;++i){const ActionEffectRecipe *r=&table->records[i];
    if(!r->surface_field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    const int index=ActionSurfaceField_Index(r->kind);if(index>=0)fields[index]=&table->surface_fields[r->surface_field-1];}
}
void ActionEffectRecipes_Apply(const ActionEffectRecipes *table, unsigned group,
    unsigned room, unsigned terrain, uint16_t clock, const ActionEnvironmentScene *scene, ActionSceneEffectFrame *frame) {
  if (!table || table->count > kActionEffectRecipeMax || !frame || frame->decoration_overflow ||
      frame->decoration_count > kActionSceneDecorationMaxInstances) return;
  const ActionSurfaceField *surfaces[kActionSurfaceFieldKinds];
  ActionEffectRecipes_SurfaceFields(table,group,room,terrain,surfaces);
  bool recapture=false;
  for(unsigned i=0;i<kActionSurfaceFieldKinds;++i)if(surfaces[i]&&(!(frame->surface_fields_valid&(1u<<i))||frame->surface_fields[i].hash!=surfaces[i]->hash))recapture=true;
  if(recapture&&scene){
    unsigned out=0,visible=0;
    for(unsigned i=0;i<frame->decoration_count;++i){const ActionEffectInstance *e=&frame->decorations[i];
      if(ActionSurfaceField_Index(e->kind)>=0)continue;frame->decorations[out++]=*e;if(e->flags&kActionEffectFlag_Visible)++visible;}
    frame->decoration_count=(uint8_t)out;frame->decoration_visible_count=(uint8_t)visible;frame->surface_fields_valid=0;
    ActionSurfaceField_Capture(scene,frame,surfaces,NULL);
  }
  for(unsigned i=0;i<table->count;++i){const ActionEffectRecipe *r=&table->records[i];
    if(r->surface_field&&!r->enabled&&r->group==group&&r->room==room&&r->terrain==terrain){
      const int index=ActionSurfaceField_Index(r->kind);if(index>=0&&(frame->surface_fields_valid&(1u<<index))){frame->surface_fields[index].Components[0]=0;frame->surface_fields[index].Heat[0]=0;}}}
  frame->projectile_fields_valid=0;
  for(unsigned j=0;j<table->count;++j){const ActionEffectRecipe *r=&table->records[j];
    if(!r->projectile_field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    const int index=ActionProjectileField_Index(r->kind);if(index<0)continue;
    frame->projectile_fields[index]=table->projectile_fields[r->projectile_field-1];frame->projectile_fields_valid|=1u<<index;
    if(!r->enabled)frame->projectile_fields[index].Components[0]=0;
    const unsigned receivers=(unsigned)frame->projectile_fields[index].Receivers[0];
    if(receivers<8)for(unsigned i=0;i<frame->effect_count&&i<kActionSceneEffectMaxInstances;++i)
      if(frame->effects[i].kind==r->kind){frame->effects[i].tuning.light_receivers_set=1;frame->effects[i].tuning.light_receivers=receivers;}
  }
  frame->arc_fields_valid=0;
  for(unsigned j=0;j<table->count;++j){const ActionEffectRecipe *r=&table->records[j];
    if(!r->arc_field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    const int index=ActionArcField_Index(r->kind);if(index<0)continue;
    frame->arc_fields[index]=table->arc_fields[r->arc_field-1];frame->arc_fields_valid|=1u<<index;
    if(!r->enabled)frame->arc_fields[index].Components[0]=0;
    const unsigned receivers=(unsigned)frame->arc_fields[index].Receivers[0];
    if(receivers<8)for(unsigned i=0;i<frame->effect_count&&i<kActionSceneEffectMaxInstances;++i)
      if(frame->effects[i].kind==r->kind){frame->effects[i].tuning.light_receivers_set=1;frame->effects[i].tuning.light_receivers=receivers;}
  }
  for(unsigned j=0;j<table->count;++j) {
    const ActionEffectRecipe *r=&table->records[j];
    if(!r->field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    unsigned out=0,visible=0;
    for(unsigned i=0;i<frame->decoration_count;++i) {
      const ActionEffectInstance *e=&frame->decorations[i];
      if(e->kind==kActionEffect_ForestCanopyLight||e->kind==kActionEffect_ForestLeaves||e->kind==kActionEffect_ForestForwardLight)continue;
      frame->decorations[out++]=*e;if(e->flags&kActionEffectFlag_Visible)++visible;
    }
    frame->decoration_count=(uint8_t)out;frame->decoration_visible_count=(uint8_t)visible;frame->ray_field_valid=0;
    if(r->enabled)ActionRayField_Capture(scene,frame,&table->fields[r->field-1],r->source);
  }
  for(unsigned j=0;j<table->count;++j) {
    const ActionEffectRecipe *r=&table->records[j];
    if(!r->water_field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    unsigned out=0,visible=0;
    for(unsigned i=0;i<frame->decoration_count;++i) {
      const ActionEffectInstance *e=&frame->decorations[i];
      if(e->kind==kActionEffect_CaveWater||e->kind==kActionEffect_CaveDrips||e->kind==kActionEffect_CaveMist||e->kind==kActionEffect_CaveSheen)continue;
      frame->decorations[out++]=*e;if(e->flags&kActionEffectFlag_Visible)++visible;
    }
    frame->decoration_count=(uint8_t)out;frame->decoration_visible_count=(uint8_t)visible;frame->water_field_valid=0;
    if(r->enabled) {
      /* Keep native layer submission order: linked drops precede air dust,
       * spray precedes grit, and contact bursts retain their earlier slots. */
      unsigned insert=out;
      for(unsigned i=0;i<out;++i) {
        const unsigned kind=frame->decorations[i].kind;
        if(kind==kActionEffect_TempleDust||kind==kActionEffect_TowerWindowLight||
           kind==kActionEffect_CaveAmbientLight||kind==kActionEffect_TempleGrit||
           kind==kActionEffect_TempleGroundMist){insert=i;break;}
      }
      ActionWaterField_Capture(scene,frame,&table->water_fields[r->water_field-1],r->source);
      const unsigned added=frame->decoration_count-out;
      if(added&&insert<out) {
        ActionEffectInstance captured[4];
        memcpy(captured,frame->decorations+out,added*sizeof(*captured));
        memmove(frame->decorations+insert+added,frame->decorations+insert,(out-insert)*sizeof(*captured));
        memcpy(frame->decorations+insert,captured,added*sizeof(*captured));
      }
    }
  }
  for(unsigned j=0;j<table->count;++j) {
    const ActionEffectRecipe *r=&table->records[j];
    if(!r->atmosphere_field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    unsigned out=0,visible=0;
    for(unsigned i=0;i<frame->decoration_count;++i) {
      const ActionEffectInstance *e=&frame->decorations[i];
      if(e->kind==kActionEffect_TempleDust||e->kind==kActionEffect_TowerWindowLight||
         e->kind==kActionEffect_CaveAmbientLight||e->kind==kActionEffect_TempleGrit||
         e->kind==kActionEffect_TempleGroundMist)continue;
      frame->decorations[out++]=*e;if(e->flags&kActionEffectFlag_Visible)++visible;
    }
    frame->decoration_count=(uint8_t)out;frame->decoration_visible_count=(uint8_t)visible;frame->atmosphere_field_valid=0;
    if(r->enabled)ActionAtmosphereField_Capture(scene,frame,&table->atmosphere_fields[r->atmosphere_field-1],r->source);
  }
  for(unsigned j=0;j<table->count;++j) {
    const ActionEffectRecipe *r=&table->records[j];
    if(!r->glow_field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    unsigned out=0,visible=0;
    for(unsigned i=0;i<frame->decoration_count;++i) {
      const ActionEffectInstance *e=&frame->decorations[i];
      if(e->kind==kActionEffect_WallTorch)continue;
      frame->decorations[out++]=*e;if(e->flags&kActionEffectFlag_Visible)++visible;
    }
    frame->decoration_count=(uint8_t)out;frame->decoration_visible_count=(uint8_t)visible;frame->glow_field_valid=false;
    if(r->enabled) {
      ActionGlowField_Capture(scene,frame,&table->glow_fields[r->glow_field-1],r->source);
      const unsigned added=frame->decoration_count-out;
      if(added&&out) {
        ActionEffectInstance captured[kActionSceneDecorationMaxInstances];
        memcpy(captured,frame->decorations+out,added*sizeof(*captured));
        memmove(frame->decorations+added,frame->decorations,out*sizeof(*captured));
        memcpy(frame->decorations,captured,added*sizeof(*captured));
      }
    }
  }
  for(unsigned j=0;j<table->count;++j) {
    const ActionEffectRecipe *r=&table->records[j];
    if(!r->castle_field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    unsigned out=0,visible=0;
    for(unsigned i=0;i<frame->decoration_count;++i) {
      const ActionEffectInstance *e=&frame->decorations[i];
      if(e->kind==kActionEffect_CastleLight||e->kind==kActionEffect_CastleMist||e->kind==kActionEffect_CastleSky||e->kind==kActionEffect_CastleWater)continue;
      frame->decorations[out++]=*e;if(e->flags&kActionEffectFlag_Visible)++visible;
    }
    frame->decoration_count=(uint8_t)out;frame->decoration_visible_count=(uint8_t)visible;frame->castle_field_valid=false;
    if(r->enabled)ActionCastleField_Capture(scene,frame,&table->castle_fields[r->castle_field-1],r->source);
  }
  for(unsigned j=0;j<table->count;++j) {
    const ActionEffectRecipe *r=&table->records[j];
    if(!r->marsh_field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    unsigned out=0,visible=0;
    for(unsigned i=0;i<frame->decoration_count;++i) {
      const ActionEffectInstance *e=&frame->decorations[i];
      if(e->kind==kActionEffect_BloodpoolWater||e->kind==kActionEffect_BloodpoolMist||e->kind==kActionEffect_BloodpoolTimber||e->kind==kActionEffect_BloodpoolAir)continue;
      frame->decorations[out++]=*e;if(e->flags&kActionEffectFlag_Visible)++visible;
    }
    frame->decoration_count=(uint8_t)out;frame->decoration_visible_count=(uint8_t)visible;frame->bloodpool.field_valid=false;frame->bloodpool.valid=false;frame->bloodpool.timber_count=frame->bloodpool.post_count=0;
    if(r->enabled)ActionMarshField_Capture(scene,frame,&table->marsh_fields[r->marsh_field-1],r->source);
  }
  for(unsigned j=0;j<table->count;++j) {
    const ActionEffectRecipe *r=&table->records[j];
    if(!r->moon_field||r->group!=group||r->room!=room||r->terrain!=terrain)continue;
    unsigned out=0,visible=0;
    for(unsigned i=0;i<frame->decoration_count;++i) {
      const ActionEffectInstance *e=&frame->decorations[i];
      if(e->kind==kActionEffect_BloodpoolMoonlight||e->kind==kActionEffect_BloodpoolMoonReflection||e->kind==kActionEffect_BloodpoolCloud)continue;
      frame->decorations[out++]=*e;if(e->flags&kActionEffectFlag_Visible)++visible;
    }
    frame->decoration_count=(uint8_t)out;frame->decoration_visible_count=(uint8_t)visible;frame->moon_field_valid=false;
    if(r->enabled)ActionMoonField_Capture(scene,frame,&table->moon_fields[r->moon_field-1],r->source);
  }
  if (scene && scene->vram) {
    bool directional = false;
    for (unsigned i = 0; i < frame->decoration_count; ++i) {
      const unsigned kind = frame->decorations[i].kind;
      directional |= kind == kActionEffect_BloodpoolMoonlight ||
                     kind == kActionEffect_ForestForwardLight;
    }
    for (unsigned i = 0; i < table->count; ++i)
      directional |= table->records[i].emitter &&
                     table->records[i].kind == kActionEffect_AuthoredFan &&
                     table->records[i].group == group && table->records[i].room == room &&
                     table->records[i].terrain == terrain && table->records[i].enabled;
    if (directional) {
      if (!ActionEnvironmentScene_CaptureScenery(scene, &frame->scenery))
        frame->scenery = (ActionMoonlightOcclusion){0};
      if ((group == 2 && room == 1)||frame->moon_field_valid) frame->moonlight = frame->scenery;
    }
  }
  frame->members.count = 0;
  for (unsigned j = 0; j < table->count; ++j) {
    const ActionEffectRecipe *r = &table->records[j];
    if (!r->member || r->group != group || r->room != room || r->terrain != terrain) continue;
    if (frame->members.count == kActionNativeMemberMax) return;
    frame->members.records[frame->members.count++] = r->shape;
  }
  frame->authored_count = 0;
  memset(frame->authored_floor,0,sizeof(frame->authored_floor));
  for (unsigned j = 0; j < table->count; ++j) {
    const ActionEffectRecipe *r = &table->records[j];
    if (!r->emitter || !r->enabled || r->group != group || r->room != room || r->terrain != terrain) continue;
    unsigned matched=0;
    const unsigned candidates=r->actor.target?frame->actor_count:1;
    if(candidates>kActionEffectActorMax)continue;
    for(unsigned candidate=0;candidate<candidates;++candidate) {
    const ActionEffectActor *actor=r->actor.target?&frame->actors[candidate]:NULL;
    if(actor&&!ActionEffectActorSelector_Matches(&r->actor,actor))continue;
    if(actor&&matched++==r->actor.limit)break;
    if (frame->authored_count == kActionAuthoredMaxInstances) { frame->authored_count = 0; return; }
    frame->authored_sources[frame->authored_count] = r->source;
    ActionEffectInstance *e = &frame->authored[frame->authored_count++];
    *e = (ActionEffectInstance){.kind = r->kind, .generation = r->source,
      .pulse_generation = r->source, .world_x = (int16_t)r->x, .world_y = (int16_t)r->y,
      .flags = kActionEffectFlag_Visible |
          (r->anchor==kActionEffectAnchor_Bg2Point ? kActionEffectFlag_StaticAnchor : 0), .obj_priority = 2,
      .projection_plane = r->anchor==kActionEffectAnchor_Bg1 ? kActionEffectProjectionPlane_Bg1 : kActionEffectProjectionPlane_Bg2,
      .render_layer = r->kind == kActionEffect_AuthoredFloorMist ? kActionEffectRenderLayer_Bg1Mist :
          r->kind == kActionEffect_AuthoredMist ? kActionEffectRenderLayer_WorldDust :
          kActionEffectRenderLayer_WorldOverlay,
      .age_ticks = clock, .phase_ticks=clock, .pulse_ticks = clock, .tuning = r->tuning,
      .particle_count = r->particles, .particle_lifetime = r->lifetime,
      .particle_style = r->particle_style, .field_style=r->field_style,
      .geometry = {.kind = kActionEffectGeometry_Rect,
        .data.rect = {-r->width/2,-r->height/2,r->width/2,r->height/2}}};
    if (r->kind!=kActionEffect_AuthoredFloorMist && r->kind!=kActionEffect_AuthoredExposure) {
      const bool alpha=r->kind==kActionEffect_AuthoredMist || r->kind==kActionEffect_AuthoredCloud || r->kind==kActionEffect_AuthoredSpray ||
          (r->kind==kActionEffect_AuthoredParticleArea && r->field_style.pattern>=kActionParticle_Dust &&
           r->field_style.pattern<=kActionParticle_Scarabs);
      e->render_layer=r->field_style.placement==0 ? kActionEffectRenderLayer_Bg2Alpha :
          r->field_style.placement==1 ? (alpha?kActionEffectRenderLayer_Bg1Mist:kActionEffectRenderLayer_Bg1Plane) :
          (alpha?kActionEffectRenderLayer_WorldDust:kActionEffectRenderLayer_WorldOverlay);
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
    if(actor) {
      const int x=actor->x+(int)r->x,y=actor->y+(int)r->y;
      if(x<INT16_MIN||x>INT16_MAX||y<INT16_MIN||y>INT16_MAX){--frame->authored_count;continue;}
      e->world_x=(int16_t)x;e->world_y=(int16_t)y;
      e->generation=r->source^(actor->generation*0x9e3779b9u);e->pulse_generation=e->generation;
      e->particle_style.seed^=actor->generation*0x9e3779b9u;
      e->age_ticks=actor->age;e->pulse_ticks=actor->age;e->phase_ticks=actor->phase_ticks;
      e->velocity_x=actor->vx;e->velocity_y=actor->vy;e->record_address=actor->address;
      e->projection_plane=kActionEffectProjectionPlane_Obj;e->obj_priority=actor->priority;
    }
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
        if (r->emitter || r->member || r->field || r->water_field || r->atmosphere_field || r->moon_field || r->marsh_field || r->castle_field || r->glow_field || r->arc_field || r->projectile_field || r->surface_field || r->group != group || r->room != room ||
            r->terrain != terrain || r->kind != effect->kind ||
            (r->source != effect->generation &&
             !((list || effect->kind == kActionEffect_LandingDust) && r->source == 0)))
          continue;
        if (ActionEffectMembers_MovableSource(effect->kind)) {
          effect->world_x += (int16_t)lroundf(r->shape.offset_x);
          effect->world_y += (int16_t)lroundf(r->shape.offset_y);
          ActionEffectLocalRect *rect = &effect->geometry.data.rect;
          rect->x0 *= r->shape.width_scale;
          rect->x1 *= r->shape.width_scale;
          rect->y0 *= r->shape.length_scale;
          rect->y1 *= r->shape.length_scale;
        }
        effect->tuning=r->tuning;if(!r->enabled)effect->flags&=(uint8_t)~kActionEffectFlag_Visible;break;
      }
      if(effect->flags&kActionEffectFlag_Visible)++visible;
    }
    if(list)frame->visible_count=visible;else frame->decoration_visible_count=visible;
  }
}
