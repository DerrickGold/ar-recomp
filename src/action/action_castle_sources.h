#ifndef AR_ACTION_CASTLE_SOURCES_H
#define AR_ACTION_CASTLE_SOURCES_H
#include <stdint.h>

/* Bloodpool 02/02-08, authored against decoded native maps. Every source has
 * two metatile witnesses. Beam corridors stop at nearby masonry/floors rather
 * than moving with the viewport. For windows, y is the arch reference row;
 * the renderer follows its highlighted contour around that row. sill is the
 * first row BELOW the rendered sill; length is the ray reach below it.
 * The extra sill metatile witness rejects a changed/missing lower frame.
 * Room 4 uses diffuse offscreen fill. Non-window sources have no sill. */
typedef enum ActionCastleSourceKind {
  kActionCastleSource_Window = 0,
  kActionCastleSource_Ambient,
  kActionCastleSource_Torch,
} ActionCastleSourceKind;

typedef struct ActionCastleSource {
  uint8_t style, top_tile, below_tile, kind;
  int16_t check_x, check_y, x, y, length;
  int16_t width, spread, lean, left, right, bottom, sill;
  uint8_t sill_tile, arch;
  uint16_t identity;
} ActionCastleSource;

enum { kActionCastleWaterStripCount=8 };
#endif
