#ifndef ACTRAISER_LOCALIZATION_HUD_H
#define ACTRAISER_LOCALIZATION_HUD_H

#include "actraiser/actraiser_localization_compose_state.h"
#include "actraiser/actraiser_hud.h"

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

/* Export the displayed owner's palette for reuse by dialogue. Without a
 * visible, intact HUD the binding is unavailable; affected text retains its
 * native rendering. Stale title/world tiles never supply HUD colors. */
void ActRaiserLocalizationHud_CapturePalette(
    ActRaiserTextPalette *palette, ActRaiserHudOwner owner,
    uint16_t map_base, const uint16_t *vram,
    size_t vram_count, const uint16_t *cgram, size_t cgram_count);

/* Known USA BG3 status fields only: no screen-wide glyph recognition or
 * gameplay mutation. Invalid/unavailable fields retain the native pixels.
 * Reset resolved on pack changes; text shaping remains presenter-cached. */
void ActRaiserLocalizationHud_Append(
    ActRaiserLocalizationHud *hud, ActRaiserHudOwner owner,
    ArLocalizationFrame *frame,
    ArTextCellDestination destination, uint16_t tile_base_words,
    const uint16_t *vram, size_t vram_count, const uint16_t *cgram,
    size_t cgram_count, ActRaiserLocalizationComposeTextResolver resolve,
    ActRaiserLocalizationFieldResolver resolve_value, void *context);

#endif
