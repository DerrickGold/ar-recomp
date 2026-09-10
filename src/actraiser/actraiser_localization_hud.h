#ifndef ACTRAISER_LOCALIZATION_HUD_H
#define ACTRAISER_LOCALIZATION_HUD_H

#include "actraiser/actraiser_localization_compose_state.h"

enum { kActRaiserLocalizationHudLabels = 8 };
typedef struct ActRaiserLocalizationHudLabel {
  char text[512];
  size_t bytes;
  uint32_t clusters;
  uint64_t revision;
  bool valid;
} ActRaiserLocalizationHudLabel;
typedef struct ActRaiserLocalizationHud {
  bool resolved;
  ActRaiserLocalizationHudLabel labels[kActRaiserLocalizationHudLabels];
} ActRaiserLocalizationHud;

/* Known USA BG3 status fields only: no screen-wide glyph recognition or
 * gameplay mutation. Invalid/unavailable fields retain the native pixels.
 * Reset resolved on pack changes; text shaping remains presenter-cached. */
void ActRaiserLocalizationHud_Append(
    ActRaiserLocalizationHud *hud, ArLocalizationFrame *frame,
    ArTextCellDestination destination, ArTextDirection direction,
    uint16_t tile_base_words,
    const uint16_t *vram, size_t vram_count,
    const uint16_t *cgram, size_t cgram_count,
    ActRaiserLocalizationComposeTextResolver resolve, void *context);

#endif
