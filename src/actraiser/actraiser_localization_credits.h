#ifndef ACTRAISER_LOCALIZATION_CREDITS_H
#define ACTRAISER_LOCALIZATION_CREDITS_H

#include "actraiser/actraiser_localization_compose_state.h"

enum { kActRaiserCreditsPageCount = 20, kActRaiserCreditsTextCapacity = 4096 };
typedef struct ActRaiserLocalizationCredits {
  bool resolved, valid;
  uint8_t page;
  uint32_t clusters, accent_end;
  uint64_t revision;
  ArLocalizationTextLanguage language;
  ArTextBidiSpans bidi;
  size_t bytes;
  char text[kActRaiserCreditsTextCapacity];
  uint8_t boundaries[AR_TEXT_BOUNDARY_BYTES(kActRaiserCreditsTextCapacity)];
} ActRaiserLocalizationCredits;

/* US credits scene 08/01 only. Match the presented map to the resident maps
 * produced by that scene's asset loader; never infer a page from a timer.
 * Native code owns fades, hold/input, sequence and tile uploads. A restore,
 * clear or scene change needs no serialized enhanced state. Pack changes
 * invalidate `resolved`. Failed text keeps the entire native page. */
void ActRaiserLocalizationCredits_Append(
    ActRaiserLocalizationCredits *credits, ArLocalizationFrame *frame,
    ArTextCellDestination destination,
    uint8_t map_group, uint8_t map_number, uint16_t tile_base_words,
    const uint8_t *wram, size_t wram_bytes,
    const uint16_t *vram, size_t vram_words,
    const uint16_t *cgram, size_t cgram_words,
    ActRaiserLocalizationComposeTextResolver resolve, void *context);

#endif
