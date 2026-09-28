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
typedef struct ActionCastleSource {
  uint8_t room, top_tile, below_tile, diffuse;
  int16_t check_x, check_y, x, y, length;
  int16_t width, spread, lean, left, right, bottom, sill;
  uint8_t sill_tile;
} ActionCastleSource;

static const ActionCastleSource kActionCastleSources[] = {
  {3,0x44,0x4C,0, 160,512,168,514,32,4,16,0,144,192,584,552,0x54},
  {3,0x44,0x4C,0, 176,512,184,514,32,4,16,0,160,208,584,552,0x54},
  {3,0x51,0x59,0, 624,176,640,178,46,16,40,0,592,688,282,236,0x6A},
  {3,0x51,0x59,0, 624,288,640,290,46,16,40,0,592,688,394,348,0x6A},
  {3,0x51,0x59,0, 624,400,640,402,46,16,40,0,592,688,506,460,0x6A},
  {3,0x51,0x59,0, 624,512,640,514,46,16,40,0,592,688,618,572,0x6A},
  {3,0x51,0x59,0, 624,624,640,626,46,16,40,0,592,688,730,684,0x6A},
  {3,0x51,0x59,0, 624,736,640,738,46,16,40,0,592,688,842,796,0x6A},
  {4,0x55,0x56,1, 16,0,256,112,338,30,116,-20,160,352,464,0,0x00},
  {5,0x44,0x4C,0, 144,304,152,306,32,4,25,0,120,200,376,344,0x54},
  {5,0x44,0x4C,0, 144,368,152,370,32,4,25,0,120,200,440,408,0x54},
  {5,0x44,0x4C,0, 144,432,152,434,32,4,25,0,120,200,504,472,0x54},
  {5,0x44,0x4C,0, 768,336,776,338,108,4,50,0,688,840,496,376,0x54},
  {5,0x44,0x4C,0, 768,416,776,418,82,4,48,0,704,840,544,456,0x54},
  {7,0x70,0x78,0, 400,112,424,114,36,16,24,0,336,816,240,204,0x89},
  {7,0x70,0x78,0, 560,112,584,114,36,16,24,0,336,816,240,204,0x89},
  {7,0x70,0x78,0, 720,112,744,114,36,16,24,0,336,816,240,204,0x89},
  {8,0x51,0x61,0, 112,32,128,38,52,32,88,0,32,224,224,172,0x89},
  /* Expanded warm bounce retains the native flame as its bright core. */
  {3,0x47,0x4F,2, 64,448,72,463,80,16,100,0,0,184,555,0,0x00},
  {3,0x47,0x4F,2, 208,512,216,527,74,16,100,0,104,328,619,0,0x00},
  {3,0x47,0x4F,2, 432,512,440,527,74,16,100,0,328,552,619,0,0x00},
  {5,0x47,0x4F,2, 832,448,840,463,86,16,110,0,720,960,555,0,0x00},
  {5,0x47,0x4F,2, 656,560,664,575,86,16,110,0,544,784,667,0,0x00},
  {5,0x47,0x4F,2, 800,640,808,655,86,16,110,0,688,928,747,0,0x00},
  /* Every arch in the gallery, including the alternating metatile layouts. */
  {7,0x51,0x59,0, 368,112,384,114,36,16,24,0,336,816,240,204,0x8C},
  {7,0x73,0x7B,0, 448,112,464,114,36,16,24,0,336,816,240,204,0x8C},
  {7,0x70,0x78,0, 480,112,504,114,36,16,24,0,336,816,240,204,0x89},
  {7,0x73,0x7B,0, 528,112,544,114,36,16,24,0,336,816,240,204,0x8C},
  {7,0x73,0x7B,0, 608,112,624,114,36,16,24,0,336,816,240,204,0x8C},
  {7,0x70,0x78,0, 640,112,664,114,36,16,24,0,336,816,240,204,0x89},
  {7,0x73,0x7B,0, 688,112,704,114,36,16,24,0,336,816,240,204,0x8C},
  {7,0x73,0x7B,0, 768,112,784,114,36,16,24,0,336,816,240,204,0x6A},
};
enum { kActionCastleSourceCount = sizeof(kActionCastleSources) / sizeof(kActionCastleSources[0]) };
/* The native blue moat comes from BG2-low, color-blended into BG1 scenery.
 * Eight validated strips cover its surface; accents follow resolved BG1. */
enum {
  kActionCastleWaterLeft = 592, kActionCastleWaterRight = 1744,
  kActionCastleWaterSurface = 944, kActionCastleWaterStripWidth = 144,
  kActionCastleWaterStripCount = 8,
};
#endif
