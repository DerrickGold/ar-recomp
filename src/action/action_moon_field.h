#ifndef AR_ACTION_MOON_FIELD_H
#define AR_ACTION_MOON_FIELD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { kActionMoonFieldMaxDefinitions=8, kActionMoonFieldColumns=129, kActionMoonFieldRows=17 };
typedef struct ActionMoonField {
#define MOON_VECTOR(symbol,name,width,low,high) float symbol[width];
#include "action_moon_field_properties.inc"
#undef MOON_VECTOR
  /* Prepared once; contiguous profiles avoid descriptor lookups in hot loops. */
  float low[6][3], middle[5][4];
  float columns[kActionMoonFieldColumns][8], rows[kActionMoonFieldRows][8];
  uint64_t seen[2];
} ActionMoonField;
bool ActionMoonField_Set(ActionMoonField *field,const char *key,const char *value);
void ActionMoonField_Prepare(ActionMoonField *field);
bool ActionMoonField_Valid(const ActionMoonField *field);
size_t ActionMoonField_Write(const ActionMoonField *field,char *text,size_t capacity);
const ActionMoonField *ActionMoonField_Bundled(void);
#endif
