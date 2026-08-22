#ifndef DIORAMA_ROM_BACKDROP_H
#define DIORAMA_ROM_BACKDROP_H

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

/* Compatibility rasterizer over the shared ActionRoomScene loader. The first
 * 256x256 map page is rendered as opaque ARGB8888 for a residual backdrop
 * plane, including profile-driven common tile priority during lookup. New
 * arbitrary-room consumers should use action/action_room_scene.h directly. */
bool DioramaRomBackdrop_LoadActionBg(const uint8_t *rom, size_t rom_size,
                                     uint8_t map_group, uint8_t map_number,
                                     uint8_t bg_layer,
                                     uint32_t *out_argb,
                                     size_t out_pixel_count);

/* Compatibility wrapper for the named source that originally established
 * this seam. Equivalent to LoadActionBg($04,$01,BG2). */
bool DioramaRomBackdrop_LoadAitosSky(const uint8_t *rom, size_t rom_size,
                                    uint32_t *out_argb,
                                    size_t out_pixel_count);

#endif  /* DIORAMA_ROM_BACKDROP_H */
