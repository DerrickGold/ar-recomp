/* Landing dust: bounded host history, never native actors or WRAM writes.
 * The caller proves the loaded Fillmore cave/temple map before observation. */
#include "action_landing_dust.h"
#include "actraiser_game.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(kActionLandingDustMaxPuffs + 7 <= kActionSceneDecorationMaxInstances,
               "landing events must leave room for all cave ambient fields");

static uint16_t Word(const uint8_t *ram, unsigned at) {
  return (uint16_t)(ram[at] | ((uint16_t)ram[at + 1] << 8));
}

static bool PhysicalActor(const uint8_t *ram, unsigned at, unsigned room) {
  const unsigned status = Word(ram, at), flags = Word(ram, at + 0x30);
  if ((status & 0xC000) || (flags & 0x0400)) return false;
  if (at == kActRaiserWram_PlayerObject)
    return Word(ram, at + 0x16) == 0x8000 && ram[at + 0x18] == 6 &&
        Word(ram, at + 0x12) >= 0x9832 && Word(ram, at + 0x12) < 0x9C47;
  if (!Word(ram, at + 0x2C) || Word(ram, at + 0x3A) || ram[at + 0x18] != 0x7E)
    return false;
  const unsigned source = Word(ram, at + 0x32);
  /* Retained US runtime identities also own region-selected animation data.
   * Flying gargoyles, statue emitters, pickups and projectile children are
   * deliberately absent. These ground families keep the same source in flight. */
  if (room == 4)
    return source == 0xAF5D && Word(ram, at + 0x16) == 0x5000;
  return Word(ram, at + 0x16) == 0x4000 &&
      (source == 0xB041 || source == 0xB0B4 || source == 0xB28D || source == 0xB2FD);
}

static bool StoneContact(const uint8_t *ram, const ActionBgMapView *map,
    unsigned room, int x, int feet, int *surface) {
  if (room == 2 && x < 1160) return false; /* Dust belongs to the temple half. */
  if (feet < 0) return false;
  const int floor_y = (feet + 2) & ~15;
  if (abs(feet - floor_y) > 2) return false;
  const int offsets[] = {0,-8,7}; /* Native flat-ground foot probes. */
  bool stone = false;
  for (unsigned i = 0; i < 3; i++) {
    uint8_t below, above;
    if (!ActionBgMapView_LookupMetatile(map, x + offsets[i], floor_y, &below) ||
        !ActionBgMapView_LookupMetatile(map, x + offsets[i], floor_y - 1, &above))
      continue;
    /* $00:91C3 resolves BG1 metatiles through $05A0. Attribute 15 is solid
     * stone; the cave's 54..57 column capitals use attribute 3 (one-way tops).
     * Spikes are separate, non-solid artwork (18/20), including spikes placed
     * ON those capitals. Check artwork as well as the exposed collision top. */
    if (room != 4 && (above == 0x18 || above == 0x20)) return false;
    const unsigned collision = ram[0x05A0 + below];
    const bool capital = room != 4 && below >= 0x54 && below <= 0x57 && collision == 3;
    if ((collision == 15 || capital) && ram[0x05A0 + above] == 0) stone = true;
  }
  if (stone) *surface = floor_y;
  return stone;
}

static void Spawn(ActionLandingDustState *state, unsigned at, int x, int y,
    unsigned impact, bool boss, uint16_t clock) {
  unsigned patch = kActionLandingDustPatchCount;
  for (unsigned i = 0; i < kActionLandingDustPatchCount; i++) {
    if (!state->patches[i].remaining) { patch = i; continue; }
    if (abs(x-state->patches[i].x) <= 20 && abs(y-state->patches[i].y) <= 4) return;
  }
  if (patch == kActionLandingDustPatchCount) return; /* Bound history as well as visible clouds. */
  for (unsigned i = 0; i < kActionLandingDustMaxPuffs; i++) {
    ActionLandingDustPuff *puff = &state->puffs[i];
    if (puff->active) continue; /* Full: drop the new cosmetic event. */
    if (!++state->next_generation) ++state->next_generation;
    state->patches[patch].x = (int16_t)x;
    state->patches[patch].y = (int16_t)y;
    state->patches[patch].remaining = (uint16_t)(120 +
        (state->next_generation * 37u + (unsigned)x * 11u) % 61u);
    *puff = (ActionLandingDustPuff){
      .generation = state->next_generation, .born = clock, .record_address = (uint16_t)at,
      .x = (int16_t)x, .y = (int16_t)y, .active = 1,
      .strength = (uint8_t)(boss ? 3 : impact >= 5 ? 2 : 1),
    };
    return;
  }
}

