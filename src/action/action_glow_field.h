#ifndef AR_ACTION_GLOW_FIELD_H
#define AR_ACTION_GLOW_FIELD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "action_glow_style.h"
enum {kActionGlowFieldMaxDefinitions=32,kActionGlowFieldMaxSources=16};
typedef struct ActionGlowField {
#define GLOW_VECTOR(symbol,name,width,low,high) float symbol[width];
#include "action_glow_field_properties.inc"
#undef GLOW_VECTOR
  ActionEffectGlowStyle spill,body;
  float sources[kActionGlowFieldMaxSources][3];
  uint64_t seen;
} ActionGlowField;
bool ActionGlowField_Set(ActionGlowField *,const char *,const char *);
bool ActionGlowField_Valid(const ActionGlowField *);
void ActionGlowField_Prepare(ActionGlowField *);
size_t ActionGlowField_Write(const ActionGlowField *,char *,size_t);
const ActionGlowField *ActionGlowField_Bundled(unsigned group,unsigned room);
#endif
