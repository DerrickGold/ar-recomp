#ifndef AR_ACTION_BLOODPOOL_SURFACE_H
#define AR_ACTION_BLOODPOOL_SURFACE_H

#include <stdint.h>

/* Decoded 02/01 BG1 water at world Y=480. Shoreline banks separate eight
 * spans; the upper eight pixels mix shore/wood art, while the exposed lower
 * water is priority 1. Never attach this treatment to the repeating BG2 sky. */
static const struct { uint16_t left, right; } kBloodpoolWaterSpans[] = {
  {176,880}, {960,1184}, {1248,1360}, {1440,2144},
  {2240,2368}, {2464,2560}, {2688,2864}, {2912,4096},
};
enum { kBloodpoolWaterSpanCount = sizeof(kBloodpoolWaterSpans) /
                                  sizeof(kBloodpoolWaterSpans[0]) };
#endif