static void Observe(ActionLandingDustState *state, const uint8_t *ram,
    const ActionBgMapView *map, unsigned room, unsigned ticks, uint16_t clock) {
  for (unsigned i = 0; i < kActionSceneEffectObserverTrackCount; i++) {
    const unsigned at = kActRaiserWram_ActionObjectTable + i * kActRaiserActionObjectStride;
    ActionLandingDustTrack *track = &state->tracks[i];
    const int bottom = (int16_t)Word(ram, at + 0x10);
    if (!PhysicalActor(ram, at, room) || bottom < 0 || bottom > 64) {
      memset(track, 0, sizeof(*track));
      continue;
    }
    const int x = (int16_t)Word(ram, at + 2);
    const int y = (int16_t)Word(ram, at + 4);
    const int feet = y + bottom;
    const unsigned source = Word(ram, at + 0x32), animation = Word(ram, at + 0x16);
    const int dy = y - track->y;
    const bool continuous = track->valid && track->source == source &&
        track->animation == animation && abs(x - track->x) <= (int)ticks * 16 &&
        abs(dy) <= (int)ticks * 24 &&
        abs((feet - track->feet_y) - dy) <= 2; /* Pose extent changes are not falls. */
    if (!continuous) track->descent = track->impact = 0;
    int surface = 0;
    const bool grounded = StoneContact(ram, map, room, x, feet, &surface);
    if (continuous && dy > 0) {
      const unsigned descent = track->descent + (unsigned)dy;
      track->descent = (uint8_t)(descent > 255 ? 255 : descent);
      const unsigned speed = (unsigned)dy / ticks;
      if (speed > track->impact) track->impact = (uint8_t)speed;
    }
    if (grounded) {
      const int cam_x = (int16_t)Word(ram, kActRaiserWram_Bg1CameraX);
      const int cam_y = (int16_t)Word(ram, kActRaiserWram_Bg1CameraY);
      if (continuous && track->descent >= 8 && track->impact >= 2 &&
          x >= cam_x - 256 && x <= cam_x + 512 &&
          feet >= cam_y - 160 && feet <= cam_y + 384)
        Spawn(state, at, x, surface, track->impact, room == 4 && source == 0xAF5D, clock);
      track->descent = track->impact = 0;
    } else if (dy < -2) {
      track->descent = track->impact = 0;
    }
    track->valid = 1;
    track->source = (uint16_t)source;
    track->animation = (uint16_t)animation;
    track->x = (int16_t)x;
    track->y = (int16_t)y;
    track->feet_y = (int16_t)feet;
  }
}

void ActionLandingDust_Capture(ActionLandingDustState *state, ActionSceneEffectFrame *frame,
    const uint8_t *wram, size_t size, const ActionBgMapView *map,
    unsigned room, uint16_t clock) {
  if (!state || !frame) return;
  if (!wram || size < 0x1AA0 || !map || room < 2 || room > 4) {
    memset(state, 0, sizeof(*state));
    return;
  }
  unsigned ticks = (uint16_t)(clock - state->clock);
  if (!state->valid || ticks > 4) {
    memset(state, 0, sizeof(*state));
    ticks = 1; /* Seed current contacts, never synthesize an earlier landing. */
  }
  state->valid = 1;
  state->clock = clock;
  for (unsigned i = 0; i < kActionLandingDustPatchCount; i++) {
    const unsigned remaining = state->patches[i].remaining;
    state->patches[i].remaining = (uint16_t)(remaining > ticks ? remaining-ticks : 0);
  }
  for (unsigned i = 0; i < kActionLandingDustMaxPuffs; i++)
    if ((uint16_t)(clock - state->puffs[i].born) >= kActionLandingDustLifetime)
      state->puffs[i].active = 0;
  if (ticks) Observe(state, wram, map, room, ticks, clock);
  for (unsigned i = 0; i < kActionLandingDustMaxPuffs; i++) {
    const ActionLandingDustPuff *p = &state->puffs[i];
    if (!p->active || frame->decoration_count >= kActionSceneDecorationMaxInstances) continue;
    const uint16_t age = (uint16_t)(clock - p->born);
    frame->decorations[frame->decoration_count++] = (ActionEffectInstance){
      .generation = p->generation, .pulse_generation = p->generation,
      .record_address = p->record_address, .world_x = p->x, .world_y = p->y,
      .dust_strength = p->strength, .age_ticks = age, .phase_ticks = age, .pulse_ticks = age,
      .kind = kActionEffect_LandingDust, .phase = kActionEffectPhase_CaveEnvironment,
      .flags = kActionEffectFlag_Visible,
      .render_layer = kActionEffectRenderLayer_WorldDust,
      .projection_plane = kActionEffectProjectionPlane_Bg1,
      .geometry = {.kind = kActionEffectGeometry_Rect, .data.rect = {-104,-72,104,2}},
    };
    ++frame->decoration_visible_count;
  }
}
