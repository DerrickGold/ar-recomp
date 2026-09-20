#ifndef ACTRAISER_LOCALIZATION_HUD_H
#define ACTRAISER_LOCALIZATION_HUD_H

#include "actraiser/actraiser_localization_compose_state.h"

enum {
  kActRaiserLocalizationHudLabels = 8,
  kActRaiserLocalizationHudValues = 5
};
typedef struct ActRaiserLocalizationHudLabel {
  ActRaiserResolvedText text;
  bool valid;
  uint64_t native_revision;
} ActRaiserLocalizationHudLabel;
typedef struct ActRaiserLocalizationHud {
  bool resolved;
  ActRaiserLocalizationHudLabel labels[kActRaiserLocalizationHudLabels];
  ActRaiserLocalizationHudLabel values[kActRaiserLocalizationHudValues];
} ActRaiserLocalizationHud;

/* Shared native field geometry. Palette and text are supplied by each host. */
typedef struct ActRaiserLocalizationHudPresentation {
  ArTextCellRegion region;
  ArLocalizationTextLayoutKind layout;
  uint8_t font_pixels, top_inset, left_inset, right_inset;
} ActRaiserLocalizationHudPresentation;
bool ActRaiserLocalizationHud_Presentation(
    const char *semantic_id,
    ActRaiserLocalizationHudPresentation *presentation);

/* Recognize the active HUD before exporting its palette for reuse by dialogue.
 * No match leaves the HUD binding unavailable, never bound to dialogue inks. */
void ActRaiserLocalizationHud_CapturePalette(
    ActRaiserTextPalette *palette, uint16_t map_base, const uint16_t *vram,
    size_t vram_count, const uint16_t *cgram, size_t cgram_count);

/* Known USA BG3 status fields only: no screen-wide glyph recognition or
 * gameplay mutation. Invalid/unavailable fields retain the native pixels.
 * Reset resolved on pack changes; text shaping remains presenter-cached. */
void ActRaiserLocalizationHud_Append(
    ActRaiserLocalizationHud *hud, ArLocalizationFrame *frame,
    ArTextCellDestination destination, uint16_t tile_base_words,
    const uint16_t *vram, size_t vram_count, const uint16_t *cgram,
    size_t cgram_count, ActRaiserLocalizationComposeTextResolver resolve,
    ActRaiserLocalizationFieldResolver resolve_value, void *context);

#endif
