#ifndef AR_WORLD_NAVIGATION_TRAVEL_H
#define AR_WORLD_NAVIGATION_TRAVEL_H
/* Session-local continent selection around the native Palace controller.
 * The game owner updates this state; presentation consumes a value snapshot. */
#include "actraiser2/ar2_world_atlas.h"
#include "sim/world_nav/sim_world_navigation_scene.h"

typedef struct WorldNavigationTravelFrame {
  bool imported, turning, flat;
  float turn, focus_x, focus_y, approach;
  int destination;
} WorldNavigationTravelFrame;

void WorldNavigationTravel_Reset(void);
bool WorldNavigationTravel_Toggle(uint8_t *wram, size_t size, const Ar2WorldAtlas *atlas,
                                  bool animate);
uint32_t WorldNavigationTravel_Input(uint8_t *wram, size_t size,
                                    const Ar2WorldAtlas *atlas, uint32_t input);
WorldNavigationTravelFrame WorldNavigationTravel_Capture(const uint8_t *wram, size_t size,
                                                         const Ar2WorldAtlas *atlas);
int WorldNavigationTravel_TakeDestination(void);
/* Presentation-only Advent/return. Zero is ordinary palace navigation;
 * one is the black landing endpoint. Native campaign state stays suspended. */
void WorldNavigationTravel_SetApproach(float approach);
void WorldNavigationTravel_ApplyApproach(const WorldNavigationTravelFrame *travel,
                                        SimWorldNavigationScene *scene, uint8_t *brightness);
#endif
