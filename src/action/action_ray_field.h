#ifndef AR_ACTION_RAY_FIELD_H
#define AR_ACTION_RAY_FIELD_H
#include "render/render_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { kActionRayFieldMaxRays=12, kActionRayFieldMaxFans=4, kActionRayFieldMaxProfiles=2,
       kActionRayFieldRows=6, kActionRayFieldMaxWitnesses=2, kActionRayFieldMaxDefinitions=8 };
typedef enum ActionRayProperty {
#define RAY_PROPERTY(symbol, name, low, high) kActionRay_##symbol,
#include "action_ray_field_properties.inc"
#undef RAY_PROPERTY
  kActionRay_PropertyCount
} ActionRayProperty;
typedef struct ActionRayOpening { float x, half_width, strength, shoulder; unsigned fan, profile; } ActionRayOpening;
typedef struct ActionRayProfile {
  float rows[kActionRayFieldRows];
  float front_start, front_range;
  ArRenderColorF rear, front;
} ActionRayProfile;
typedef struct ActionRayWitness { unsigned bg; int x,y; uint8_t tile; } ActionRayWitness;
typedef struct ActionRayField {
  float values[kActionRay_PropertyCount];
  ActionRayOpening rays[kActionRayFieldMaxRays];
  ArRenderPointF fans[kActionRayFieldMaxFans+1];
  ActionRayProfile profiles[kActionRayFieldMaxProfiles];
  ArRenderColorF mote_color, cluster_color, leaf_color, leaf_rim_color;
  ArRenderPointF leaf_shape[6];
  int dimensions[4];
  ActionRayWitness witnesses[kActionRayFieldMaxWitnesses];
  float anchor[2], window[4], bounds[4];
  uint32_t pulse_seed;
  unsigned ray_count, fan_count, profile_count, witness_count, components;
  uint64_t seen[2];
} ActionRayField;
bool ActionRayField_Set(ActionRayField *field, const char *key, const char *value);
bool ActionRayField_Valid(const ActionRayField *field);
size_t ActionRayField_Write(const ActionRayField *field, char *text, size_t capacity);
const ActionRayField *ActionRayField_Bundled(void);
#endif
