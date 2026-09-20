/* Generated native ink contract; edit semantic-catalog-v1.json. */
#ifndef ACTRAISER_TEXT_INK_BINDINGS_H
#define ACTRAISER_TEXT_INK_BINDINGS_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum ActRaiserTextInkBinding {
  kActRaiserTextInk_Literal = 0,
  kActRaiserTextInk_LocationBand,
  kActRaiserTextInk_LocationBody,
  kActRaiserTextInk_LocationShadow,
  kActRaiserTextInk_DialogueBand,
  kActRaiserTextInk_DialogueBody,
  kActRaiserTextInk_DialogueShadow,
  kActRaiserTextInk_HudBand,
  kActRaiserTextInk_HudBody,
  kActRaiserTextInk_HudShadow,
  kActRaiserTextInk_CreditsBody,
  kActRaiserTextInk_CreditsAccent,
  kActRaiserTextInk_WorldBand,
  kActRaiserTextInk_WorldBody,
  kActRaiserTextInk_WorldShadow,
  kActRaiserTextInk_Count,
} ActRaiserTextInkBinding;

typedef struct ArLanguageInkBindingContract {
  const char *binding;
  uint8_t index;
  bool hud, location;
} ArLanguageInkBindingContract;

static const ArLanguageInkBindingContract kArLanguageInkBindings[] = {
  {"", 0, false, false},
  {"location.band", 2, false, true},
  {"location.body", 3, false, true},
  {"location.shadow", 1, false, true},
  {"dialogue.band", 2, false, false},
  {"dialogue.body", 3, false, false},
  {"dialogue.shadow", 1, false, false},
  {"hud.band", 2, true, false},
  {"hud.body", 3, true, false},
  {"hud.shadow", 1, true, false},
  {"credits.body", 1, false, false},
  {"credits.accent", 5, false, false},
  {"world.band", 131, false, false},
  {"world.body", 132, false, false},
  {"world.shadow", 129, false, false},
};
#endif
