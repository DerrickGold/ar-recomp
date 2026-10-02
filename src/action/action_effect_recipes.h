#ifndef AR_ACTION_EFFECT_RECIPES_H
#define AR_ACTION_EFFECT_RECIPES_H
#include "action_environment_scene.h"
#include <stddef.h>

/* Sparse source overrides. IDs are room + terrain + kind + stable source
 * generation, never an array index or a transient actor address. */
enum { kActionEffectRecipeMax = 256, kActionEffectRecipeMaxBytes = 131072 };
/* Draw placement is independent of the source coordinate system. */
typedef enum ActionEffectAnchor {
  kActionEffectAnchor_Bg1, kActionEffectAnchor_Bg2Point, kActionEffectAnchor_Bg2Raster,
} ActionEffectAnchor;
typedef struct ActionEffectRecipe {
  uint8_t group, room, terrain, kind, enabled, emitter, member, field, water_field, atmosphere_field, moon_field, marsh_field, castle_field, glow_field, arc_field, projectile_field, surface_field;
  ActionEffectActorSelector actor;
  ActionNativeMember shape;
  float x, y, width, height, mist_height;
  uint16_t particles, lifetime;
  uint32_t source;
  ActionEffectTuning tuning;
  ActionEffectParticleStyle particle_style;
  ActionEffectFieldStyle field_style;
  uint8_t travel_y_set, color_end_set;
  uint8_t anchor;
} ActionEffectRecipe;
typedef struct ActionEffectRecipes {
  unsigned count;
  ActionEffectRecipe records[kActionEffectRecipeMax];
  unsigned surface_field_count;
  ActionSurfaceField surface_fields[kActionSurfaceFieldMaxDefinitions];
  unsigned projectile_field_count;
  ActionProjectileField projectile_fields[kActionProjectileFieldMaxDefinitions];
  unsigned arc_field_count;
  ActionArcField arc_fields[kActionArcFieldMaxDefinitions];
  unsigned glow_field_count;
  ActionGlowField glow_fields[kActionGlowFieldMaxDefinitions];
  unsigned castle_field_count;
  ActionCastleField castle_fields[kActionCastleFieldMaxDefinitions];
  unsigned marsh_field_count;
  ActionMarshField marsh_fields[kActionMarshFieldMaxDefinitions];
  unsigned moon_field_count;
  ActionMoonField moon_fields[kActionMoonFieldMaxDefinitions];
  unsigned atmosphere_field_count;
  ActionAtmosphereField atmosphere_fields[kActionAtmosphereFieldMaxDefinitions];
  unsigned water_field_count;
  ActionWaterField water_fields[kActionWaterFieldMaxDefinitions];
  unsigned field_count;
  ActionRayField fields[kActionRayFieldMaxDefinitions];
} ActionEffectRecipes;
const char *ActionEffectRecipes_KindName(unsigned kind);
bool ActionEffectRecipes_ReachSupported(unsigned kind);
bool ActionEffectRecipes_ParticleSupported(unsigned kind);
bool ActionEffectRecipes_IsEmitter(unsigned kind);
bool ActionEffectRecipes_ReplacesRayField(const ActionEffectRecipes *table,
    unsigned group, unsigned room, unsigned terrain);
bool ActionEffectRecipes_NeedsBgMask(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain,unsigned bg);
void ActionEffectRecipes_SurfaceFields(const ActionEffectRecipes *,unsigned,unsigned,unsigned,const ActionSurfaceField **);
bool ActionEffectRecipes_ReplacesGlowField(const ActionEffectRecipes *,unsigned,unsigned,unsigned);
bool ActionEffectRecipes_ReplacesCastleField(const ActionEffectRecipes *,unsigned,unsigned,unsigned);
bool ActionEffectRecipes_ReplacesMarshField(const ActionEffectRecipes *,unsigned,unsigned,unsigned);
bool ActionEffectRecipes_ReplacesMoonField(const ActionEffectRecipes *table,unsigned group,unsigned room,unsigned terrain);
bool ActionEffectRecipes_ReplacesWaterField(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain);
bool ActionEffectRecipes_ReplacesAtmosphereField(const ActionEffectRecipes *table,
    unsigned group,unsigned room,unsigned terrain);
const char *ActionEffectRecipes_PatternName(unsigned pattern);
/* Atomic: on rejection neither the existing table nor captured frames change.
 * error_line receives the first failing line (zero for an allocation failure). */
bool ActionEffectRecipes_Parse(ActionEffectRecipes *table, const char *text,
    size_t size, unsigned *error_line);
void ActionEffectRecipes_Apply(const ActionEffectRecipes *table, unsigned group,
    unsigned room, unsigned terrain, uint16_t clock, const ActionEnvironmentScene *scene, ActionSceneEffectFrame *frame);
#endif
