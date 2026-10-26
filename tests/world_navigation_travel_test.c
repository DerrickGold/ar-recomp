#include "support/test_assert.h"
#include "sim/world_nav/world_navigation_travel.h"
#include "actraiser_game.h"
#include "byte_order.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint8_t ram[kSnesWramSize];
static void Focus(unsigned x, unsigned y) {
  ByteOrder_WriteLe16(ram + kActRaiserWram_WorldFocusX, x);
  ByteOrder_WriteLe16(ram + kActRaiserWram_WorldFocusY, y);
}
static WorldNavigationTravelFrame Frame(const Ar2WorldAtlas *atlas) {
  return WorldNavigationTravel_Capture(ram, sizeof(ram), atlas);
}
static void FinishTurn(const Ar2WorldAtlas *atlas) {
  for (int i = 0; i < 45; i++)
    assert(WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 0xfff) == 0);
  assert(!Frame(atlas).turning);
  assert(WorldNavigationTravel_TakeDestination() == -1);
}
int main(void) {
  Ar2WorldAtlas *atlas = calloc(1, sizeof(*atlas));
  assert(atlas);
  atlas->destination_count = 3;
  atlas->destinations[0] = (Ar2WorldDestination){.x_min = 692, .x_max = 708,
      .y_min = 355, .y_max = 371, .flags_and_scene = 0x14};
  atlas->destinations[1] = (Ar2WorldDestination){.x_min = 732, .x_max = 748,
      .y_min = 259, .y_max = 275, .flags_and_scene = 0x16};
  atlas->destinations[2] = atlas->destinations[0];
  atlas->destinations[2].flags_and_scene = 0x0a;
  WorldNavigationTravel_Reset();
  assert(!WorldNavigationTravel_Toggle(ram, sizeof(ram), atlas, true));
  ram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  ram[kActRaiserWram_CurrentMap] = kActRaiserNonActionMap_WorldMap;
  Focus(600, 640);
  assert(WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 0xfff) == 0xfff);
  assert(!WorldNavigationTravel_Toggle(ram, sizeof(ram), NULL, true));
  ram[0xfb] = 1;
  assert(!WorldNavigationTravel_Toggle(ram, sizeof(ram), atlas, true));
  ram[0xfb] = 0;
  assert(WorldNavigationTravel_Toggle(ram, sizeof(ram), atlas, true));
  assert(Frame(atlas).imported && Frame(atlas).turning);
  assert(Frame(atlas).turn == 0 && Frame(atlas).focus_x == 600 && Frame(atlas).focus_y == 640);
  assert(ByteOrder_ReadLe16(ram + kActRaiserWram_Bg1CameraX) == 572);
  FinishTurn(atlas);
  assert(Frame(atlas).turn == 1 && Frame(atlas).focus_x == 700 && Frame(atlas).focus_y == 363);
  assert(Frame(atlas).destination == 0); /* Action wins over its co-located town. */
  assert(WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 8) == 0);
  assert(WorldNavigationTravel_TakeDestination() == -1); /* Must release held enter. */
  WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 0);
  WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 8);
  assert(WorldNavigationTravel_TakeDestination() == 0);
  assert(WorldNavigationTravel_TakeDestination() == -1);
  assert(WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 0xff0) == 0xff0);
  Focus(740, 267); /* Native palace controller's next position. */
  WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 1);
  assert(WorldNavigationTravel_TakeDestination() == 1);
  Focus(748, 267); /* The old 16px boundary now lies within the padded target. */
  assert(Frame(atlas).destination == 1);
  assert(WorldNavigationTravel_Toggle(ram, sizeof(ram), atlas, true));
  const WorldNavigationTravelFrame from = Frame(atlas);
  assert(!from.imported && from.turn == 1 && from.focus_x == 748);
  FinishTurn(atlas);
  assert(Frame(atlas).turn == 0 && Frame(atlas).focus_x == 600 && Frame(atlas).focus_y == 640);
  assert(WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 0xfff) == 0xfff);
  assert(WorldNavigationTravel_Toggle(ram, sizeof(ram), atlas, true));
  for (int i = 0; i < 15; i++) WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 0);
  const WorldNavigationTravelFrame middle = Frame(atlas);
  assert(middle.turn > 0 && middle.turn < 1);
  assert(WorldNavigationTravel_Toggle(ram, sizeof(ram), atlas, true));
  assert(fabsf(Frame(atlas).turn - middle.turn) < .0001f);
  assert(fabsf(Frame(atlas).focus_x - middle.focus_x) < .0001f);
  WorldNavigationTravel_Reset();
  assert(!Frame(atlas).imported && !Frame(atlas).turning);
  assert(WorldNavigationTravel_TakeDestination() == -1);
  assert(WorldNavigationTravel_Toggle(ram, sizeof(ram), atlas, false));
  assert(Frame(atlas).imported && !Frame(atlas).turning && Frame(atlas).turn == 1);
  Focus(668, 331); /* Expanded Industen corner: 64 by 64 map pixels. */
  assert(Frame(atlas).destination == 0);
  WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 0);
  WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 8);
  assert(WorldNavigationTravel_TakeDestination() == 0);
  Focus(667, 331);
  assert(Frame(atlas).destination == -1);
  Focus(732, 363);
  assert(Frame(atlas).destination == -1);
  Focus(700, 395);
  assert(Frame(atlas).destination == -1);
  /* Overlapping targets choose the closest destination, even if a farther
   * action is later in the table. The label and confirm use the same choice. */
  atlas->destinations[1] = (Ar2WorldDestination){.x_min = 732, .x_max = 748,
      .y_min = 355, .y_max = 371, .flags_and_scene = 0x16};
  Focus(715, 363);
  assert(Frame(atlas).destination == 0);
  Focus(725, 363);
  assert(Frame(atlas).destination == 1);
  WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 0);
  WorldNavigationTravel_Input(ram, sizeof(ram), atlas, 8);
  assert(WorldNavigationTravel_TakeDestination() == 1);
  Focus(720, 363); /* Equal distance stays deterministic. */
  assert(Frame(atlas).destination == 0);
  uint8_t before[kSnesWramSize];
  memcpy(before, ram, sizeof(ram));
  const SimWorldNavigationScene base = {.valid = true,
      .source_to_screen = {1, 0, 128 - 720, 0, 1, 112 - 363},
      .composition = {.valid = true}};
  float previous_scale = 0;
  for (int tick = 0; tick <= 90; tick++) {
    WorldNavigationTravel_SetApproach(tick / 90.f);
    const WorldNavigationTravelFrame travel = Frame(atlas);
    SimWorldNavigationScene scene = base;
    uint8_t brightness = 15;
    WorldNavigationTravel_ApplyApproach(&travel, &scene, &brightness);
    const float *m = scene.source_to_screen;
    assert(fabsf(m[0] * 720 + m[1] * 363 + m[2] - 128) < .01f);
    assert(fabsf(m[3] * 720 + m[4] * 363 + m[5] - 112) < .01f);
    const float scale = hypotf(m[0], m[3]);
    assert(isfinite(scale) && scale >= previous_scale);
    assert(scene.composition.empty_animation == (tick > 0));
    if (tick == 90) assert(brightness == 0);
    previous_scale = scale;
  }
  for (int tick = 90; tick >= 0; tick--) {
    WorldNavigationTravel_SetApproach(tick / 90.f);
    assert(fabsf(Frame(atlas).approach - tick / 90.f) < .0001f);
  }
  SimWorldNavigationScene restored = base;
  uint8_t brightness = 15;
  WorldNavigationTravelFrame travel = Frame(atlas);
  WorldNavigationTravel_ApplyApproach(&travel, &restored, &brightness);
  assert(!memcmp(&base, &restored, sizeof(base)) && brightness == 15);
  assert(!memcmp(before, ram, sizeof(ram)));
  WorldNavigationTravel_SetApproach(NAN);
  assert(Frame(atlas).approach == 0);
  free(atlas);
  return 0;
}
