#include "actraiser/actraiser_death_heim_rush.h"
#include "support/test_assert.h"

#include <stdio.h>
#include <string.h>

static void CheckConfig(void) {
  const ActRaiserDeathHeimRushConfig defaults = ActRaiserDeathHeimRushConfig_Default();
  assert(defaults.roster == kActRaiserDeathHeimRoster_Standard && !defaults.shuffle_order);
  assert(defaults.seed == 1 && defaults.algorithm == kActRaiserDeathHeimRushAlgorithm);
  assert(ActRaiserDeathHeimRushConfig_IsNative(&defaults));
  assert(!ActRaiserDeathHeimRushConfig_Valid(NULL));
  assert(!ActRaiserDeathHeimRushConfig_IsNative(NULL));
  const uint32_t seeds[] = {0, 1, 16909060, kActRaiserDeathHeimRushSeedMax};
  for (unsigned roster = 0; roster < 3; ++roster) {
    for (unsigned shuffled = 0; shuffled < 2; ++shuffled) {
      for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i) {
        ActRaiserDeathHeimRushConfig config = defaults, decoded = {0};
        config.roster = (uint8_t)roster;
        config.shuffle_order = shuffled != 0;
        config.seed = seeds[i];
        uint8_t bytes[kActRaiserDeathHeimRushConfigBytes];
        assert(ActRaiserDeathHeimRushConfig_Encode(&config, bytes));
        assert(!memcmp(bytes, "ARDHRSH1", 8));
        assert(bytes[8] == 1 && bytes[9] == roster && bytes[10] == shuffled && !bytes[11]);
        assert(ActRaiserDeathHeimRushConfig_Decode(bytes, sizeof(bytes), &decoded));
        assert(decoded.algorithm == config.algorithm && decoded.roster == config.roster);
        assert(decoded.shuffle_order == config.shuffle_order && decoded.seed == config.seed);
        assert(ActRaiserDeathHeimRushConfig_IsNative(&config) == (!roster && !shuffled));
        if (config.seed == 16909060)
          assert(bytes[12] == 4 && bytes[13] == 3 && bytes[14] == 2 && bytes[15] == 1);
      }
    }
  }
  for (unsigned bad = 0; bad < 9; ++bad) {
    uint8_t bytes[kActRaiserDeathHeimRushConfigBytes];
    assert(ActRaiserDeathHeimRushConfig_Encode(&defaults, bytes));
    size_t size = sizeof(bytes);
    switch (bad) {
    case 0: bytes[0] = 0; break;
    case 1: bytes[8] = 0; break;
    case 2: bytes[8] = 2; break;
    case 3: bytes[9] = 3; break;
    case 4: bytes[10] = 2; break;
    case 5: bytes[11] = 1; break;
    case 6: memset(bytes + 12, 255, 4); break;
    case 7: size--; break;
    case 8: size++; break;
    }
    ActRaiserDeathHeimRushConfig decoded = defaults;
    assert(!ActRaiserDeathHeimRushConfig_Decode(bytes, size, &decoded));
    assert(decoded.seed == defaults.seed && decoded.algorithm == defaults.algorithm &&
           decoded.roster == defaults.roster && decoded.shuffle_order == defaults.shuffle_order);
  }
  uint8_t bytes[kActRaiserDeathHeimRushConfigBytes], untouched[sizeof(bytes)];
  memset(bytes, 0x5a, sizeof(bytes));
  memcpy(untouched, bytes, sizeof(bytes));
  ActRaiserDeathHeimRushConfig invalid = defaults;
  invalid.seed = kActRaiserDeathHeimRushSeedMax + 1u;
  assert(!ActRaiserDeathHeimRushConfig_Encode(&invalid, bytes));
  assert(!memcmp(bytes, untouched, sizeof(bytes)));
  assert(!ActRaiserDeathHeimRushConfig_Encode(NULL, bytes));
  assert(!ActRaiserDeathHeimRushConfig_Encode(&defaults, NULL));
  assert(!ActRaiserDeathHeimRushConfig_Decode(NULL, sizeof(bytes), &invalid));
  assert(!ActRaiserDeathHeimRushConfig_Decode(bytes, sizeof(bytes), NULL));
}

