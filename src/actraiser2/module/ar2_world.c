#include "ar2_world.h"

static uint16_t word(const uint8_t *bytes, size_t index) {
  return bytes[index] | (uint16_t)bytes[index + 1] << 8;
}

bool Ar2World_SupportsScene(unsigned scene) {
  /* $15 is Industen's following boss room. It retains the row-major maps
   * and raster HUD ownership of the entrance; the globe still launches $14. */
  return scene == 0x14 || scene == 0x15 || scene == 0x16;
}

int Ar2World_CaptureShift(int camera, int world_width, int margin) {
  if (margin <= 0 || world_width < 256 || camera < 0 || camera > world_width - 256) return 0;
  const int minimum = world_width >= 256 + 2 * margin ? margin : (world_width - 256) / 2;
  const int maximum = world_width - 256 - minimum;
  return (camera < minimum ? minimum : camera > maximum ? maximum : camera) - camera;
}

bool Ar2World_Update(Ar2World *world, const uint8_t *wram, size_t size) {
  if (!world) return false;
  world->valid = false;
  if (!wram || size != 0x20000) return false;
  const unsigned scene = word(wram, 0x51a);
  if (!Ar2World_SupportsScene(scene)) return false;
  for (unsigned layer = 0; layer < 2; ++layer) {
    const unsigned index = layer * 2;
    const unsigned width = word(wram, 0x55c + index);
    const unsigned height = word(wram, 0x560 + index);
    const unsigned map = word(wram, 0x564 + index);
    const unsigned definitions = word(wram, 0x56c + index);
    if (!width || !height || width > 256 || height > 256 ||
        width * height > AR2_WORLD_TILE_CAPACITY / 4 || map + width * height > 0x10000 ||
        definitions + 256 * 8 > 0x10000)
      return false;
    Ar2WorldLayer *out = &world->layers[layer];
    out->width = width * 2;
    out->height = height * 2;
    out->camera_x = word(wram, 0x538 + index);
    out->camera_y = word(wram, 0x53c + index);
    out->x_mask = word(wram, 0x580 + index);
    out->y_mask = word(wram, 0x584 + index);
    for (unsigned y = 0; y < height; ++y) {
      for (unsigned x = 0; x < width; ++x) {
        unsigned definition = definitions + wram[map + y * width + x] * 8;
        unsigned tile = y * 2 * out->width + x * 2;
        out->tiles[tile] = word(wram, definition);
        out->tiles[tile + 1] = word(wram, definition + 2);
        out->tiles[tile + out->width] = word(wram, definition + 4);
        out->tiles[tile + out->width + 1] = word(wram, definition + 6);
      }
    }
  }
  world->valid = true;
  return true;
}

uint32_t Ar2World_Lookup(void *layer, int32_t x, int32_t y, uint16_t *entry) {
  const Ar2WorldLayer *world = layer;
  if (!world || !entry || !world->width || !world->height) return 0;
  /* AR2's scenery repeats only where its native strip builder masks world
   * coordinates. $FFFF is the finite terrain case, not a wrapping texture. */
  if (world->x_mask != 0xffff) x = (uint32_t)x & (world->x_mask >> 3);
  if (world->y_mask != 0xffff) y = (uint32_t)y & (world->y_mask >> 3);
  if (x < 0 || y < 0 || (uint32_t)x >= world->width || (uint32_t)y >= world->height) return 0;
  *entry = world->tiles[(size_t)y * world->width + x];
  return 1;
}
