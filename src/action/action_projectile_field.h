#ifndef AR_ACTION_PROJECTILE_FIELD_H
#define AR_ACTION_PROJECTILE_FIELD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "action_glow_style.h"
enum {kActionProjectileFieldMaxDefinitions=32,kActionProjectileFieldKinds=4};
typedef struct ActionProjectileField {
#define PROJECTILE_VECTOR(symbol,name,width,low,high) float symbol[width];
#include "action_projectile_field_properties.inc"
#undef PROJECTILE_VECTOR
  ActionEffectGlowStyle spill,body;
  uint64_t seen;
  uint32_t hash;
} ActionProjectileField;
bool ActionProjectileField_Set(ActionProjectileField *,const char *,const char *);
bool ActionProjectileField_Valid(const ActionProjectileField *);
void ActionProjectileField_Prepare(ActionProjectileField *);
size_t ActionProjectileField_Write(const ActionProjectileField *,char *,size_t);
const ActionProjectileField *ActionProjectileField_Bundled(unsigned index);
int ActionProjectileField_Index(unsigned kind);
#endif
