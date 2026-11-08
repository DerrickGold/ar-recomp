#include "actraiser/actraiser_death_heim_rush.h"

#include "byte_order.h"
#include <string.h>

static const ActRaiserDeathHeimEncounter s_encounters[] = {
    {0, 1, 1, 1, 1, 0xad45}, {1, 1, 2, 7, 2, 0xf6ca},
    {2, 2, 1, 2, 1, 0xb786}, {3, 2, 2, 7, 3, 0xf6e2},
    {4, 3, 1, 3, 2, 0xc66f}, {5, 3, 2, 7, 4, 0xf6fa},
    {6, 4, 1, 4, 3, 0xd646}, {7, 4, 2, 7, 5, 0xf712},
    {8, 5, 1, 5, 3, 0xd974}, {9, 5, 2, 7, 6, 0xf72a},
    {10, 6, 1, 6, 4, 0xe7c6}, {11, 6, 2, 7, 7, 0xf760},
    {kActRaiserDeathHeimRush_FinalEncounter, 0, 0, 7, 8, 0xf80f},
};

ActRaiserDeathHeimRushConfig ActRaiserDeathHeimRushConfig_Default(void) {
  return (ActRaiserDeathHeimRushConfig){
      .seed = 1, .algorithm = kActRaiserDeathHeimRushAlgorithm};
}

bool ActRaiserDeathHeimRushConfig_Valid(const ActRaiserDeathHeimRushConfig *config) {
  return config && config->algorithm == kActRaiserDeathHeimRushAlgorithm &&
         config->roster <= kActRaiserDeathHeimRoster_Extended &&
         config->seed <= kActRaiserDeathHeimRushSeedMax;
}

bool ActRaiserDeathHeimRushConfig_IsNative(const ActRaiserDeathHeimRushConfig *config) {
  return ActRaiserDeathHeimRushConfig_Valid(config) &&
         config->roster == kActRaiserDeathHeimRoster_Standard && !config->shuffle_order;
}

bool ActRaiserDeathHeimRushConfig_Encode(
    const ActRaiserDeathHeimRushConfig *config,
    uint8_t out[kActRaiserDeathHeimRushConfigBytes]) {
  if (!out || !ActRaiserDeathHeimRushConfig_Valid(config)) return false;
  uint8_t bytes[kActRaiserDeathHeimRushConfigBytes] = "ARDHRSH1";
  bytes[8] = config->algorithm;
  bytes[9] = config->roster;
  bytes[10] = config->shuffle_order;
  ByteOrder_WriteLe32(bytes + 12, config->seed);
  memcpy(out, bytes, sizeof(bytes));
  return true;
}

bool ActRaiserDeathHeimRushConfig_Decode(const uint8_t *bytes, size_t size,
                                       ActRaiserDeathHeimRushConfig *out) {
  if (!bytes || !out || size != kActRaiserDeathHeimRushConfigBytes ||
      memcmp(bytes, "ARDHRSH1", 8) || bytes[10] > 1 || bytes[11])
    return false;
  const ActRaiserDeathHeimRushConfig config = {
      .seed = ByteOrder_ReadLe32(bytes + 12),
      .algorithm = bytes[8],
      .roster = bytes[9],
      .shuffle_order = bytes[10] != 0,
  };
  if (!ActRaiserDeathHeimRushConfig_Valid(&config)) return false;
  *out = config;
  return true;
}

const ActRaiserDeathHeimEncounter *ActRaiserDeathHeimRush_Encounter(unsigned id) {
  return id < sizeof(s_encounters) / sizeof(s_encounters[0]) ? &s_encounters[id] : NULL;
}

/* Algorithm 1: a private SplitMix64 stream, keyed by the persisted seed and
 * roster, followed by descending Fisher-Yates with rejection sampling. These
 * constants and draw order are part of the persisted algorithm contract. */
static uint64_t NextRandom(uint64_t *state) {
  uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));
  value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
  return value ^ (value >> 31);
}

static unsigned RandomBelow(uint64_t *state, unsigned bound) {
  const uint64_t threshold = (UINT64_C(0) - bound) % bound;
  uint64_t value;
  do {
    value = NextRandom(state);
  } while (value < threshold);
  return (unsigned)(value % bound);
}

