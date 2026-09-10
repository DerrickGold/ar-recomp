#ifndef ACTRAISER_LOCALIZATION_RUNTIME_H
#define ACTRAISER_LOCALIZATION_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "localization/localization_frame.h"
#include "localization/text_presentation.h"

/* Game-thread adapter. Persisted localization settings select native/enhanced
 * presentation and font style, and name the selected pack by package ID from
 * the installed-pack catalog (see localization/pack_discovery.h).
 * AR_LOCALIZATION_PACK remains as a development override that points directly
 * at one manifest, bypassing discovery; the native enhanced source is loaded
 * separately. */
/* The copied host/context must remain valid until replaced. It survives game
 * runtime shutdown; pass NULL before destroying the host. Native mode never
 * calls it. An absent/incompatible host rejects enhanced activation. */
void ActRaiserLocalizationRuntime_SetPresentationHost(
    const ArTextPresentationHost *host);
/* Resolve source changes synchronously before the overlay saves preferences.
 * Failed activation restores the previous content/presentation settings. */
void ActRaiserLocalizationRuntime_ApplySettings(void);
/* `mode7_transformed` says the Mode 7 layer is being presented rotated or
 * scaled rather than flat. The title screen's lettering lives inside that
 * layer, so a flat screen-space replacement only stands in for it while the
 * layer is untransformed; once the game spins the title away, the letters go
 * with it and the replacement must go too. */
void ActRaiserLocalizationRuntime_CaptureFrame(
    ArLocalizationFrame *frame, uint16_t bg3_tilemap_base_words,
    uint16_t bg3_tile_base_words,
    const uint16_t *vram_words, size_t vram_word_count,
    const uint16_t *cgram_words, size_t cgram_word_count,
    bool mode7_transformed);
void ActRaiserLocalizationRuntime_Shutdown(void);

#endif /* ACTRAISER_LOCALIZATION_RUNTIME_H */
