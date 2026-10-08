#ifndef AR_ABILITY_NAME_H
#define AR_ABILITY_NAME_H

#include "localization/language_pack.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* A named ability uses its existing translated menu label. Flatten native
 * tile-row padding; the containing dialogue supplies font and treatment. */
typedef struct ArAbilityName {
  const char *placeholder, *message_id, *native_name;
} ArAbilityName;
#include "localization/ability_name_data.inc"

static inline const ArAbilityName *ArAbilityName_Find(const char *name) {
  for (size_t i = 0; name && i < sizeof(kArAbilityNames) / sizeof(kArAbilityNames[0]); ++i)
    if (!strcmp(name, kArAbilityNames[i].placeholder)) return &kArAbilityNames[i];
  return NULL;
}

static inline bool ArAbilityName_Copy(const ArLanguagePack *selected,
    const ArLanguagePack *fallback, const char *name, char *text, size_t capacity) {
  const ArAbilityName *ability = ArAbilityName_Find(name);
  if (!ability || !text || !capacity) return false;
  const ArLanguagePack *pack = selected;
  const ArLanguageMessage *message = selected
      ? ArLanguagePack_FindMessage(selected, ability->message_id) : NULL;
  if (!message) {
    pack = fallback;
    message = fallback ? ArLanguagePack_FindMessage(fallback, ability->message_id) : NULL;
  }
  if (!message) {
    const size_t bytes = strlen(ability->native_name);
    if (bytes >= capacity) return false;
    memcpy(text, ability->native_name, bytes + 1);
    return true;
  }
  for (uint32_t depth = 0; message->is_alias; ++depth) {
    if (depth >= ArLanguagePack_MessageCount(pack)) return false;
    message = ArLanguagePack_FindMessage(pack, ArLanguagePack_GetString(pack, message->alias));
    if (!message) return false;
  }
  size_t used = 0;
  bool space = false;
  for (uint32_t i = 0; i < message->operation_count; ++i) {
    const ArLanguageOperation *op = ArLanguagePack_GetOperation(pack, message, i);
    if (!op) return false;
    if (op->kind == kArLanguageOperation_End) break;
    if (op->kind == kArLanguageOperation_LineBreak ||
        op->kind == kArLanguageOperation_PreferredLineBreak) {
      space = used != 0;
      continue;
    }
    if (op->kind != kArLanguageOperation_Text) return false;
    const char *part = ArLanguagePack_GetString(pack, op->value.text);
    if (!part) return false;
    for (size_t j = 0; j < op->value.text.length; ++j) {
      const char byte = part[j];
      if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
        space = used != 0;
        continue;
      }
      if (used + (space ? 1u : 0u) + 1u >= capacity) return false;
      if (space) text[used++] = ' ';
      space = false;
      text[used++] = byte;
    }
  }
  text[used] = 0;
  return used != 0;
}
#endif
