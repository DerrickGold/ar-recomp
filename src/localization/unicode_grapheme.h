#ifndef ACTRAISER_LOCALIZATION_UNICODE_GRAPHEME_H
#define ACTRAISER_LOCALIZATION_UNICODE_GRAPHEME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decodes one well-formed UTF-8 scalar. Surrogates, overlong encodings, and
 * values above U+10FFFF are rejected. */
bool ArUnicode_DecodeScalar(const char *text, size_t length, size_t offset,
                            uint32_t *scalar, size_t *next_offset);

/* Returns the byte boundary after one extended grapheme cluster according to
 * UAX #29. The property table is embedded so this has no platform dependency.
 * first_scalar is optional; callers use it to distinguish line breaks. */
bool ArUnicodeGrapheme_Next(const char *text, size_t length, size_t offset,
                            uint32_t *first_scalar, size_t *next_offset);

#endif
