#include "diorama_rom_backdrop.h"

#include "action/action_room_scene.h"
#include "byte_order.h"
#include "quintet_lzss.h"
#include "snes_bgr555.h"

enum {
  kTileBytes4Bpp = 32,
};

bool DioramaRomBackdrop_DecompressAsset(const uint8_t *packed,
                                        size_t packed_size,
                                        uint8_t *out,
                                        size_t expected_size) {
  if (!packed || !out || packed_size < 2) return false;
  return QuintetLzss_DecompressAsset(
      packed, packed_size, out, expected_size, NULL);
}

static unsigned TilePixel4Bpp(const uint8_t *characters, unsigned tile,
                              unsigned x, unsigned y) {
  const uint8_t *art = characters + tile * kTileBytes4Bpp;
  const unsigned shift = 7 - x;
  const uint16_t low = ByteOrder_ReadLe16(art + y * 2);
  const uint16_t high = ByteOrder_ReadLe16(art + (y + 8) * 2);
  return ((low >> shift) & 1u) | (((low >> (shift + 8)) & 1u) << 1) |
      (((high >> shift) & 1u) << 2) |
      (((high >> (shift + 8)) & 1u) << 3);
}

bool DioramaRomBackdrop_LoadActionBg(const uint8_t *rom, size_t rom_size,
                                     uint8_t map_group, uint8_t map_number,
                                     uint8_t bg_layer,
                                     uint32_t *out_argb,
                                     size_t out_pixel_count) {
  if (!rom || !out_argb ||
      out_pixel_count <
          (size_t)kDioramaRomBackdropPixels * kDioramaRomBackdropPixels ||
      (bg_layer != 1 && bg_layer != 2))
    return false;

  ActionRoomScene scene;
  if (!ActionRoomScene_Load(
          &scene, rom, rom_size, map_group, map_number) ||
      !scene.have_character_bank[0] || !scene.have_character_bank[1] ||
      !scene.have_palette ||
      !ActionRoomScene_HasBackground(&scene, bg_layer))
    return false;

  uint32_t palette[128];
  for (unsigned i = 0; i < 128; i++) {
    const uint16_t colour = ByteOrder_ReadLe16(scene.palette + i * 2);
    palette[i] = 0xFF000000u |
        (uint32_t)ExpandColor5(colour, 15) << 16 |
        (uint32_t)ExpandColor5(colour >> 5, 15) << 8 |
        ExpandColor5(colour >> 10, 15);
  }

  for (unsigned tile_y = 0; tile_y < 32; tile_y++) {
    for (unsigned tile_x = 0; tile_x < 32; tile_x++) {
      uint16_t entry = 0;
      if (!ActionRoomScene_LookupTile(
              &scene, bg_layer, tile_x, tile_y, &entry, NULL))
        return false;
      const unsigned tile = entry & 0x3FFu;
      const unsigned palette_base = ((entry >> 10) & 7u) * 16u;
      const bool flip_x = (entry & 0x4000u) != 0;
      const bool flip_y = (entry & 0x8000u) != 0;
      const uint8_t *tile_characters = scene.characters;
      unsigned tile_index = tile;
      if (tile >=
          kActionRoomSceneCharacterBytes / kTileBytes4Bpp) {
        if (!scene.have_extra_characters || tile < 0x200 || tile >= 0x300)
          return false;
        tile_characters = scene.extra_characters;
        tile_index = tile - 0x200;
      }
      for (unsigned py = 0; py < 8; py++) {
        for (unsigned px = 0; px < 8; px++) {
          const unsigned sx = flip_x ? 7 - px : px;
          const unsigned sy = flip_y ? 7 - py : py;
          const unsigned colour = TilePixel4Bpp(
              tile_characters, tile_index, sx, sy);
          out_argb[(size_t)(tile_y * 8 + py) * kDioramaRomBackdropPixels +
                   tile_x * 8 + px] =
              palette[colour ? palette_base + colour : 0];
        }
      }
    }
  }
  return true;
}

bool DioramaRomBackdrop_LoadAitosSky(const uint8_t *rom, size_t rom_size,
                                     uint32_t *out_argb,
                                     size_t out_pixel_count) {
  return DioramaRomBackdrop_LoadActionBg(
      rom, rom_size, 0x04, 0x01, 2, out_argb, out_pixel_count);
}
