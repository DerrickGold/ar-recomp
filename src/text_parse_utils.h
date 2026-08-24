#ifndef TEXT_PARSE_UTILS_H
#define TEXT_PARSE_UTILS_H

#include <string.h>

static inline char *TextParse_TrimLeft(char *text) {
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    text++;
  return text;
}

static inline void TextParse_TrimRight(char *text) {
  size_t length = strlen(text);
  while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                    text[length - 1] == '\r' || text[length - 1] == '\n'))
    text[--length] = 0;
}

/* Strip an INI comment only at the start or after horizontal whitespace. */
static inline void TextParse_StripInlineComment(char *text) {
  for (char *cursor = text; *cursor; cursor++) {
    if ((*cursor == ';' || *cursor == '#') &&
        (cursor == text || cursor[-1] == ' ' || cursor[-1] == '\t')) {
      *cursor = 0;
      break;
    }
  }
  TextParse_TrimRight(text);
}

#endif
