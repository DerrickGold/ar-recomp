#include "localization/unicode_grapheme.h"

#include <limits.h>

typedef struct ArUnicodeGraphemeRange {
  uint32_t first;
  uint32_t last;
  uint8_t packed_properties;
} ArUnicodeGraphemeRange;

/* Values are intentionally compatible with Unicode Grapheme_Cluster_Break
 * and Indic_Conjunct_Break values emitted by utf8proc 2.11.3. */
typedef enum ArUnicodeGraphemeClass {
  kGraphemeOther = 1,
  kGraphemeCr = 2,
  kGraphemeLf = 3,
  kGraphemeControl = 4,
  kGraphemeExtend = 5,
  kGraphemeL = 6,
  kGraphemeV = 7,
  kGraphemeT = 8,
  kGraphemeLv = 9,
  kGraphemeLvt = 10,
  kGraphemeRegionalIndicator = 11,
  kGraphemeSpacingMark = 12,
  kGraphemePrepend = 13,
  kGraphemeZwj = 14,
  kGraphemeExtendedPictographic = 19,
} ArUnicodeGraphemeClass;

typedef enum ArUnicodeIndicClass {
  kIndicNone = 0,
  kIndicLinker = 1,
  kIndicConsonant = 2,
  kIndicExtend = 3,
} ArUnicodeIndicClass;

#include "localization/unicode_grapheme_data.inc"

static bool IsContinuationByte(unsigned char byte) {
  return (byte & 0xC0u) == 0x80u;
}

bool ArUnicode_DecodeScalar(const char *text, size_t length, size_t offset,
                            uint32_t *scalar, size_t *next_offset) {
  if (!text || offset >= length || !scalar || !next_offset) return false;
  const unsigned char first = (unsigned char)text[offset];
  if (first < 0x80u) {
    *scalar = first;
    *next_offset = offset + 1u;
    return true;
  }
  unsigned count;
  uint32_t value;
  if (first >= 0xC2u && first <= 0xDFu) {
    count = 2;
    value = first & 0x1Fu;
  } else if (first >= 0xE0u && first <= 0xEFu) {
    count = 3;
    value = first & 0x0Fu;
  } else if (first >= 0xF0u && first <= 0xF4u) {
    count = 4;
    value = first & 0x07u;
  } else {
    return false;
  }
  if (count > length - offset) return false;
  for (unsigned index = 1; index < count; ++index) {
    const unsigned char byte = (unsigned char)text[offset + index];
    if (!IsContinuationByte(byte)) return false;
    value = (value << 6u) | (byte & 0x3Fu);
  }
  if ((count == 3 && value < 0x800u) ||
      (count == 4 && value < 0x10000u) || value > 0x10FFFFu ||
      (value >= 0xD800u && value <= 0xDFFFu))
    return false;
  *scalar = value;
  *next_offset = offset + count;
  return true;
}

static uint8_t Properties(uint32_t scalar) {
  size_t low = 0;
  size_t high = sizeof(kUnicodeGraphemeRanges) /
                sizeof(kUnicodeGraphemeRanges[0]);
  while (low < high) {
    const size_t middle = low + (high - low) / 2u;
    const ArUnicodeGraphemeRange *range = &kUnicodeGraphemeRanges[middle];
    if (scalar < range->first) {
      high = middle;
    } else if (scalar > range->last) {
      low = middle + 1u;
    } else {
      return range->packed_properties;
    }
  }
  return kGraphemeOther;
}

static unsigned GraphemeClass(uint8_t properties) {
  return properties & 0x1Fu;
}

static unsigned IndicClass(uint8_t properties) {
  return properties >> 5u;
}

static bool IsControl(unsigned grapheme_class) {
  return grapheme_class == kGraphemeCr ||
         grapheme_class == kGraphemeLf ||
         grapheme_class == kGraphemeControl;
}

