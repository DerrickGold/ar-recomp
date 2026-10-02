#ifndef AR_ACTION_SURFACE_FIELD_H
#define AR_ACTION_SURFACE_FIELD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "action_glow_style.h"
enum {kActionSurfaceFieldKinds=5,kActionSurfaceFieldMaxDefinitions=32};
typedef struct ActionSurfaceField {
#define SURFACE_VECTOR(symbol,name,width,low,high) float symbol[width];
#include "action_surface_field_properties.inc"
#undef SURFACE_VECTOR
  ActionEffectGlowStyle styles[2];
  float sources[16][7],tiers[4][9];
  uint64_t seen;uint32_t hash;
} ActionSurfaceField;
bool ActionSurfaceField_Set(ActionSurfaceField *,const char *,const char *);
bool ActionSurfaceField_Valid(const ActionSurfaceField *);
void ActionSurfaceField_Prepare(ActionSurfaceField *);
size_t ActionSurfaceField_Write(const ActionSurfaceField *,char *,size_t);
const ActionSurfaceField *ActionSurfaceField_Bundled(unsigned);
int ActionSurfaceField_Index(unsigned kind);
#endif
