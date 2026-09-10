#ifndef ACTRAISER_LOCALIZATION_RUNTIME_H
#define ACTRAISER_LOCALIZATION_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "localization/localization_frame.h"
#include "localization/text_presentation.h"

/* Game-thread adapter. Persisted localization settings select native/enhanced
 * presentation and font style. AR_LOCALIZATION_PACK still supplies the optional
 * selected manifest until installed-pack discovery replaces that bootstrap
 * seam; the native enhanced source is loaded separately. */
/* The copied host/context must remain valid until replaced. It survives game
 * runtime shutdown; pass NULL before destroying the host. Native mode never
 * calls it. An absent/incompatible host rejects enhanced activation. */
void ActRaiserLocalizationRuntime_SetPresentationHost(
    const ArTextPresentationHost *host);
/* Resolve source changes synchronously before the overlay saves preferences.
 * Failed activation restores the previous content/presentation settings. */
void ActRaiserLocalizationRuntime_ApplySettings(void);
void ActRaiserLocalizationRuntime_CaptureFrame(
    ArLocalizationFrame *frame, uint16_t bg3_tilemap_base_words,
    uint16_t bg3_tile_base_words,
    const uint16_t *vram_words, size_t vram_word_count,
    const uint16_t *cgram_words, size_t cgram_word_count);
void ActRaiserLocalizationRuntime_Shutdown(void);

#endif /* ACTRAISER_LOCALIZATION_RUNTIME_H */
