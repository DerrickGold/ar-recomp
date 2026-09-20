#ifndef ACTRAISER_LOCALIZATION_RESOLVED_TEXT_H
#define ACTRAISER_LOCALIZATION_RESOLVED_TEXT_H

#include "actraiser/actraiser_localization_text_style.h"

enum { kActRaiserLocalizationComposeTextCapacity = 4096 };

/* One owned resolution result. Source language, authored boundaries and text
 * annotations travel together through normalization, caching and publication.
 * No member borrows storage from the temporary dialogue session. */
typedef struct ActRaiserResolvedText {
  uint64_t source_revision;
  ActRaiserTextStylePlan styles;
  ArLocalizationTextLanguage language;
  ArTextBidiSpans bidi;
  ArLocalizationTextField live_field;
  uint32_t cluster_count;
  ArLocalizationInlineObjectSnapshot
      inline_objects[kArLocalizationFrameInlineObjectCapacity];
  uint8_t inline_object_count;
  size_t utf8_bytes;
  char utf8[kActRaiserLocalizationComposeTextCapacity];
  uint8_t structural_boundaries[AR_TEXT_BOUNDARY_BYTES(
      kActRaiserLocalizationComposeTextCapacity)];
} ActRaiserResolvedText;

/* Resolve a complete semantic message into caller-owned storage. Callers clear
 * the result before resolution; failure retains the native pixels. */
typedef bool (*ActRaiserLocalizationComposeTextResolver)(
    void *context, const char *semantic_id, ActRaiserResolvedText *text,
    char *error, size_t error_capacity);

/* A surface supplies one captured display value to its authored wrapper. */
typedef bool (*ActRaiserLocalizationFieldResolver)(
    void *context, const char *id, const char *value,
    ActRaiserResolvedText *text, char *error, size_t capacity);

#endif