static uint16_t RequiredMask(const ActRaiserDeathHeimRush *rush) {
  if (!rush || !rush->run_id || !ActRaiserDeathHeimRushConfig_Valid(&rush->config)) return 0;
  switch (rush->config.roster) {
  case kActRaiserDeathHeimRoster_Standard:
    return 0x0aaa;
  case kActRaiserDeathHeimRoster_Act1:
    return 0x0555;
  case kActRaiserDeathHeimRoster_Extended:
    return 0x0fff;
  }
  return 0;
}

bool ActRaiserDeathHeimRush_Active(const ActRaiserDeathHeimRush *rush) {
  return rush && rush->phase >= kActRaiserDeathHeimRush_Hub &&
         rush->phase <= kActRaiserDeathHeimRush_Final && RequiredMask(rush);
}

bool ActRaiserDeathHeimRush_Begin(ActRaiserDeathHeimRush *rush,
                                const ActRaiserDeathHeimRushConfig *config,
                                uint64_t run_id) {
  if (!rush || !ActRaiserDeathHeimRushConfig_Valid(config) || !run_id ||
      run_id <= rush->run_id || ActRaiserDeathHeimRush_Active(rush))
    return false;
  ActRaiserDeathHeimRush next = {
      .config = *config,
      .run_id = run_id,
      .active_id = kActRaiserDeathHeimRush_NoEncounter,
      .last_completed = kActRaiserDeathHeimRush_NoEncounter,
      .phase = kActRaiserDeathHeimRush_Hub,
  };
  for (unsigned city = 0; city < kActRaiserDeathHeimRushCityCount; ++city) {
    if (config->roster != kActRaiserDeathHeimRoster_Standard)
      next.sequence[next.count++] = (uint8_t)(city * 2);
    if (config->roster != kActRaiserDeathHeimRoster_Act1)
      next.sequence[next.count++] = (uint8_t)(city * 2 + 1);
  }
  if (config->shuffle_order) {
    uint64_t state = UINT64_C(0x4448525553480001) ^ config->seed ^
                     ((uint64_t)config->roster << 32);
    for (unsigned i = next.count - 1; i > 0; --i) {
      const unsigned j = RandomBelow(&state, i + 1);
      const uint8_t previous = next.sequence[i];
      next.sequence[i] = next.sequence[j];
      next.sequence[j] = previous;
    }
  }
  *rush = next;
  return true;
}

const ActRaiserDeathHeimEncounter *ActRaiserDeathHeimRush_Current(
    const ActRaiserDeathHeimRush *rush) {
  if (!ActRaiserDeathHeimRush_Active(rush) ||
      rush->count > kActRaiserDeathHeimRushMaxEncounters || rush->index > rush->count)
    return NULL;
  if (rush->phase != kActRaiserDeathHeimRush_Hub)
    return ActRaiserDeathHeimRush_Encounter(rush->active_id);
  return ActRaiserDeathHeimRush_Encounter(
      rush->index == rush->count ? kActRaiserDeathHeimRush_FinalEncounter
                                : rush->sequence[rush->index]);
}

static bool TokenMatches(const ActRaiserDeathHeimRush *rush,
                         ActRaiserDeathHeimRushToken token) {
  return ActRaiserDeathHeimRush_Active(rush) && token.run_id == rush->run_id &&
         token.attempt && token.attempt == rush->attempt &&
         token.encounter_id == rush->active_id;
}

bool ActRaiserDeathHeimRush_Prepare(ActRaiserDeathHeimRush *rush,
                                  ActRaiserDeathHeimRushToken *token) {
  if (!rush || !token ||
      (rush->phase != kActRaiserDeathHeimRush_Hub &&
       rush->phase != kActRaiserDeathHeimRush_Retry) ||
      rush->attempt == UINT32_MAX)
    return false;
  const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeimRush_Current(rush);
  if (!encounter ||
      (encounter->id == kActRaiserDeathHeimRush_FinalEncounter &&
       rush->completed_mask != RequiredMask(rush)))
    return false;
  rush->active_id = encounter->id;
  rush->attempt++;
  rush->phase = kActRaiserDeathHeimRush_Preparing;
  *token = (ActRaiserDeathHeimRushToken){rush->run_id, rush->attempt, encounter->id};
  return true;
}

