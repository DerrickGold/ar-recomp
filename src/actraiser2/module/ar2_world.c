#include "ar2_world.h"
#include "actraiser2/ar2_scenes.h"

static uint16_t word(const uint8_t *bytes, size_t index) {
  return bytes[index] | (uint16_t)bytes[index + 1] << 8;
}

bool Ar2World_SupportsEntrance(unsigned scene) {
  return Ar2Scenes_IsEntrance(scene);
}

bool Ar2World_SupportsScene(unsigned scene) {
  return Ar2Scenes_Find(scene) != NULL;
}

int Ar2World_CaptureShift(int camera, int world_width, int margin) {
  if (margin <= 0 || world_width < 256 || camera < 0 || camera > world_width - 256) return 0;
  const int minimum = world_width >= 256 + 2 * margin ? margin : (world_width - 256) / 2;
  const int maximum = world_width - 256 - minimum;
  return (camera < minimum ? minimum : camera > maximum ? maximum : camera) - camera;
}

int Ar2World_BackgroundCaptureShift(const Ar2World *world, int capture_shift, int view_shift) {
  if (!world || !world->valid) return capture_shift;
  const unsigned numerator = world->background_x_ratio & 255;
  const unsigned denominator = world->background_x_ratio >> 8;
  int parallax_shift = 0;
  if (world->background_x_ratio) {
    /* $80:AD66 adds native foreground motion to independently scrolling BG2.
     * Keep its authored motion; only compensate the foreground's view stop. */
    if (world->background_x_autoscroll) parallax_shift = view_shift;
    else {
      if (!denominator) return capture_shift;
      /* $80:ADC9 scales the foreground by the ratio at $05A2 (low/high byte).
       * Subtract the two rounded positions, not a rounded delta: otherwise
       * an odd native camera jitters while the widened view is held still. */
      const int camera = world->layers[0].camera_x;
      parallax_shift = (camera + view_shift) * (int)numerator / (int)denominator -
          camera * (int)numerator / (int)denominator;
    }
  }
  /* Diorama captures a larger apron than it displays. Its common viewport
   * translation must not become an extra parallax movement on BG2. */
  return parallax_shift + capture_shift - view_shift;
}

bool Ar2World_Update(Ar2World *world, const uint8_t *wram, size_t size) {
  if (!world) return false;
  world->valid = false;
  if (!wram || size != 0x20000) return false;
  const unsigned scene = word(wram, 0x51a);
  if (!Ar2World_SupportsScene(scene)) return false;
  world->background_x_ratio = word(wram, 0x5a2);
  world->background_x_autoscroll = (word(wram, 0x5ca) & 0x20) != 0;
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
