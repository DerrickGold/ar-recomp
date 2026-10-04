#ifndef AR_HUD_ICON_FRAME_H
#define AR_HUD_ICON_FRAME_H
#include <stdbool.h>
#include <stdint.h>
#include "snesrecomp/runner/ppu.h"

/* Completed native OBJ transfer. Coordinates use native screen pixels; the
 * surface owns its stride/origin. FrameQueue copies pixels with this record. */
typedef struct HudIconFrame {
  uint64_t lifetime_generation;
  uint64_t frame_serial;
  SrPpuSurfaceView surface;
  int16_t x, y, width, height;
  uint8_t first, count;
  bool scene_removed;
} HudIconFrame;
#endif