static bool BreakBetween(unsigned previous, unsigned current,
                         unsigned regional_indicators,
                         bool extended_pictographic_zwj,
                         bool indic_linker) {
  if (previous == kGraphemeCr && current == kGraphemeLf) return false; /* GB3 */
  if (IsControl(previous) || IsControl(current)) return true;          /* GB4/5 */
  if (previous == kGraphemeL &&
      (current == kGraphemeL || current == kGraphemeV ||
       current == kGraphemeLv || current == kGraphemeLvt))
    return false;                                                       /* GB6 */
  if ((previous == kGraphemeLv || previous == kGraphemeV) &&
      (current == kGraphemeV || current == kGraphemeT))
    return false;                                                       /* GB7 */
  if ((previous == kGraphemeLvt || previous == kGraphemeT) &&
      current == kGraphemeT)
    return false;                                                       /* GB8 */
  if (current == kGraphemeExtend || current == kGraphemeZwj)
    return false;                                                       /* GB9 */
  if (current == kGraphemeSpacingMark) return false;                    /* GB9a */
  if (previous == kGraphemePrepend) return false;                       /* GB9b */
  if (indic_linker) return false;                                       /* GB9c */
  if (extended_pictographic_zwj &&
      current == kGraphemeExtendedPictographic)
    return false;                                                       /* GB11 */
  if (previous == kGraphemeRegionalIndicator &&
      current == kGraphemeRegionalIndicator &&
      (regional_indicators & 1u) != 0u)
    return false;                                                       /* GB12/13 */
  return true;                                                          /* GB999 */
}

bool ArUnicodeGrapheme_Next(const char *text, size_t length, size_t offset,
                            uint32_t *first_scalar, size_t *next_offset) {
  if (!next_offset) return false;
  uint32_t scalar;
  size_t cursor;
  if (!ArUnicode_DecodeScalar(text, length, offset, &scalar, &cursor))
    return false;
  if (first_scalar) *first_scalar = scalar;

  uint8_t properties = Properties(scalar);
  unsigned previous = GraphemeClass(properties);
  unsigned regional_indicators =
      previous == kGraphemeRegionalIndicator ? 1u : 0u;
  enum { kEmojiNone, kEmojiExtended, kEmojiZwj } emoji_state =
      previous == kGraphemeExtendedPictographic
          ? kEmojiExtended : kEmojiNone;
  enum { kIndicStateNone, kIndicStateConsonant, kIndicStateLinker }
      indic_state = IndicClass(properties) == kIndicConsonant
          ? kIndicStateConsonant : kIndicStateNone;

  while (cursor < length) {
    uint32_t following;
    size_t following_end;
    if (!ArUnicode_DecodeScalar(
            text, length, cursor, &following, &following_end))
      return false;
    const uint8_t following_properties = Properties(following);
    const unsigned current = GraphemeClass(following_properties);
    const unsigned current_indic = IndicClass(following_properties);
    if (BreakBetween(previous, current, regional_indicators,
                     emoji_state == kEmojiZwj,
                     indic_state == kIndicStateLinker &&
                         current_indic == kIndicConsonant))
      break;

    if (current == kGraphemeRegionalIndicator) {
      if (regional_indicators < UINT_MAX) ++regional_indicators;
    } else {
      regional_indicators = 0;
    }

    if (current == kGraphemeExtendedPictographic) {
      emoji_state = kEmojiExtended;
    } else if (current == kGraphemeExtend &&
               emoji_state == kEmojiExtended) {
      /* Extended_Pictographic Extend* */
    } else if (current == kGraphemeZwj &&
               emoji_state == kEmojiExtended) {
      emoji_state = kEmojiZwj;
    } else {
      emoji_state = kEmojiNone;
    }

    if (current_indic == kIndicConsonant) {
      indic_state = kIndicStateConsonant;
    } else if (current_indic == kIndicLinker &&
               indic_state != kIndicStateNone) {
      indic_state = kIndicStateLinker;
    } else if (current_indic != kIndicExtend) {
      indic_state = kIndicStateNone;
    }

    previous = current;
    cursor = following_end;
  }
  *next_offset = cursor;
  return true;
}