static void CheckCatalog(void) {
  const uint8_t act1_maps[] = {1, 1, 2, 3, 3, 4};
  const uint16_t sources[] = {0xad45, 0xf6ca, 0xb786, 0xf6e2, 0xc66f, 0xf6fa, 0xd646,
                              0xf712, 0xd974, 0xf72a, 0xe7c6, 0xf760, 0xf80f};
  for (unsigned id = 0; id < 12; ++id) {
    const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeimRush_Encounter(id);
    assert(encounter && encounter->id == id && encounter->source == sources[id]);
    assert(encounter->city == id / 2 + 1 && encounter->act == id % 2 + 1);
    assert(encounter->map_group == (id % 2 ? 7 : encounter->city));
    assert(encounter->map == (id % 2 ? encounter->city + 1 : act1_maps[id / 2]));
  }
  const ActRaiserDeathHeimEncounter *final = ActRaiserDeathHeimRush_Encounter(12);
  assert(final && !final->city && !final->act && final->map_group == 7 && final->map == 8);
  assert(final->source == sources[12]);
  assert(!ActRaiserDeathHeimRush_Encounter(13));
  assert(!ActRaiserDeathHeimRush_Encounter(UINT32_MAX));
}

static uint16_t ExpectedMask(unsigned roster) {
  return roster == kActRaiserDeathHeimRoster_Extended ? 0xfff
         : roster == kActRaiserDeathHeimRoster_Act1  ? 0x555
                                                    : 0xaaa;
}

static void CheckSequences(void) {
  static const uint8_t expected[3][3][12] = {
      {{3, 5, 7, 11, 9, 1}, {7, 3, 11, 5, 1, 9}, {11, 1, 7, 5, 9, 3}},
      {{10, 0, 8, 2, 4, 6}, {0, 10, 8, 2, 6, 4}, {10, 4, 8, 6, 2, 0}},
      {{7, 11, 1, 9, 0, 6, 3, 10, 4, 8, 5, 2},
       {7, 3, 1, 10, 9, 6, 8, 2, 11, 5, 0, 4},
       {3, 8, 10, 9, 1, 11, 7, 0, 6, 5, 4, 2}},
  };
  const uint32_t vector_seeds[] = {0, 1, kActRaiserDeathHeimRushSeedMax};
  for (unsigned roster = 0; roster < 3; ++roster) {
    ActRaiserDeathHeimRushConfig config = ActRaiserDeathHeimRushConfig_Default();
    config.roster = (uint8_t)roster;
    for (unsigned shuffled = 0; shuffled < 2; ++shuffled) {
      config.shuffle_order = shuffled != 0;
      for (unsigned seed = 0; seed < 1024; ++seed) {
        config.seed = seed;
        ActRaiserDeathHeimRush rush = {0}, again = {0};
        assert(ActRaiserDeathHeimRush_Begin(&rush, &config, 1));
        assert(ActRaiserDeathHeimRush_Begin(&again, &config, 2));
        assert(rush.count == (roster == kActRaiserDeathHeimRoster_Extended ? 12 : 6));
        assert(!memcmp(rush.sequence, again.sequence, sizeof(rush.sequence)));
        uint16_t seen = 0;
        for (unsigned i = 0; i < rush.count; ++i) {
          const unsigned id = rush.sequence[i];
          assert(id < 12 && !(seen & (1u << id)));
          seen |= (uint16_t)(1u << id);
          if (!shuffled)
            assert(id == (roster == kActRaiserDeathHeimRoster_Extended ? i
                          : roster == kActRaiserDeathHeimRoster_Act1  ? i * 2
                                                                    : i * 2 + 1));
        }
        assert(seen == ExpectedMask(roster));
      }
    }
    config.shuffle_order = true;
    for (unsigned i = 0; i < 3; ++i) {
      ActRaiserDeathHeimRush rush = {0};
      config.seed = vector_seeds[i];
      assert(ActRaiserDeathHeimRush_Begin(&rush, &config, 1));
      assert(!memcmp(rush.sequence, expected[roster][i], rush.count));
    }
  }
}

static void CheckEyes(const ActRaiserDeathHeimRush *rush) {
  for (unsigned city = 1; city <= 6; ++city) {
    const unsigned defeated = (rush->completed_mask >> ((city - 1) * 2)) & 3;
    const unsigned expected = rush->config.roster == kActRaiserDeathHeimRoster_Extended
                                  ? 3u & ~defeated
                                  : defeated ? 0 : 3;
    assert(ActRaiserDeathHeimRush_EyeMask(rush, city) == expected);
  }
  assert(!ActRaiserDeathHeimRush_EyeMask(rush, 0));
  assert(!ActRaiserDeathHeimRush_EyeMask(rush, 7));
}

static void CheckInvalidVictory(ActRaiserDeathHeimRush *rush,
                                ActRaiserDeathHeimRushToken token) {
  const ActRaiserDeathHeimRush before = *rush;
  assert(!ActRaiserDeathHeimRush_Victory(rush, token));
  assert(!memcmp(rush, &before, sizeof(before)));
}

