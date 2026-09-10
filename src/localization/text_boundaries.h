#ifndef AR_LOCALIZATION_TEXT_BOUNDARIES_H
#define AR_LOCALIZATION_TEXT_BOUNDARIES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One bit per UTF-8 byte. A set bit marks an authored table delimiter (`|`
 * or newline), never punctuation supplied by a dynamic value. Buffers use
 * ceil(text_capacity / 8) bytes; callers own and bound their storage. */
#define AR_TEXT_BOUNDARY_BYTES(capacity) (((capacity) + 7u) / 8u)

static inline bool ArTextBoundary_Get(const uint8_t *bits, size_t offset) {
  return bits && (bits[offset / 8u] & (1u << (offset % 8u))) != 0;
}

static inline void ArTextBoundary_Set(uint8_t *bits, size_t offset, bool value) {
  const uint8_t mask = (uint8_t)(1u << (offset % 8u));
  if (value) bits[offset / 8u] |= mask;
  else bits[offset / 8u] &= (uint8_t)~mask;
}

#endif /* AR_LOCALIZATION_TEXT_BOUNDARIES_H */
