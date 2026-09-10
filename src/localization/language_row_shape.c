#include "localization/language_row_shape.h"

#include <stddef.h>
#include <string.h>

typedef struct RowRule {
  ArLanguageRowShape shape;
  uint8_t first_line, last_line;
  uint16_t field_mask;
  bool native_reserved;
} RowRule;

typedef struct RowBinding {
  const char *id;
  ArLanguageRowShape shape;
} RowBinding;

typedef struct ProfileRowBinding {
  const char *id;
  ArLanguageSourceProfile profile;
  ArLanguageRowShape shape;
} ProfileRowBinding;

#include "localization/language_row_shape_data.inc"

ArLanguageRowShape ArLanguageRowShape_ForRoute(const char *semantic_id) {
  if (!semantic_id) return kArLanguageRowShape_None;
  size_t low = 0, high = sizeof(kRowBindings) / sizeof(kRowBindings[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2;
    const int order = strcmp(semantic_id, kRowBindings[middle].id);
    if (!order) return kRowBindings[middle].shape;
    if (order < 0) high = middle;
    else low = middle + 1;
  }
  return kArLanguageRowShape_None;
}

ArLanguageRowShape ArLanguageRowShape_ForProfile(
    const char *semantic_id, ArLanguageSourceProfile profile) {
  if (!semantic_id) return kArLanguageRowShape_None;
  for (size_t i = 0; i < sizeof(kProfileRowBindings) / sizeof(kProfileRowBindings[0]); ++i) {
    if (kProfileRowBindings[i].profile == profile &&
        !strcmp(kProfileRowBindings[i].id, semantic_id))
      return kProfileRowBindings[i].shape;
  }
  return ArLanguageRowShape_ForRoute(semantic_id);
}

bool ArLanguageRowShape_IsReserved(ArLanguageRowShape shape, uint32_t line) {
  for (size_t i = 0; i < sizeof(kRowRules) / sizeof(kRowRules[0]); ++i) {
    const RowRule *rule = &kRowRules[i];
    if (rule->shape == shape && rule->native_reserved &&
        line >= rule->first_line && line <= rule->last_line) return true;
  }
  return false;
}

bool ArLanguageRowShape_Allows(ArLanguageRowShape shape, uint32_t line,
                               uint32_t fields) {
  if (ArLanguageRowShape_IsReserved(shape, line)) return true;
  if (!fields || fields > 10) return false;
  for (size_t i = 0; i < sizeof(kRowRules) / sizeof(kRowRules[0]); ++i) {
    const RowRule *rule = &kRowRules[i];
    if (rule->shape == shape && line >= rule->first_line &&
        line <= rule->last_line && (rule->field_mask & (1u << fields)))
      return true;
  }
  return false;
}