static ActRaiserDeathHeimRushToken Retry(ActRaiserDeathHeimRush *rush,
                                       ActRaiserDeathHeimRushToken old) {
  const ActRaiserDeathHeimRush before = *rush;
  assert(ActRaiserDeathHeimRush_RetryEncounter(rush, old));
  assert(!ActRaiserDeathHeimRush_RetryEncounter(rush, old));
  CheckInvalidVictory(rush, old);
  ActRaiserDeathHeimRushToken next;
  assert(ActRaiserDeathHeimRush_Prepare(rush, &next));
  assert(next.encounter_id == old.encounter_id && next.attempt == old.attempt + 1);
  assert(next.run_id == old.run_id);
  assert(ActRaiserDeathHeimRush_Ready(rush, next));
  CheckInvalidVictory(rush, old);
  assert(rush->completed_mask == before.completed_mask && rush->index == before.index);
  assert(rush->last_completed == before.last_completed && rush->count == before.count);
  assert(!memcmp(rush->sequence, before.sequence, sizeof(rush->sequence)));
  CheckEyes(rush);
  return next;
}

static void CheckRun(unsigned roster, bool shuffled) {
  ActRaiserDeathHeimRushConfig config = ActRaiserDeathHeimRushConfig_Default();
  config.roster = (uint8_t)roster;
  config.shuffle_order = shuffled;
  ActRaiserDeathHeimRush rush = {0};
  assert(!ActRaiserDeathHeimRush_Begin(&rush, &config, 0));
  assert(ActRaiserDeathHeimRush_Begin(&rush, &config, 1));
  assert(!ActRaiserDeathHeimRush_Begin(&rush, &config, 2));
  assert(ActRaiserDeathHeimRush_Active(&rush));
  assert(!ActRaiserDeathHeimRush_FinalReady(&rush));
  assert(!ActRaiserDeathHeimRush_NativeProgress(&rush));
  assert(rush.last_completed == kActRaiserDeathHeimRush_NoEncounter);
  CheckEyes(&rush);
  uint16_t mask = 0;
  for (unsigned i = 0; i < rush.count; ++i) {
    assert(rush.index == i && rush.completed_mask == mask);
    const ActRaiserDeathHeimEncounter *encounter = ActRaiserDeathHeimRush_Current(&rush);
    assert(encounter && encounter->id == rush.sequence[i]);
    assert(ActRaiserDeathHeimRush_NativeProgress(&rush) == (i < 6 ? i : 5));
    ActRaiserDeathHeimRushToken token;
    assert(ActRaiserDeathHeimRush_Prepare(&rush, &token));
    CheckInvalidVictory(&rush, token);
    assert(!ActRaiserDeathHeimRush_RetryEncounter(&rush, token));
    assert(ActRaiserDeathHeimRush_Ready(&rush, token));
    assert(!ActRaiserDeathHeimRush_Ready(&rush, token));
    assert(rush.phase == kActRaiserDeathHeimRush_Combat);
    ActRaiserDeathHeimRushToken wrong = token;
    wrong.encounter_id = kActRaiserDeathHeimRush_FinalEncounter;
    CheckInvalidVictory(&rush, wrong);
    wrong = token;
    wrong.run_id++;
    CheckInvalidVictory(&rush, wrong);
    wrong = token;
    wrong.attempt++;
    CheckInvalidVictory(&rush, wrong);
    if (!i || i == rush.count - 1u) token = Retry(&rush, token);
    assert(ActRaiserDeathHeimRush_Victory(&rush, token));
    mask |= (uint16_t)(1u << encounter->id);
    assert(rush.completed_mask == mask && rush.last_completed == encounter->id);
    assert(rush.index == i && ActRaiserDeathHeimRush_Current(&rush) == encounter);
    CheckInvalidVictory(&rush, token);
    assert(!ActRaiserDeathHeimRush_RetryEncounter(&rush, token));
    assert(!ActRaiserDeathHeimRush_FinalReady(&rush));
    CheckEyes(&rush);
    assert(ActRaiserDeathHeimRush_ReturnToHub(&rush));
    assert(!ActRaiserDeathHeimRush_ReturnToHub(&rush));
    CheckInvalidVictory(&rush, token);
  }
  assert(rush.completed_mask == ExpectedMask(roster));
  assert(ActRaiserDeathHeimRush_FinalReady(&rush));
  assert(ActRaiserDeathHeimRush_NativeProgress(&rush) == 6);
  assert(ActRaiserDeathHeimRush_Current(&rush)->id == kActRaiserDeathHeimRush_FinalEncounter);
  ActRaiserDeathHeimRushToken final;
  assert(ActRaiserDeathHeimRush_Prepare(&rush, &final));
  CheckInvalidVictory(&rush, final);
  assert(ActRaiserDeathHeimRush_Ready(&rush, final));
  assert(rush.phase == kActRaiserDeathHeimRush_Final);
  final = Retry(&rush, final);
  assert(ActRaiserDeathHeimRush_Victory(&rush, final));
  assert(rush.phase == kActRaiserDeathHeimRush_Complete);
  assert(rush.last_completed == kActRaiserDeathHeimRush_FinalEncounter);
  assert(!ActRaiserDeathHeimRush_Active(&rush) && !ActRaiserDeathHeimRush_Current(&rush));
  assert(ActRaiserDeathHeimRush_NativeProgress(&rush) == 7);
  CheckInvalidVictory(&rush, final);
  assert(!ActRaiserDeathHeimRush_ReturnToHub(&rush));
  assert(!ActRaiserDeathHeimRush_Abandon(&rush));
  assert(!ActRaiserDeathHeimRush_Begin(&rush, &config, 1));
  assert(ActRaiserDeathHeimRush_Begin(&rush, &config, 2));
  assert(!rush.completed_mask && !rush.index && !rush.attempt);
  CheckInvalidVictory(&rush, final);
}

