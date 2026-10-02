#ifndef AR_ACTION_ARC_FIELD_H
#define AR_ACTION_ARC_FIELD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "action_glow_style.h"
enum {kActionArcFieldMaxDefinitions=32,kActionArcFieldKinds=3,kActionArcFieldMaxJoints=25};
typedef struct ActionArcField {
#define ARC_VECTOR(symbol,name,width,low,high) float symbol[width];
#include "action_arc_field_properties.inc"
#undef ARC_VECTOR
  ActionEffectGlowStyle styles[4];
  float paths[8][28];
  uint64_t seen;
  uint32_t hash;
} ActionArcField;
bool ActionArcField_Set(ActionArcField *,const char *,const char *);
bool ActionArcField_Valid(const ActionArcField *);
void ActionArcField_Prepare(ActionArcField *);
size_t ActionArcField_Write(const ActionArcField *,char *,size_t);
const ActionArcField *ActionArcField_Bundled(unsigned index);
/* Recognized binding -> profile. -1 means no arc response. */
int ActionArcField_Index(unsigned kind);
#endif
