#ifndef ACTRAISER_LOCALIZATION_RUNTIME_H
#define ACTRAISER_LOCALIZATION_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "localization/localization_frame.h"
#include "localization/language_pack.h"
#include "localization/text_presentation.h"

#define ACTRAISER_LOCALIZATION_PACK_HOST_ABI_VERSION UINT32_C(1)

/* Host-owned pack storage policy. The runtime copies this binding and the
 * native manifest locator; the I/O context remains borrowed through Shutdown.
 * Desktop files are one adapter, not a requirement of the game integration. */
typedef struct ActRaiserLocalizationPackHost {
  size_t struct_size;
  uint32_t abi_version;
  ArLanguagePackIo io;
  const char *native_manifest;
} ActRaiserLocalizationPackHost;

/* Game-thread adapter. Persisted localization settings select native/enhanced
 * presentation and font style, and name the selected pack by package ID from
 * the installed-pack catalog (see localization/pack_discovery.h).
 * AR_LOCALIZATION_PACK remains as a development override that points directly
 * at one manifest, bypassing discovery; the native enhanced source is loaded
 * separately. */
/* The copied host/context must remain valid through runtime shutdown (which
 * retires its font registrations). Only replace/detach it after Shutdown;
 * the binding itself survives that reset. Untouched native mode never calls
 * it. An absent/incompatible host rejects enhanced activation. */
void ActRaiserLocalizationRuntime_SetPresentationHost(
    const ArTextPresentationHost *host);
/* Bind only while the runtime is shut down. NULL detaches the host. An invalid
 * binding leaves enhanced localization unavailable rather than falling back
 * to process environment variables or the current working directory. */
void ActRaiserLocalizationRuntime_SetPackHost(
    const ActRaiserLocalizationPackHost *host);
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
