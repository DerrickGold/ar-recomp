#ifndef AR_UI_CATALOG_H
#define AR_UI_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

/* Host interface language, independent of game content/presentation settings.
 * Values are stable settings indices. European English shares English. */
typedef enum ArUiLocale {
  kArUiLocale_English,
  kArUiLocale_French,
  kArUiLocale_German,
  kArUiLocale_Japanese,
  kArUiLocale_Count,
} ArUiLocale;

const char *ArUiCatalog_LocaleTag(ArUiLocale locale);
ArUiLocale ArUiCatalog_ParseLocale(const char *tag);
/* Static borrowed strings, no allocation or I/O. An unknown key returns the
 * caller's English fallback (or an empty string). Invalid locales use English. */
const char *ArUiCatalog_Text(ArUiLocale locale, const char *key,
                           const char *english_fallback);

typedef struct ArUiTextArgument {
  const char *name;
  const char *value;
} ArUiTextArgument;

/* Named {arguments}, never printf formats. Values are inserted literally and
 * cannot introduce another substitution. Fails without modifying output when
 * input is malformed, a name is missing/duplicated, or capacity is insufficient.
 * Inputs must not alias the destination. No allocation; bounded to 16 arguments
 * and 4096 output bytes including NUL. */
bool ArUiCatalog_Format(char *output, size_t capacity, const char *message,
                        const ArUiTextArgument *arguments, size_t count);

#endif