static void CheckAbandonmentAndGuards(void) {
  ActRaiserDeathHeimRushConfig config = ActRaiserDeathHeimRushConfig_Default();
  for (unsigned phase = 0; phase < 5; ++phase) {
    ActRaiserDeathHeimRush rush = {0};
    ActRaiserDeathHeimRushToken token = {0};
    assert(ActRaiserDeathHeimRush_Begin(&rush, &config, 1));
    if (phase) assert(ActRaiserDeathHeimRush_Prepare(&rush, &token));
    if (phase > 1) assert(ActRaiserDeathHeimRush_Ready(&rush, token));
    if (phase == 3) assert(ActRaiserDeathHeimRush_Victory(&rush, token));
    if (phase == 4) assert(ActRaiserDeathHeimRush_RetryEncounter(&rush, token));
    assert(ActRaiserDeathHeimRush_Abandon(&rush));
    assert(!ActRaiserDeathHeimRush_Active(&rush));
    assert(!ActRaiserDeathHeimRush_Abandon(&rush));
    CheckInvalidVictory(&rush, token);
    assert(ActRaiserDeathHeimRush_Begin(&rush, &config, 2));
    ActRaiserDeathHeimRushToken current;
    assert(ActRaiserDeathHeimRush_Prepare(&rush, &current));
    assert(ActRaiserDeathHeimRush_Ready(&rush, current));
    CheckInvalidVictory(&rush, token);
    assert(ActRaiserDeathHeimRush_Victory(&rush, current));
  }
  ActRaiserDeathHeimRush rush = {0};
  ActRaiserDeathHeimRushToken token = {9, 9, 9}, untouched = token;
  assert(!ActRaiserDeathHeimRush_Prepare(&rush, &token));
  assert(!memcmp(&token, &untouched, sizeof(token)));
  assert(!ActRaiserDeathHeimRush_Active(NULL));
  assert(!ActRaiserDeathHeimRush_Current(NULL));
  assert(!ActRaiserDeathHeimRush_FinalReady(NULL));
  assert(!ActRaiserDeathHeimRush_EyeMask(NULL, 1));
  assert(!ActRaiserDeathHeimRush_NativeProgress(NULL));
  assert(!ActRaiserDeathHeimRush_Begin(NULL, &config, 1));
  assert(!ActRaiserDeathHeimRush_Begin(&rush, NULL, 1));
  assert(!ActRaiserDeathHeimRush_Victory(NULL, token));
  assert(!ActRaiserDeathHeimRush_Ready(NULL, token));
  assert(!ActRaiserDeathHeimRush_RetryEncounter(NULL, token));
  assert(!ActRaiserDeathHeimRush_ReturnToHub(NULL));
  assert(!ActRaiserDeathHeimRush_Abandon(NULL));
  assert(ActRaiserDeathHeimRush_Begin(&rush, &config, 1));
  assert(!ActRaiserDeathHeimRush_Prepare(&rush, NULL));
  rush.index = rush.count;
  assert(!ActRaiserDeathHeimRush_FinalReady(&rush));
  assert(!ActRaiserDeathHeimRush_Prepare(&rush, &token));
  rush.index = 0;
  rush.attempt = UINT32_MAX;
  assert(!ActRaiserDeathHeimRush_Prepare(&rush, &token));
  assert(!memcmp(&token, &untouched, sizeof(token)));
}

int main(void) {
  CheckConfig();
  CheckCatalog();
  CheckSequences();
  for (unsigned roster = 0; roster < 3; ++roster)
    for (unsigned shuffled = 0; shuffled < 2; ++shuffled)
      CheckRun(roster, shuffled != 0);
  CheckAbandonmentAndGuards();
  puts("actraiser_death_heim_rush_test: PASS");
  return 0;
}
