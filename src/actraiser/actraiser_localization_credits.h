#ifndef ACTRAISER_LOCALIZATION_CREDITS_H
#define ACTRAISER_LOCALIZATION_CREDITS_H

#include "actraiser/actraiser_localization_compose_state.h"
#include "actraiser/actraiser_credits.h"

enum {
  kActRaiserCreditsTextCapacity = 4096,
  kActRaiserCreditsFontPixels = 13
};
extern const ArTextCellRegion kActRaiserCreditsRegion;

/* Pure credits row placement and frame geometry, shared by live playback and
 * authoring. Accent output supports legacy hosts; v2 templates own color. */
bool ActRaiserLocalizationCredits_PrepareText(ActRaiserResolvedText *text,
                                              uint32_t *accent_end);
bool ActRaiserLocalizationCredits_AddText(ArLocalizationFrame *frame,
                                          ArTextCellDestination destination,
                                          const ActRaiserResolvedText *text);
typedef struct ActRaiserLocalizationCredits {
  bool resolved, valid;
  uint8_t page;
  uint32_t accent_end;
  ActRaiserResolvedText text;
} ActRaiserLocalizationCredits;

/* US credits scene 08/01 only. `presented_page` comes from the native upload
 * observer; tiles only validate that page's surface, never select its identity.
 * Native code owns fades, hold/input, sequence and tile uploads. Missing
 * ownership or failed text keeps the native page. Pack changes invalidate
 * `resolved` without discarding the native producer's page history. */
void ActRaiserLocalizationCredits_Append(
    ActRaiserLocalizationCredits *credits, ArLocalizationFrame *frame,
    ArTextCellDestination destination, int presented_page,
    uint8_t map_group, uint8_t map_number, uint16_t tile_base_words,
    const uint8_t *wram, size_t wram_bytes,
    const uint16_t *vram, size_t vram_words,
    const uint16_t *cgram, size_t cgram_words,
    ActRaiserLocalizationComposeTextResolver resolve, void *context);

#endif
