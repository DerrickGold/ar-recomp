#ifndef AR_DIORAMA_ROM_BACKDROP_H
#define AR_DIORAMA_ROM_BACKDROP_H
/* DioramaRomBackdrop: decodes the ROM's compressed assets and rasterizes an
 * action room's first background page into an opaque backdrop plane for the
 * diorama view. New room consumers should use action/action_room_scene.h.
 * Phase: pure.
 * Tests: tests/diorama_rom_backdrop_test.c */

#include "action/action_room_scene.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  kDioramaRomBackdropPixels = 256,
};

/* Pure decoder for the stock compressed-asset format: a little-endian output
 * size followed by MSB-first literal/back-reference tokens. Public so the HLE
 * source path can be regression-tested without requiring a copyrighted ROM. */
bool DioramaRomBackdrop_DecompressAsset(const uint8_t *packed,
                                        size_t packed_size,
                                        uint8_t *out,
                                        size_t expected_size);

/* Identical rasterization from already decoded assets (e.g. offline editor).
 * Does not borrow scene storage after return. */
bool DioramaRomBackdrop_RenderScenePage(const ActionRoomScene *scene,
    uint8_t bg_layer, uint32_t transparent_fill_argb, bool sparse,
    uint32_t *out_default_fill_argb, uint32_t *out_argb, size_t out_pixel_count);

/* Compatibility rasterizer over the shared ActionRoomScene loader. The first
 * 256x256 map page is rendered as opaque ARGB8888 for a residual backdrop
 * plane, including profile-driven common tile priority during lookup. New
 * arbitrary-room consumers should use action/action_room_scene.h directly. */
bool DioramaRomBackdrop_LoadActionBg(const uint8_t *rom, size_t rom_size,
                                     uint8_t map_group, uint8_t map_number,
                                     uint8_t bg_layer,
                                     uint32_t *out_argb,
                                     size_t out_pixel_count);

/* Same immutable decode, but first fills the complete plane opaque black and
 * then paints its non-zero tile pixels. This models the authentic final black
 * backing even where the source is not fully tiled, without changing opaque
 * black or mixed tile detail. */
bool DioramaRomBackdrop_LoadActionBgTransparentBlack(
    const uint8_t *rom, size_t rom_size,
    uint8_t map_group, uint8_t map_number, uint8_t bg_layer,
    uint32_t *out_argb, size_t out_pixel_count);

/* General form used by the layer editor: pre-fill the complete output plane
 * with one opaque ARGB colour, then paint every non-zero source pixel. */
bool DioramaRomBackdrop_LoadActionBgTransparentFill(
    const uint8_t *rom, size_t rom_size,
    uint8_t map_group, uint8_t map_number, uint8_t bg_layer,
    uint32_t fill_argb, uint32_t *out_argb, size_t out_pixel_count);

/* Decode only authored non-zero tile pixels. Colour-zero pixels and untiled
 * regions remain transparent; `out_default_fill_argb` receives the source
 * palette's colour zero so a renderer can cheaply compose either the stock
 * backdrop or a live fill behind the immutable artwork. */
bool DioramaRomBackdrop_LoadActionBgSparse(
    const uint8_t *rom, size_t rom_size,
    uint8_t map_group, uint8_t map_number, uint8_t bg_layer,
    uint32_t *out_argb, size_t out_pixel_count,
    uint32_t *out_default_fill_argb);

#endif  /* AR_DIORAMA_ROM_BACKDROP_H */
