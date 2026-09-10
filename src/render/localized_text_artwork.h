#ifndef AR_RENDER_LOCALIZED_TEXT_ARTWORK_H
#define AR_RENDER_LOCALIZED_TEXT_ARTWORK_H

#include "render/localized_text_presenter.h"

/* Placement of the objects that sit inside a line of text: the game's own
 * selector, life and population icons, the speed arrow, and the name-field
 * underlines. This module owns the textures it decodes from the frame's
 * artwork, so that cache lives with the code that fills it rather than in the
 * presenter's state. */

/* Positions one inline object beside its shaped cluster, uploading the frame's
 * artwork on first use. `snapshot` supplies the surface's key separator, which
 * sizes a selector to the gutter the game reserves.
 *
 * `key_cell_extent` overrides that measurement with the room a key actually
 * has, in surface pixels, and is zero when the text flows. Text on a uniform
 * key pitch no longer keeps the gutter the shaper measured -- the four blanks
 * between keys are wider than the single native cell the game reserved -- so
 * sizing the selector to them would draw it larger than the original. */
bool ArLocalizedTextArtwork_PrepareInlineObject(
    ArRenderDevice *device, const ArLocalizationFrame *frame,
    const ArLocalizationTextSnapshot *snapshot,
    ArLocalizationInlineObjectKind kind, const ArTextSurface *surface,
    const char *utf8, size_t utf8_bytes,
    const ArTextRevealCluster *cluster, ArRenderRectI text_destination,
    int key_cell_extent, ArLocalizedPreparedInlineObject *prepared);

/* Uploads one of the frame's artworks on first use and returns its texture,
 * reusing the cached upload while the pixels are unchanged. */
bool ArLocalizedTextArtwork_PrepareTexture(ArRenderDevice *device,
                                           const ArLocalizationFrame *frame,
                                           ArLocalizationArtworkKind kind,
                                           ArRenderTexture *texture);

/* Ink bounds measured when that artwork was uploaded, for callers aligning an
 * object's visible centre rather than its transparent bitmap. */
ArRenderRectI ArLocalizedTextArtwork_Ink(ArLocalizationArtworkKind kind);

/* Releases every artwork and selector texture this module owns. */
void ArLocalizedTextArtwork_Reset(ArRenderDevice *device);

#endif /* AR_RENDER_LOCALIZED_TEXT_ARTWORK_H */
