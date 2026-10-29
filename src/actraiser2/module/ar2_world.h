#ifndef AR_AR2_WORLD_H
#define AR_AR2_WORLD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* AR2 module adapter for catalogued action rooms. Words are PPU tile attributes;
 * no AR1 page layout, palette patching, or gameplay state is imposed. */
enum { AR2_WORLD_TILE_CAPACITY = 65536 };
typedef struct Ar2WorldLayer {
  uint16_t tiles[AR2_WORLD_TILE_CAPACITY];
  unsigned width, height;
  uint16_t x_mask, y_mask;
  int32_t camera_x, camera_y;
} Ar2WorldLayer;
typedef struct Ar2World {
  Ar2WorldLayer layers[2];
  uint16_t background_x_ratio;
  bool background_x_autoscroll;
  bool valid;
} Ar2World;

bool Ar2World_Update(Ar2World *world, const uint8_t *wram, size_t size);
bool Ar2World_SupportsEntrance(unsigned scene);
bool Ar2World_SupportsScene(unsigned scene);
int Ar2World_CaptureShift(int camera, int world_width, int margin);
/* Reframe BG2 at its native parallax rate, with the same capture apron as BG1. */
int Ar2World_BackgroundCaptureShift(const Ar2World *world, int capture_shift, int view_shift);
uint32_t Ar2World_Lookup(void *layer, int32_t x, int32_t y, uint16_t *entry);
#endif
