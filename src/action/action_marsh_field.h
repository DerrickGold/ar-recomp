#ifndef AR_ACTION_MARSH_FIELD_H
#define AR_ACTION_MARSH_FIELD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { kActionMarshFieldMaxDefinitions=8, kActionMarshFieldMaxSpans=8 };
typedef struct ActionMarshField {
#define MARSH_VECTOR(symbol,name,width,low,high) float symbol[width];
#include "action_marsh_field_properties.inc"
#undef MARSH_VECTOR
  /* Prepared at load; capture and render never look up property descriptors. */
  float spans[kActionMarshFieldMaxSpans][2], materials[9][5];
  uint64_t seen[2];
} ActionMarshField;
bool ActionMarshField_Set(ActionMarshField *,const char *,const char *);
bool ActionMarshField_Valid(const ActionMarshField *);
void ActionMarshField_Prepare(ActionMarshField *);
size_t ActionMarshField_Write(const ActionMarshField *,char *,size_t);
const ActionMarshField *ActionMarshField_Bundled(void);
#endif
