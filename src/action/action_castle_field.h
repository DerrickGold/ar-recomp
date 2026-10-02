#ifndef AR_ACTION_CASTLE_FIELD_H
#define AR_ACTION_CASTLE_FIELD_H
#include "action_moon_field.h"
#include "action_castle_sources.h"
enum { kActionCastleFieldMaxDefinitions=24, kActionCastleFieldMaxSources=16 };
typedef struct ActionCastleArchProfile {
  float rows[31];
  unsigned count;
  float width,min_row,max_row,haze_inset;
} ActionCastleArchProfile;
typedef struct ActionCastleField {
#define CASTLE_VECTOR(symbol,name,width,low,high) float symbol[width];
#include "action_castle_field_properties.inc"
#undef CASTLE_VECTOR
  ActionMoonField moon;
  ActionCastleSource sources[kActionCastleFieldMaxSources];
  ActionCastleArchProfile arches[3];
  uint64_t seen[2];
} ActionCastleField;
bool ActionCastleField_Set(ActionCastleField *,const char *,const char *);
bool ActionCastleField_Valid(const ActionCastleField *);
void ActionCastleField_Prepare(ActionCastleField *);
size_t ActionCastleField_Write(const ActionCastleField *,char *,size_t);
const ActionCastleField *ActionCastleField_Bundled(unsigned room);
static inline const ActionCastleArchProfile *ActionCastleField_Arch(const ActionCastleField *f,unsigned index){return &f->arches[index];}
void ActionCastleField_UpgradeExposure(ActionCastleField *,unsigned group,unsigned room);
#endif
