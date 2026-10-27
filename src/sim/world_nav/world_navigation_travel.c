#include "sim/world_nav/world_navigation_travel.h"
#include "actraiser_game.h"
#include "byte_order.h"
#include <math.h>
#include <string.h>

enum { kTurnTicks = 45, kDestinationPadding = 24 };
static struct {
  bool imported, visited, transition;
  uint16_t x[2], y[2];
  float from_x, from_y, from_turn;
  unsigned ticks;
  uint32_t previous_input;
  int pending;
} s_travel = {.ticks = kTurnTicks, .pending = -1};

static bool Navigating(const uint8_t *ram, size_t size) {
  return ram && size >= kSnesWramSize &&
         ram[kActRaiserWram_MapGroup] == kActRaiserMapGroup_NonAction &&
         ram[kActRaiserWram_CurrentMap] == kActRaiserNonActionMap_WorldMap;
}

static int Destination(const Ar2WorldAtlas *atlas, unsigned x, unsigned y) {
  if (!atlas) return -1;
  int found = -1;
  unsigned nearest = UINT32_MAX;
  for (unsigned i = 0; i < atlas->destination_count; i++) {
    const Ar2WorldDestination *d = &atlas->destinations[i];
    /* The native 16px target is much smaller than the hovering palace.
     * Pad in map coordinates so flat, globe and zoomed views agree. */
    if ((int)x < (int)d->x_min - kDestinationPadding ||
        (int)x >= d->x_max + kDestinationPadding ||
        (int)y < (int)d->y_min - kDestinationPadding ||
        (int)y >= d->y_max + kDestinationPadding) continue;
    const int dx = 2 * (int)x - (d->x_min + d->x_max);
    const int dy = 2 * (int)y - (d->y_min + d->y_max);
    const unsigned distance = dx * dx + dy * dy;
    /* Co-located town/action records retain their identities. Until town
     * execution is supported, prefer the action at equal distance. Overlapping
     * targets otherwise select the nearest center, independent of table order. */
    if (found < 0 || distance < nearest ||
        (distance == nearest && (d->flags_and_scene & 255) != 0x0a &&
         (atlas->destinations[found].flags_and_scene & 255) == 0x0a)) {
      found = (int)i;
      nearest = distance;
    }
  }
  return found;
}

void WorldNavigationTravel_Reset(void) {
  memset(&s_travel, 0, sizeof(s_travel));
  s_travel.ticks = kTurnTicks;
  s_travel.pending = -1;
}

WorldNavigationTravelFrame WorldNavigationTravel_Capture(const uint8_t *ram, size_t size,
                                                         const Ar2WorldAtlas *atlas) {
  WorldNavigationTravelFrame frame = {.destination = -1};
  if (!Navigating(ram, size) || !atlas) return frame;
  const unsigned x = ByteOrder_ReadLe16(ram + kActRaiserWram_WorldFocusX);
  const unsigned y = ByteOrder_ReadLe16(ram + kActRaiserWram_WorldFocusY);
  float t = s_travel.ticks / (float)kTurnTicks;
  t = t * t * (3 - 2 * t);
  frame.imported = s_travel.imported;
  frame.transition = s_travel.transition;
  frame.turning = s_travel.ticks < kTurnTicks;
  frame.turn = s_travel.from_turn + ((s_travel.imported ? 1 : 0) - s_travel.from_turn) * t;
  frame.focus_x = s_travel.from_x + (x - s_travel.from_x) * t;
  frame.focus_y = s_travel.from_y + (y - s_travel.from_y) * t;
  if (frame.imported && !frame.turning) frame.destination = Destination(atlas, x, y);
  return frame;
}

bool WorldNavigationTravel_Toggle(uint8_t *ram, size_t size, const Ar2WorldAtlas *atlas,
                                  bool animate) {
  if (!Navigating(ram, size) || !atlas || !atlas->destination_count ||
      ByteOrder_ReadLe16(ram + kActRaiserWram_WorldEmergenceState) || ram[0xfb] ||
      ByteOrder_ReadLe16(ram + 0x347) == 7)
    return false;
  const WorldNavigationTravelFrame before = WorldNavigationTravel_Capture(ram, size, atlas);
  s_travel.x[s_travel.imported] = ByteOrder_ReadLe16(ram + kActRaiserWram_WorldFocusX);
  s_travel.y[s_travel.imported] = ByteOrder_ReadLe16(ram + kActRaiserWram_WorldFocusY);
  if (!s_travel.visited) {
    unsigned entry = 0;
    for (unsigned i = 0; i < atlas->destination_count; i++)
      if ((atlas->destinations[i].flags_and_scene & 255) == 0x14) entry = i;
    s_travel.x[1] = atlas->destinations[entry].x_min + 8;
    s_travel.y[1] = atlas->destinations[entry].y_min + 8;
    s_travel.visited = true;
  }
  s_travel.imported = !s_travel.imported;
  const unsigned x = s_travel.x[s_travel.imported], y = s_travel.y[s_travel.imported];
  ByteOrder_WriteLe16(ram + kActRaiserWram_WorldFocusX, x);
  ByteOrder_WriteLe16(ram + kActRaiserWram_WorldFocusY, y);
  ByteOrder_WriteLe16(ram + kActRaiserWram_Bg1CameraX, x - 128);
  ByteOrder_WriteLe16(ram + kActRaiserWram_Bg1CameraY, y - 112);
  s_travel.from_x = before.focus_x;
  s_travel.from_y = before.focus_y;
  s_travel.from_turn = before.turn;
  s_travel.ticks = animate ? 0 : kTurnTicks;
  s_travel.previous_input = UINT32_MAX;
  s_travel.pending = -1;
  return true;
}

uint32_t WorldNavigationTravel_Input(uint8_t *ram, size_t size,
                                    const Ar2WorldAtlas *atlas, uint32_t input) {
  if (!Navigating(ram, size) || !atlas) {
    s_travel.imported = false;
    s_travel.ticks = kTurnTicks;
    s_travel.pending = -1;
    s_travel.previous_input = input;
    return input;
  }
  const uint32_t pressed = input & ~s_travel.previous_input;
  s_travel.previous_input = input;
  if (s_travel.ticks < kTurnTicks) {
    s_travel.ticks++;
    return 0;
  }
  if (!s_travel.imported) return input;
  /* Native B/Y/Start enter the selected place; A/X keep their native zoom
   * behavior. Direction and shoulder inputs still run the original code. */
  if (pressed & 0x0b) {
    const unsigned x = ByteOrder_ReadLe16(ram + kActRaiserWram_WorldFocusX);
    const unsigned y = ByteOrder_ReadLe16(ram + kActRaiserWram_WorldFocusY);
    s_travel.pending = Destination(atlas, x, y);
  }
  return input & 0xff0;
}

int WorldNavigationTravel_TakeDestination(void) {
  int destination = s_travel.pending;
  s_travel.pending = -1;
  return destination;
}

void WorldNavigationTravel_SetTransition(bool active) {
  s_travel.transition = active;
}
