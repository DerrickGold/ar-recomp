#ifndef ACTRAISER_LOCALIZATION_RUNTIME_H
#define ACTRAISER_LOCALIZATION_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "localization/localization_frame.h"

/* Game-thread adapter. Enhanced mode is currently enabled by the private
 * AR_LOCALIZATION_PACK manifest path; P9 replaces that bootstrap seam with the
 * persisted overlay catalogue without changing this frame contract. */
void ActRaiserLocalizationRuntime_CaptureFrame(
    ArLocalizationFrame *frame, uint16_t bg3_tilemap_base_words,
    unsigned bg3_tilemap_width_tiles,
    unsigned bg3_tilemap_height_tiles,
    const uint16_t *vram_words,
    size_t vram_word_count);
void ActRaiserLocalizationRuntime_Shutdown(void);

#endif /* ACTRAISER_LOCALIZATION_RUNTIME_H */
