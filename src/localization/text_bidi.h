#ifndef AR_LOCALIZATION_TEXT_BIDI_H
#define AR_LOCALIZATION_TEXT_BIDI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum ArTextDirection {
  kArTextDirection_Auto = 0,
  kArTextDirection_LeftToRight,
  kArTextDirection_RightToLeft,
} ArTextDirection;

enum { kArTextMaximumBidiSpans = 256 };

/* A semantic insertion, in original logical UTF-8 bytes, half-open. Flat,
 * ordered and nonoverlapping. Auto isolates a name by its own first strong
 * character; numbers use LTR and terms use their effective source direction.
 * No control bytes or artificial reveal steps are added to source text. */
typedef struct ArTextBidiSpan {
  uint32_t start, end;
  ArTextDirection direction;
} ArTextBidiSpan;

typedef struct ArTextBidiSpans {
  uint16_t count;
  ArTextBidiSpan spans[kArTextMaximumBidiSpans];
} ArTextBidiSpans;

/* Relocate annotations with an already validated byte edit. Insertions at an
 * endpoint belong outside the old span, just like native inline objects. */
static inline void ArTextBidiSpans_Edit(ArTextBidiSpans *spans,
    uint32_t offset, uint32_t removed, uint32_t inserted) {
  if (!spans) return;
  uint16_t kept = 0;
  for (uint16_t i = 0; i < spans->count; ++i) {
    ArTextBidiSpan s = spans->spans[i];
    if (s.start >= offset + removed) s.start = s.start - removed + inserted;
    else if (s.start > offset) s.start = offset + inserted;
    if (s.end > offset) s.end = s.end >= offset + removed
        ? s.end - removed + inserted : offset;
    if (s.start < s.end) spans->spans[kept++] = s;
  }
  spans->count = kept;
}

/* Validate ranges against an optionally sliced source. Spans outside the view
 * are legal; intersecting ends must coincide with complete UTF-8 scalars. */
static inline bool ArTextBidiSpans_Valid(const ArTextBidiSpan *spans, size_t count,
    const char *text, size_t bytes, size_t source_offset) {
  if (count > kArTextMaximumBidiSpans || (count && !spans) ||
      (bytes && !text) ||
      source_offset > UINT32_MAX || bytes > UINT32_MAX - source_offset)
    return false;
  uint32_t previous = 0;
  for (size_t i = 0; i < count; ++i) {
    const ArTextBidiSpan s = spans[i];
    if (s.start < previous || s.start >= s.end ||
        s.direction < kArTextDirection_Auto || s.direction > kArTextDirection_RightToLeft)
      return false;
    if (s.start > source_offset && s.start < source_offset + bytes &&
        ((uint8_t)text[s.start - source_offset] & 0xc0u) == 0x80u) return false;
    if (s.end > source_offset && s.end < source_offset + bytes &&
        ((uint8_t)text[s.end - source_offset] & 0xc0u) == 0x80u) return false;
    previous = s.end;
  }
  return true;
}

static inline bool ArTextBidiSpans_FitSource(const ArTextBidiSpans *spans,
                                            const char *text, size_t bytes) {
  return spans && spans->count <= kArTextMaximumBidiSpans &&
      (!spans->count || spans->spans[spans->count - 1].end <= bytes) &&
      ArTextBidiSpans_Valid(spans->spans, spans->count, text, bytes, 0);
}

#endif
