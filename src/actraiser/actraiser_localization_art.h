#ifndef ACTRAISER_LOCALIZATION_ART_H
#define ACTRAISER_LOCALIZATION_ART_H

#include "localization/localization_frame.h"

/* Decode a semantically identified native BG3 object. Tile/ROM identity stays
 * inside the adapter; the presenter receives owned RGBA artwork only. */
bool ActRaiserLocalizationArt_Capture(
    ArLocalizationArtwork *art, uint16_t tile_base_words,
    const uint16_t *tile_words, unsigned tile_count,
    const uint16_t *vram, size_t vram_words,
    const uint16_t *cgram, size_t cgram_words);

#endif
