#include "actraiser/actraiser_death_heim_eyes.h"

#include "actraiser/actraiser_death_heim_runtime.h"
#include "actraiser_game.h"

static struct {
  uint16_t frame;
  uint8_t next_city, next_act, last_city, last_act;
} s_visit;

void ActRaiserDeathHeimEyes_BeginHub(CpuState *cpu,
    const ActRaiserDeathHeimEncounter *next,
    const ActRaiserDeathHeimEncounter *last) {
  s_visit.frame = cpu_read16(cpu, 0, kActRaiserWram_GameFrame);
  s_visit.next_city = next ? next->city : 0;
  s_visit.next_act = next ? next->act : 0;
  s_visit.last_city = last ? last->city : 0;
  s_visit.last_act = last ? last->act : 0;
}

/* Each original duration is an inclusive down-counter. Reading the resident
 * sequences preserves the native fade timing and regional frame choices. */
static unsigned Visual(CpuState *cpu, unsigned state, unsigned age) {
  uint16_t at = (uint16_t)(0x4000 + cpu_read16(cpu, 0x7e,
      (uint16_t)(0x4002 + state * 2)));
  unsigned result = 24;
  for (unsigned frame = 0; frame < 32; ++frame, at = (uint16_t)(at + 4)) {
    const unsigned visual = cpu_read8(cpu, 0x7e, at);
    if (visual == 255) return result;
    result = visual;
    const unsigned duration = cpu_read8(cpu, 0x7e, (uint16_t)(at + 1)) + 1;
    if (age < duration) return visual;
    age -= duration;
  }
  return result;
}

static unsigned EyeVisual(CpuState *cpu, unsigned city, unsigned act,
    unsigned age) {
  const bool aitos = city == 4;
  if (aitos) {
    /* This profile statue has only one visible eye. Flash its original glow
     * after either victory, keeping it lit until both city bosses are beaten. */
    if (s_visit.last_city == city) {
      const unsigned visual = Visual(cpu, 13, age);
      if (visual != 24 || !ActRaiserDeathHeim_EyeMask(city)) return visual;
    }
    if (!ActRaiserDeathHeim_EyeMask(city)) return 24;
    return s_visit.next_city == city ? Visual(cpu, 12, age) : 12;
  }
  if (s_visit.last_city == city && s_visit.last_act == act)
    return Visual(cpu, 2, age);
  if (!(ActRaiserDeathHeim_EyeMask(city) & (1u << (act - 1)))) return 24;
  if (s_visit.next_city == city && s_visit.next_act == act)
    return Visual(cpu, 1, age);
  return 1;
}

bool ActRaiserDeathHeimEyes_Draw(CpuState *cpu, uint16_t owner,
    bool flip_x, bool flip_y, int16_t left, int16_t top,
    ActRaiserDeathHeimEyesDraw *out) {
  if (!cpu || !out) return false;
  const unsigned city = ActRaiserDeathHeim_EyeActorCity(cpu, owner);
  const unsigned source = cpu_read16(cpu, 0, (uint16_t)(owner + 0x32));
  if (!city || (source != 0xf408 && source != 0xf3fa && source != 0xf430)) return false;
  *out = (ActRaiserDeathHeimEyesDraw){0};
  const unsigned age = (uint16_t)(cpu_read16(cpu, 0, kActRaiserWram_GameFrame) - s_visit.frame);
  const uint16_t table = (uint16_t)(0x4000 + cpu_read16(cpu, 0x7e, 0x4000));
  for (unsigned act = 1; act <= (city == 4 ? 1u : 2u); ++act) {
    const unsigned visual = EyeVisual(cpu, city, act, age);
    if (visual == 24) continue;
    const uint16_t composition = (uint16_t)(0x4000 +
        cpu_read16(cpu, 0x7e, (uint16_t)(table + visual * 2)));
    ActRaiserActorArtDraw regional = {0};
    const bool donor = ActRaiserActorArt_Active() &&
        ActRaiserActorArt_Draw(0x4000, composition, visual, &regional);
    const unsigned count = cpu_read8(cpu, 0x7e, (uint16_t)(composition + 4));
    /* Both validated hub artworks share these small eye compositions. */
    if (count != (city == 4 ? 2u : 3u) ||
        (donor && regional.picture.count != count)) continue;
    const bool source_right = (act == 2) != flip_x;
    for (unsigned index = 0; index < count; ++index) {
      const uint16_t address = (uint16_t)(composition + 5 + index * 7);
      const unsigned source_x = cpu_read8(cpu, 0x7e, (uint16_t)(address + 1));
      if (city != 4 && (source_x == 16) != source_right) continue;
      ActRaiserActorArtPart part = {
        .x = cpu_read8(cpu, 0x7e, (uint16_t)(address + 1 + flip_x)) + left -
            (int8_t)cpu_read8(cpu, 0x7e, (uint16_t)(composition + flip_x)),
        .y = cpu_read8(cpu, 0x7e, (uint16_t)(address + 3 + flip_y)) + top -
            (int8_t)cpu_read8(cpu, 0x7e, (uint16_t)(composition + 2 + flip_y)),
        .attributes = cpu_read16(cpu, 0x7e, (uint16_t)(address + 5)),
        .large = false,
      };
      if (donor) ActRaiserActorArt_ResolvePart(&regional, index, flip_x,
          flip_y, left, top, &part);
      if (out->count < kActRaiserDeathHeimEyeParts)
        out->parts[out->count++] = part;
    }
  }
  return true;
}

int ActRaiserDeathHeimEyes_FlareOffset(CpuState *cpu, uint16_t owner) {
  const unsigned city = ActRaiserDeathHeim_EyeActorCity(cpu, owner);
  if (!city || city != s_visit.last_city) return 0;
  const unsigned source = cpu_read16(cpu, 0, (uint16_t)(owner + 0x32));
  if (source != 0xf494 && source != 0xf486) return 0;
  const unsigned state = cpu_read16(cpu, 0,
      (uint16_t)(owner + kActRaiserActionObject_AnimationState));
  const unsigned visual = cpu_read16(cpu, 0,
      (uint16_t)(owner + kActRaiserActionObject_Visual));
  /* State 6 begins with the original forehead spark (visual 0) for 90
   * updates, then plays burst visuals 25..38 and ends blank (visual 24).
   * Only the burst follows the extinguished eye. Moving the whole actor
   * also pulls the forehead detail off its original statue anchor. */
  if (state != 6 || visual < 25 || visual > 38) return 0;
  /* Retain the complete original burst, centered toward the eye just lost.
   * The surviving glow is composed independently and never fades with it. */
  return city == 4 ? 0 : (s_visit.last_act == 1 ? -6 : 6);
}
