#ifndef AR_TEXT_APPEARANCE_H
#define AR_TEXT_APPEARANCE_H

#include "localization/text_template.h"
#include <string.h>

static inline bool
ArTextSourceOrigin_IsValid(const ArTextSourceOrigin *origin) {
  return origin &&
         memchr(origin->source_path, 0, sizeof(origin->source_path)) &&
         memchr(origin->message_id, 0, sizeof(origin->message_id)) &&
         memchr(origin->resolved_id, 0, sizeof(origin->resolved_id)) &&
         memchr(origin->package_id, 0, sizeof(origin->package_id));
}

static inline bool
ArTextAppearance_IsValid(const ArTextRunAppearance *appearance) {
  return appearance && appearance->font_role[0] &&
         memchr(appearance->font_role, 0, sizeof(appearance->font_role)) &&
         appearance->scale_basis >= 625 && appearance->scale_basis <= 160000 &&
         appearance->band_rgb <= 0xFFFFFF && appearance->body_rgb <= 0xFFFFFF &&
         appearance->shadow_rgb <= 0xFFFFFF;
}

/* A view may borrow spans from a larger source. Only intersecting endpoints
 * need validation against its UTF-8, but every span remains ordered and valid.
 */
static inline bool
ArTextAppearanceSpans_IsValid(const ArTextAppearanceSpan *spans, size_t count,
                              const char *utf8, size_t bytes,
                              uint32_t source_offset) {
  if (count > kArTextTemplateMaximumRuns || (count && !spans) ||
      (bytes && !utf8) || bytes > UINT32_MAX - source_offset)
    return false;
  const uint32_t end = source_offset + (uint32_t)bytes;
  uint32_t previous = 0;
  for (size_t i = 0; i < count; ++i) {
    const ArTextAppearanceSpan *span = &spans[i];
    if (span->start < previous || span->start >= span->end ||
        !ArTextAppearance_IsValid(&span->appearance))
      return false;
    previous = span->end;
    const uint32_t boundaries[] = {span->start, span->end};
    for (unsigned j = 0; j < 2; ++j) {
      const uint32_t at = boundaries[j];
      if (at > source_offset && at < end &&
          ((uint8_t)utf8[at - source_offset] & 0xC0) == 0x80)
        return false;
    }
  }
  return true;
}
#endif