bool ActRaiserDeathHeimRush_Ready(ActRaiserDeathHeimRush *rush,
                                ActRaiserDeathHeimRushToken token) {
  if (!TokenMatches(rush, token) || rush->phase != kActRaiserDeathHeimRush_Preparing)
    return false;
  rush->phase = rush->active_id == kActRaiserDeathHeimRush_FinalEncounter
                    ? kActRaiserDeathHeimRush_Final
                    : kActRaiserDeathHeimRush_Combat;
  return true;
}

bool ActRaiserDeathHeimRush_Victory(ActRaiserDeathHeimRush *rush,
                                  ActRaiserDeathHeimRushToken token) {
  if (!TokenMatches(rush, token)) return false;
  if (rush->phase == kActRaiserDeathHeimRush_Final &&
      rush->active_id == kActRaiserDeathHeimRush_FinalEncounter &&
      rush->completed_mask == RequiredMask(rush)) {
    rush->last_completed = rush->active_id;
    rush->phase = kActRaiserDeathHeimRush_Complete;
    return true;
  }
  if (rush->phase != kActRaiserDeathHeimRush_Combat ||
      rush->active_id >= kActRaiserDeathHeimRushMaxEncounters)
    return false;
  const uint16_t bit = (uint16_t)(1u << rush->active_id);
  if (!(RequiredMask(rush) & bit) || (rush->completed_mask & bit)) return false;
  rush->completed_mask |= bit;
  rush->last_completed = rush->active_id;
  rush->phase = kActRaiserDeathHeimRush_Returning;
  return true;
}

bool ActRaiserDeathHeimRush_ReturnToHub(ActRaiserDeathHeimRush *rush) {
  if (!ActRaiserDeathHeimRush_Active(rush) ||
      rush->phase != kActRaiserDeathHeimRush_Returning || rush->index >= rush->count ||
      rush->count > kActRaiserDeathHeimRushMaxEncounters ||
      rush->active_id >= kActRaiserDeathHeimRushMaxEncounters ||
      rush->sequence[rush->index] != rush->active_id ||
      !(rush->completed_mask & (1u << rush->active_id)))
    return false;
  rush->index++;
  rush->active_id = kActRaiserDeathHeimRush_NoEncounter;
  rush->phase = kActRaiserDeathHeimRush_Hub;
  return true;
}

bool ActRaiserDeathHeimRush_RetryEncounter(ActRaiserDeathHeimRush *rush,
                                         ActRaiserDeathHeimRushToken token) {
  if (!TokenMatches(rush, token) ||
      (rush->phase != kActRaiserDeathHeimRush_Combat &&
       rush->phase != kActRaiserDeathHeimRush_Final))
    return false;
  rush->phase = kActRaiserDeathHeimRush_Retry;
  return true;
}

bool ActRaiserDeathHeimRush_Abandon(ActRaiserDeathHeimRush *rush) {
  if (!ActRaiserDeathHeimRush_Active(rush)) return false;
  rush->phase = kActRaiserDeathHeimRush_Abandoned;
  rush->active_id = kActRaiserDeathHeimRush_NoEncounter;
  return true;
}

bool ActRaiserDeathHeimRush_FinalReady(const ActRaiserDeathHeimRush *rush) {
  return ActRaiserDeathHeimRush_Active(rush) && rush->phase == kActRaiserDeathHeimRush_Hub &&
         rush->index == rush->count && rush->completed_mask == RequiredMask(rush);
}

uint8_t ActRaiserDeathHeimRush_EyeMask(const ActRaiserDeathHeimRush *rush,
                                     unsigned city) {
  if (city < 1 || city > kActRaiserDeathHeimRushCityCount || !RequiredMask(rush)) return 0;
  const unsigned completed = (rush->completed_mask >> ((city - 1) * 2)) & 3u;
  return rush->config.roster == kActRaiserDeathHeimRoster_Extended
             ? (uint8_t)(3u & ~completed)
             : (uint8_t)(completed ? 0 : 3);
}

uint8_t ActRaiserDeathHeimRush_NativeProgress(const ActRaiserDeathHeimRush *rush) {
  const uint16_t required = RequiredMask(rush);
  if (!required) return 0;
  if (rush->phase == kActRaiserDeathHeimRush_Complete) return 7;
  if (rush->completed_mask == required) return 6;
  unsigned count = 0;
  for (unsigned i = 0; i < kActRaiserDeathHeimRushMaxEncounters; ++i)
    count += (rush->completed_mask >> i) & 1u;
  return (uint8_t)(count < 6 ? count : 5);
}
