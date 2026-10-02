#ifndef AR_ACTION_WATER_FIELD_H
#define AR_ACTION_WATER_FIELD_H
#include "render/render_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { kActionWaterFieldMaxPools=3, kActionWaterFieldMaxFalls=4,
       kActionWaterFieldMaxWet=8, kActionWaterFieldMaxWitnesses=4,
       kActionWaterFieldMaxDefinitions=8, kActionWaterContourColumns=33 };
typedef struct ActionCaveWaterRegion { int16_t left, right, surface_y; } ActionCaveWaterRegion;
typedef struct ActionCaveWaterfall { int16_t x,y; } ActionCaveWaterfall;
typedef struct ActionCaveWetSource {
  int16_t x,ceiling_y,landing_y;
  uint8_t ceiling_tile,landing_tile;
  bool water;
  int8_t contour[kActionWaterContourColumns];
} ActionCaveWetSource;
typedef struct ActionWaterField {
#define WATER_VECTOR(symbol, name, width, low, high) float symbol[width];
#include "action_water_field_properties.inc"
#undef WATER_VECTOR
  /* Prepared once at load time; retained frames never convert contours. */
  ActionCaveWaterRegion pools[kActionWaterFieldMaxPools];
  ActionCaveWaterfall falls[kActionWaterFieldMaxFalls];
  ActionCaveWetSource wet[kActionWaterFieldMaxWet];
  uint64_t seen[2];
} ActionWaterField;
bool ActionWaterField_Set(ActionWaterField *field,const char *key,const char *value);
void ActionWaterField_Prepare(ActionWaterField *field);
bool ActionWaterField_Valid(const ActionWaterField *field);
size_t ActionWaterField_Write(const ActionWaterField *field,char *text,size_t capacity);
const ActionWaterField *ActionWaterField_Bundled(void);
static inline ActionCaveWaterRegion ActionWaterField_Pool(const ActionWaterField *f,unsigned i) {
  return i<kActionWaterFieldMaxPools?f->pools[i]:(ActionCaveWaterRegion){0};
}
static inline ActionCaveWaterfall ActionWaterField_Fall(const ActionWaterField *f,unsigned i) {
  return i<kActionWaterFieldMaxFalls?f->falls[i]:(ActionCaveWaterfall){0};
}
static inline ActionCaveWetSource ActionWaterField_Wet(const ActionWaterField *f,unsigned i) {
  return i<kActionWaterFieldMaxWet?f->wet[i]:(ActionCaveWetSource){0};
}
const float *ActionWaterField_Witness(const ActionWaterField *field,unsigned i);
static inline ArRenderColorF ActionWaterField_Color(const float *v) {
  return (ArRenderColorF){v[0],v[1],v[2],v[3]};
}
#endif
