#include "localization/interface_text.h"
#include "localization/unicode_grapheme.h"

#include <stdint.h>
#include <string.h>

size_t ArInterfaceText_Append(char *buffer, size_t capacity,
                               const char *input, size_t input_bytes) {
  if (!buffer || !capacity || !input) return 0;
  char *terminator = memchr(buffer, 0, capacity);
  if (!terminator) return 0;
  const size_t used = (size_t)(terminator - buffer);
  const size_t available = capacity - used - 1;
  size_t keep = 0;
  for (size_t offset = 0; offset < input_bytes;) {
    size_t next;
    uint32_t first;
    if (!ArUnicodeGrapheme_Next(input, input_bytes, offset, &first, &next) ||
        !first || next <= offset) return 0;
    if (next <= available) keep = next;
    offset = next;
  }
  if (keep) memmove(buffer + used, input, keep);
  buffer[used + keep] = 0;
  return keep;
}

bool ArInterfaceText_EraseLast(char *buffer, size_t capacity) {
  if (!buffer || !capacity) return false;
  const char *terminator = memchr(buffer, 0, capacity);
  if (!terminator || terminator == buffer) return false;
  const size_t bytes = (size_t)(terminator - buffer);
  size_t last = 0;
  for (size_t offset = 0; offset < bytes;) {
    size_t next;
    if (!ArUnicodeGrapheme_Next(buffer, bytes, offset, NULL, &next) || next <= offset) {
      /* Recover an old malformed buffer one byte at a time, not by discarding
       * an unrelated valid prefix. New appends never create this state. */
      last = bytes - 1;
      break;
    }
    last = offset;
    offset = next;
  }
  buffer[last] = 0;
  return true;
}

static bool WordSpace(unsigned char ch) { return ch == ' ' || ch == '\t'; }

bool ArInterfaceText_WrapLine(const char *text, size_t bytes,
                               size_t max_cells, size_t max_bytes,
                               ArInterfaceTextLine *line) {
  if (!line) return false;
  *line = (ArInterfaceTextLine){0};
  if (!text || !bytes || !max_cells || !max_bytes) return false;
  size_t offset = 0, cells = 0, word_end = 0, word_cells = 0;
  while (offset < bytes && cells < max_cells) {
    size_t next;
    uint32_t first;
    if (!ArUnicodeGrapheme_Next(text, bytes, offset, &first, &next) || next <= offset) {
      next = offset + 1;
      first = '?';
    }
    if (first == '\n' || first == '\r') {
      *line = (ArInterfaceTextLine){offset, next, cells};
      return true;
    }
    if (next > max_bytes) break;
    if (WordSpace((unsigned char)text[offset])) {
      word_end = offset;
      word_cells = cells;
    }
    offset = next;
    ++cells;
  }
  if (!offset) return false;
  /* A word ending exactly at the limit need not be backed up to the previous
   * space. This preserves the original English paragraph layout. */
  if (offset < bytes && !WordSpace((unsigned char)text[offset]) &&
      text[offset] != '\n' && text[offset] != '\r' && word_end) {
    offset = word_end;
    cells = word_cells;
  }
  size_t consumed = offset;
  while (consumed < bytes && WordSpace((unsigned char)text[consumed])) ++consumed;
  /* An explicit break directly after a full row terminates that row rather
   * than inserting a spurious empty one on the next call. */
  if (consumed < bytes && (text[consumed] == '\n' || text[consumed] == '\r')) {
    if (text[consumed++] == '\r' && consumed < bytes && text[consumed] == '\n') ++consumed;
  }
  *line = (ArInterfaceTextLine){offset, consumed, cells};
  return true;
}
