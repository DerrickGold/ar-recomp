#ifndef AR_BUTTON_PROMPT_H
#define AR_BUTTON_PROMPT_H
/* Immutable button names and artwork identities. Hosts capture physical
 * bindings; templates address logical SNES buttons, plus Describe. */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { kArButtonPromptCount = 13, kArButtonPromptLabelCapacity = 96,
       kArButtonPromptIconCapacity = 128 };
static const char *const kArButtonPromptKeys[kArButtonPromptCount] = {
  "b", "y", "select", "start", "up", "down", "left", "right",
  "a", "x", "l", "r", "describe"
};
typedef struct ArButtonPrompt {
  char label[kArButtonPromptLabelCapacity];
  char icon[kArButtonPromptIconCapacity];
} ArButtonPrompt;
typedef struct ArButtonPrompts { ArButtonPrompt buttons[kArButtonPromptCount]; } ArButtonPrompts;
typedef void (*ArCaptureButtonPrompts)(void *context, ArButtonPrompts *prompts);

/* A textual icon fallback is literal data, never template syntax. */
static inline int ArButtonPrompt_Index(const char *name, bool *icon) {
  if (!name || !icon) return -1;
  *icon = !strncmp(name, "icon.button.", 12);
  const char *key = *icon ? name + 12 :
      !strncmp(name, "button.", 7) ? name + 7 : NULL;
  for (int i = 0; key && i < kArButtonPromptCount; ++i)
    if (!strcmp(key, kArButtonPromptKeys[i])) return i;
  return -1;
}
static inline void ArButtonPrompts_Default(ArButtonPrompts *prompts) {
  static const char *const labels[kArButtonPromptCount] = {
    "B", "Y", "Select", "Start", "Up", "Down", "Left", "Right",
    "A", "X", "L", "R", "X"
  };
  static const char *const symbols[kArButtonPromptCount] = {
    "b", "y", "back", "start", "up", "down", "left", "right",
    "a", "x", "l", "r", "x"
  };
  memset(prompts, 0, sizeof(*prompts));
  for (unsigned i = 0; i < kArButtonPromptCount; ++i) {
    snprintf(prompts->buttons[i].label, sizeof(prompts->buttons[i].label), "%s", labels[i]);
    snprintf(prompts->buttons[i].icon, sizeof(prompts->buttons[i].icon),
             "button.glyph.nintendo.%s", symbols[i]);
  }
}

typedef enum ArButtonGlyphFamily {
  kArButtonGlyphFamily_Generic = 0, kArButtonGlyphFamily_Xbox,
  kArButtonGlyphFamily_PlayStation, kArButtonGlyphFamily_Nintendo,
  kArButtonGlyphFamily_Count
} ArButtonGlyphFamily;
typedef enum ArButtonGlyphSymbol {
  kArButtonGlyphSymbol_A = 0, kArButtonGlyphSymbol_B, kArButtonGlyphSymbol_X,
  kArButtonGlyphSymbol_Y, kArButtonGlyphSymbol_Cross, kArButtonGlyphSymbol_Circle,
  kArButtonGlyphSymbol_Square, kArButtonGlyphSymbol_Triangle,
  kArButtonGlyphSymbol_LB, kArButtonGlyphSymbol_RB, kArButtonGlyphSymbol_LT,
  kArButtonGlyphSymbol_RT, kArButtonGlyphSymbol_L1, kArButtonGlyphSymbol_R1,
  kArButtonGlyphSymbol_L2, kArButtonGlyphSymbol_R2, kArButtonGlyphSymbol_L,
  kArButtonGlyphSymbol_R, kArButtonGlyphSymbol_ZL, kArButtonGlyphSymbol_ZR,
  kArButtonGlyphSymbol_Up, kArButtonGlyphSymbol_Down, kArButtonGlyphSymbol_Left,
  kArButtonGlyphSymbol_Right, kArButtonGlyphSymbol_Back, kArButtonGlyphSymbol_Start,
  kArButtonGlyphSymbol_Guide, kArButtonGlyphSymbol_LeftStick,
  kArButtonGlyphSymbol_RightStick, kArButtonGlyphSymbol_Count
} ArButtonGlyphSymbol;
typedef struct ArButtonGlyph { unsigned char family, symbol; } ArButtonGlyph;
static const char *const kArButtonGlyphFamilies[] = {"generic", "xbox", "playstation", "nintendo"};
static const char *const kArButtonGlyphSymbols[] = {
  "a", "b", "x", "y", "cross", "circle", "square", "triangle",
  "lb", "rb", "lt", "rt", "l1", "r1", "l2", "r2", "l", "r", "zl", "zr",
  "up", "down", "left", "right", "back", "start", "guide", "left_stick", "right_stick"
};
static inline bool ArButtonGlyph_IsValid(ArButtonGlyph glyph) {
  return glyph.family < kArButtonGlyphFamily_Count && glyph.symbol < kArButtonGlyphSymbol_Count;
}
static inline bool ArButtonGlyph_Parse(const char *id, ArButtonGlyph *glyph) {
  if (!id || !glyph || strncmp(id, "button.glyph.", 13)) return false;
  const char *family = id + 13, *symbol = strchr(family, '.');
  if (!symbol) return false;
  for (unsigned i = 0; i < kArButtonGlyphFamily_Count; ++i) {
    if (strlen(kArButtonGlyphFamilies[i]) != (size_t)(symbol-family) ||
        strncmp(family, kArButtonGlyphFamilies[i], (size_t)(symbol-family))) continue;
    for (unsigned j = 0; j < kArButtonGlyphSymbol_Count; ++j)
      if (!strcmp(symbol+1, kArButtonGlyphSymbols[j])) {
        *glyph = (ArButtonGlyph){(unsigned char)i, (unsigned char)j}; return true;
      }
  }
  return false;
}
#endif
