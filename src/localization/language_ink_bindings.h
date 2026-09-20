/* Generated ink names; edit semantic-catalog-v1.json. */
#ifndef AR_LANGUAGE_INK_BINDINGS_H
#define AR_LANGUAGE_INK_BINDINGS_H
#include <stdbool.h>
#include <string.h>
static inline bool ArLanguageInkBinding_IsKnown(const char *name) {
  static const char *const names[] = {
    "location.band",
    "location.body",
    "location.shadow",
    "dialogue.band",
    "dialogue.body",
    "dialogue.shadow",
    "hud.band",
    "hud.body",
    "hud.shadow",
    "credits.body",
    "credits.accent",
    "world.band",
    "world.body",
    "world.shadow",
  };
  for (unsigned i = 0; i < sizeof(names)/sizeof(names[0]); ++i)
    if (!strcmp(name, names[i])) return true;
  return false;
}
#endif
