#include "ui_catalog.h"

#include <string.h>

typedef struct ArUiCatalogEntry {
  const char *key;
  const char *text[kArUiLocale_Count];
} ArUiCatalogEntry;

#include "ui_catalog_data.inc"

const char *ArUiCatalog_LocaleTag(ArUiLocale locale) {
  static const char *const tags[] = {"en", "fr", "de", "ja"};
  return tags[locale >= 0 && locale < kArUiLocale_Count ? locale : 0];
}

ArUiLocale ArUiCatalog_ParseLocale(const char *tag) {
  if (!tag || !tag[0] || !tag[1]) return kArUiLocale_English;
  if (tag[2] && tag[2] != '-' && tag[2] != '_') return kArUiLocale_English;
  const unsigned char a = (unsigned char)tag[0] | 32;
  const unsigned char b = (unsigned char)tag[1] | 32;
  if (a == 'f' && b == 'r') return kArUiLocale_French;
  if (a == 'd' && b == 'e') return kArUiLocale_German;
  if (a == 'j' && b == 'a') return kArUiLocale_Japanese;
  return kArUiLocale_English;
}

const char *ArUiCatalog_Text(ArUiLocale locale, const char *key,
                           const char *fallback) {
  if (locale < 0 || locale >= kArUiLocale_Count) locale = kArUiLocale_English;
  size_t lo = 0, hi = sizeof(kArUiCatalog) / sizeof(kArUiCatalog[0]);
  while (key && lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int order = strcmp(key, kArUiCatalog[mid].key);
    if (!order) return kArUiCatalog[mid].text[locale];
    if (order < 0) hi = mid;
    else lo = mid + 1;
  }
  return fallback ? fallback : "";
}

static bool ArgumentName(const char *name, size_t bytes) {
  if (!bytes || bytes > 64) return false;
  for (size_t i = 0; i < bytes; ++i)
    if (!(name[i] >= 'a' && name[i] <= 'z') && name[i] != '_' &&
        !(i && name[i] >= '0' && name[i] <= '9')) return false;
  return true;
}

static size_t BoundedLength(const char *text, size_t maximum) {
  size_t bytes = 0;
  while (bytes < maximum && text[bytes]) ++bytes;
  return bytes;
}

static bool FormatPass(char *output, size_t capacity, const char *message,
                       const ArUiTextArgument *args, size_t count) {
  size_t used = 0;
  while (*message) {
    const char *value = message;
    size_t bytes = 1;
    if (*message == '}') return false;
    if (*message == '{') {
      const char *end = strchr(++message, '}');
      if (!end || !ArgumentName(message, (size_t)(end - message))) return false;
      value = NULL;
      for (size_t i = 0; i < count; ++i)
        if (strlen(args[i].name) == (size_t)(end - message) &&
            !memcmp(args[i].name, message, (size_t)(end - message))) {
          value = args[i].value;
          break;
        }
      if (!value) return false;
      bytes = strlen(value);
      message = end + 1;
    } else ++message;
    if (bytes >= capacity - used) return false;
    if (output) memcpy(output + used, value, bytes);
    used += bytes;
  }
  if (output) output[used] = 0;
  return true;
}

bool ArUiCatalog_Format(char *output, size_t capacity, const char *message,
                        const ArUiTextArgument *args, size_t count) {
  if (!output || !capacity || !message || (!args && count) || count > 16)
    return false;
  if (capacity > 4096) capacity = 4096;
  if (BoundedLength(message, 4096) == 4096) return false;
  for (size_t i = 0; i < count; ++i) {
    if (!args[i].name || !args[i].value ||
        !ArgumentName(args[i].name, BoundedLength(args[i].name, 65)) ||
        BoundedLength(args[i].value, 4096) == 4096) return false;
    for (size_t j = 0; j < i; ++j)
      if (!strcmp(args[i].name, args[j].name)) return false;
  }
  return FormatPass(NULL, capacity, message, args, count) &&
      FormatPass(output, capacity, message, args, count);
}
