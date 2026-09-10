#ifndef AR_LOCALIZATION_INTERFACE_TEXT_H
#define AR_LOCALIZATION_INTERFACE_TEXT_H

#include <stdbool.h>
#include <stddef.h>

enum { kArInterfaceTextMaximumBytes = 4096 };

/* Bounded host-interface editing. Append accepts only complete graphemes that
 * fit with a terminator. Malformed/non-NUL-UTF8 input is rejected atomically.
 * Existing valid text is preserved. Return the number of bytes appended. */
size_t ArInterfaceText_Append(char *buffer, size_t capacity,
                               const char *input, size_t input_bytes);
bool ArInterfaceText_EraseLast(char *buffer, size_t capacity);

typedef struct ArInterfaceTextLine {
  size_t bytes;     /* Display this prefix, excluding any consumed newline. */
  size_t consumed;  /* Advance by this much, including following spaces. */
  size_t cells;
} ArInterfaceTextLine;

/* Single-line layout for fixed-cell interface paragraphs, not game dialogue.
 * Prefer ASCII word spaces; otherwise break at a whole grapheme (e.g. CJK).
 * Respect explicit CR/LF/CRLF. Byte and cell limits both apply. False means no
 * input or that even the first cluster cannot fit; it never emits a partial
 * character. Malformed legacy display bytes use the existing one-byte/cell
 * recovery policy so callers can draw their replacement glyph. */
bool ArInterfaceText_WrapLine(const char *text, size_t text_bytes,
                               size_t max_cells, size_t max_bytes,
                               ArInterfaceTextLine *line);

#endif
