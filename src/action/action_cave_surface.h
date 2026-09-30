#ifndef AR_ACTION_CAVE_SURFACE_H
#define AR_ACTION_CAVE_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

/* Shared by pool glints, ripples and splash mist. BG2 metatile 01 is the
 * pool surface; paired 02/06 columns are falls. Coordinates were checked
 * against the decoded 01/02 map, including the native splash at each foot. */
typedef struct ActionCaveWaterRegion {
  int16_t left, right, surface_y;
} ActionCaveWaterRegion;

typedef struct ActionCaveWaterfall {
  int16_t x, y;
} ActionCaveWaterfall;

static const ActionCaveWaterRegion kActionCavePools[] = {
  {0,880,896}, {896,1104,688}, {1088,1312,304},
};
static const ActionCaveWaterfall kActionCaveFalls[] = {
  {736,896}, {1008,688}, {1120,304}, {1264,304},
};
enum {
  kActionCavePoolCount = sizeof(kActionCavePools) / sizeof(kActionCavePools[0]),
  kActionCaveFallCount = sizeof(kActionCaveFalls) / sizeof(kActionCaveFalls[0]),
};

/* Authored against 01/02's foreground rock silhouettes. The blue decorative
 * boulders share BG1-low with several real rock edges: tile priority alone
 * cannot classify this material. Keep exact metatile identities at both ends
 * and fail closed if the loaded room no longer matches the authored surface.
 * Contours are the first opaque pixel in each of 33 columns, x-16..x+16;
 * 127 denotes an open column. No render-time map or texture reads are needed. */
typedef struct ActionCaveWetSource {
  int16_t x, ceiling_y, landing_y;
  uint8_t ceiling_tile, landing_tile;
  bool water;
  int8_t contour[33];
} ActionCaveWetSource;

static const ActionCaveWetSource kActionCaveWetSources[] = {
  {220,348,448,0x93,0xAF,false,{
    8,8,8,8,6,5,4,5,4,4,3,3,3,3,1,0,0,
    0,0,0,0,-2,-3,-4,-4,-5,-7,-8,-9,-12,-13,-14,-15}},
  {326,371,451,0xC0,0x8E,false,{
    -16,-12,-11,-11,-10,-9,-9,-8,-7,-4,-3,-2,-2,-2,-1,-1,0,
    4,5,5,6,7,7,8,9,12,13,14,14,14,15,15,16}},
  {358,371,483,0xC0,0x8E,false,{
    -16,-12,-11,-11,-10,-9,-9,-8,-7,-4,-3,-2,-2,-2,-1,-1,0,
    4,5,5,6,7,7,8,9,12,127,127,127,127,127,127,127}},
  {420,452,620,0xB8,0x8D,false,{
    127,127,127,127,127,127,127,127,127,127,127,127,4,2,1,0,0,
    -1,-3,-4,-5,-8,-9,-10,-11,-12,-12,-12,-12,-12,-12,-11,-11}},
  {534,766,896,0x9D,0x62,true,{0}},
  {662,787,896,0xC0,0x6C,true,{0}},
  {872,844,896,0xC7,0x2F,true,{0}},
  {888,556,674,0xC7,0x91,false,{
    5,2,1,0,-1,-2,-2,-2,-2,-2,-2,-1,-1,-1,0,0,0,
    -1,-1,-1,-1,-1,-1,-2,-2,-1,-1,-1,0,0,1,5,6}},
};

enum { kActionCaveWetSourceCount = sizeof(kActionCaveWetSources) /
                                   sizeof(kActionCaveWetSources[0]) };
#endif
