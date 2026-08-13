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

/* Read-only HLE reconstruction of an action room's BG1 or BG2. The loader
 * interprets the stock per-map asset script, including inherited assets within
 * an act, and never mutates emulated WRAM/VRAM/CGRAM. The first 256x256 map
 * page is rendered as opaque ARGB8888 for the residual backdrop plane. BG1's
 * native action tile-word mask and per-layer attribute merge are applied
 * before character lookup; this is what maps BG1 definitions to tiles
 * $000-$0FF and BG2 definitions to tiles $100-$1FF. */
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
