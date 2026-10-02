#ifndef AR_ACTION_ATMOSPHERE_FIELD_H
#define AR_ACTION_ATMOSPHERE_FIELD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { kActionAtmosphereFieldMaxDefinitions=8 };
typedef struct ActionAtmosphereField {
#define ATMOSPHERE_VECTOR(symbol,name,width,low,high) float symbol[width];
#include "action_atmosphere_field_properties.inc"
#undef ATMOSPHERE_VECTOR
  /* Decoded once when loading. Contiguous sources keep inner loops bounded
   * and avoid property lookups while drawing. */
  float ambient[8][8],areas[3][4],grit[4][4],tower[3][3];
  uint64_t seen[2];
} ActionAtmosphereField;
bool ActionAtmosphereField_Set(ActionAtmosphereField *field,const char *key,const char *value);
void ActionAtmosphereField_Prepare(ActionAtmosphereField *field);
bool ActionAtmosphereField_Valid(const ActionAtmosphereField *field);
size_t ActionAtmosphereField_Write(const ActionAtmosphereField *field,char *text,size_t capacity);
/* Selection only: all three defaults are complete external recipes. */
const ActionAtmosphereField *ActionAtmosphereField_Bundled(unsigned room);
static inline const float *ActionAtmosphereField_Ambient(const ActionAtmosphereField *field,unsigned index) {
  return index<8?field->ambient[index]:NULL;
}
static inline const float *ActionAtmosphereField_Area(const ActionAtmosphereField *field,unsigned index) {
  return index<3?field->areas[index]:NULL;
}
static inline const float *ActionAtmosphereField_Grit(const ActionAtmosphereField *field,unsigned index) {
  return index<4?field->grit[index]:NULL;
}
static inline const float *ActionAtmosphereField_Tower(const ActionAtmosphereField *field,unsigned index) {
  return index<3?field->tower[index]:NULL;
}
const float *ActionAtmosphereField_Witness(const ActionAtmosphereField *field,unsigned index);
void ActionAtmosphereField_UpgradeExposure(ActionAtmosphereField *,unsigned group,unsigned room);
#endif
