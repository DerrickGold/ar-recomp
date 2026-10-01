#ifndef AR_ACTION_EFFECT_RECIPES_H
#define AR_ACTION_EFFECT_RECIPES_H
#include "action_environment_scene.h"
#include <stddef.h>

/* Sparse source overrides. IDs are room + terrain + kind + stable source
 * generation, never an array index or a transient actor address. */
enum { kActionEffectRecipeMax = 256, kActionEffectRecipeMaxBytes = 131072 };
typedef struct ActionEffectRecipe {
  uint8_t group, room, terrain, kind, enabled, emitter;
  float x, y, width, height, mist_height;
  uint16_t particles, lifetime;
  uint32_t source;
  ActionEffectTuning tuning;
  ActionEffectParticleStyle particle_style;
  ActionEffectFieldStyle field_style;
  uint8_t travel_y_set, color_end_set;
} ActionEffectRecipe;
typedef struct ActionEffectRecipes {
  unsigned count;
  ActionEffectRecipe records[kActionEffectRecipeMax];
} ActionEffectRecipes;
const char *ActionEffectRecipes_KindName(unsigned kind);
bool ActionEffectRecipes_ReachSupported(unsigned kind);
bool ActionEffectRecipes_ParticleSupported(unsigned kind);
bool ActionEffectRecipes_IsEmitter(unsigned kind);
const char *ActionEffectRecipes_PatternName(unsigned pattern);
/* Atomic: on rejection neither the existing table nor captured frames change.
 * error_line receives the first failing line (zero for an allocation failure). */
bool ActionEffectRecipes_Parse(ActionEffectRecipes *table, const char *text,
    size_t size, unsigned *error_line);
void ActionEffectRecipes_Apply(const ActionEffectRecipes *table, unsigned group,
    unsigned room, unsigned terrain, uint16_t clock, const ActionEnvironmentScene *scene, ActionSceneEffectFrame *frame);
#endif
