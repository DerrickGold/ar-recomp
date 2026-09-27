/* Revision-checked choices and the gameplay boundaries that activate them. */
#include "regional_session_test.h"
#include "support/test_check.h"

#include <string.h>

static int s_failures;
#define CHECK(condition) AR_TEST_CHECK(s_failures, condition)

static void CheckActorArtwork(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  for (unsigned choices = 0; choices < 2187; ++choices) {
    unsigned digits = choices;
    ArRegionalActorArtworkPolicy policy;
    for (unsigned i = 0; i < 7; ++i) {
      policy.source[i] = digits % 3;
      digits /= 3;
    }
    CHECK(!ArRegionalSession_RequestActorArtwork(&session, session.revision - 1, &policy));
    CHECK(ArRegionalSession_RequestActorArtwork(&session, session.revision, &policy));
    for (unsigned area = 0; area < 7; ++area) {
      const ArRegionalActorArtworkPolicy before = session.effective.actor_artwork;
      bool enabled = false;
      CHECK(ArRegionalSession_BeginActorArtwork(&session, area, &enabled) &&
            enabled == (policy.source[area] == 1));
      for (unsigned i = 0; i < 7; ++i)
        CHECK(session.effective.actor_artwork.source[i] ==
              (i == area ? policy.source[i] : before.source[i]));
    }
  }
  bool enabled = true;
  CHECK(!ArRegionalSession_BeginActorArtwork(&session, 7, &enabled) && enabled);
  CHECK(!ArRegionalSession_BeginActorArtwork(NULL, 0, &enabled));
  CHECK(!ArRegionalSession_BeginActorArtwork(&session, 0, NULL));
  CHECK(!ArRegionalSession_RequestActorArtwork(&session, session.revision, NULL));
  ArRegionalActorArtworkPolicy bad = {{0}};
  bad.source[6] = 3;
  CHECK(!ArRegionalSession_RequestActorArtwork(&session, session.revision, &bad));
  session.revision = UINT32_MAX;
  CHECK(ArRegionalSession_RequestActorArtwork(&session, UINT32_MAX,
                                              &session.requested.actor_artwork));
  bad.source[6] = 0;
  CHECK(!ArRegionalSession_RequestActorArtwork(&session, UINT32_MAX, &bad));
  session.requested.actor_artwork.source[0] = 1;
  enabled = false;
  CHECK(!ArRegionalSession_BeginActorArtwork(&session, 0, &enabled) && !enabled);
}

static void CheckFire(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  for (unsigned n = 0; n < 81; ++n) {
    ArRegionalFirePolicy policy;
    unsigned digits = n;
    for (unsigned i = 0; i < 4; ++i) {
      policy.source[i] = digits % 3;
      digits /= 3;
    }
    uint8_t snapshot = 255, expected;
    CHECK(ArRegionalFire_Resolve(&policy, &expected));
    const ArRegionalSession before = session;
    CHECK(!ArRegionalSession_RequestFire(&session, session.revision - 1, &policy) &&
          RegionalSessionTest_Equal(&before, &session));
    CHECK(ArRegionalSession_RequestFire(&session, session.revision, &policy));
    CHECK(!memcmp(&before.effective.fire_enemy, &session.effective.fire_enemy, sizeof(policy)));
    CHECK(ArRegionalSession_BeginFire(&session, &snapshot) && snapshot == expected);
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_BeginFire(&session, &snapshot) && session.revision == revision);
  }
  ArRegionalFirePolicy policy = {{1, 0, 2, 1}};
  CHECK(ArRegionalSession_RequestFire(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  uint8_t snapshot = 99;
  CHECK(!ArRegionalSession_BeginFire(&session, &snapshot) && snapshot == 99 &&
        RegionalSessionTest_Equal(&before, &session));
  policy.source[0] = 3;
  CHECK(!ArRegionalSession_RequestFire(&session, session.revision, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalFire_Resolve(&policy, &snapshot) && snapshot == 99);
}

static void CheckCastHold(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  for (unsigned n = 0; n < 27; ++n) {
    ArRegionalCastHoldPolicy policy;
    unsigned digits = n;
    for (unsigned i = 0; i < 3; ++i) {
      policy.source[i] = digits % 3;
      digits /= 3;
    }
    uint8_t snapshot = 255, expected;
    CHECK(ArRegionalCastHold_Resolve(&policy, &expected));
    const ArRegionalSession before = session;
    CHECK(!ArRegionalSession_RequestCastHold(&session, session.revision - 1, &policy) &&
          RegionalSessionTest_Equal(&before, &session));
    CHECK(ArRegionalSession_RequestCastHold(&session, session.revision, &policy));
    CHECK(!memcmp(&before.effective.cast_hold, &session.effective.cast_hold, sizeof(policy)));
    CHECK(ArRegionalSession_BeginCastHold(&session, &snapshot) && snapshot == expected);
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_BeginCastHold(&session, &snapshot) && session.revision == revision);
  }
  ArRegionalCastHoldPolicy policy = {{0, 2, 1}};
  CHECK(ArRegionalSession_RequestCastHold(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  uint8_t snapshot = 99;
  CHECK(!ArRegionalSession_BeginCastHold(&session, &snapshot) && snapshot == 99 &&
        RegionalSessionTest_Equal(&before, &session));
  policy.source[0] = 3;
  CHECK(!ArRegionalSession_RequestCastHold(&session, session.revision, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalCastHold_Resolve(&policy, &snapshot) && snapshot == 99);
}

static void CheckActorStats(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  for (unsigned i = 0; i < kArRegionalActorStat_Count; ++i)
    for (unsigned source = 0; source < 3; ++source) {
      ArRegionalActorStatsPolicy policy = {{0}};
      policy.source[i] = source;
      ArRegionalActorStatsSnapshot snapshot, expected;
      CHECK(ArRegionalActorStats_Resolve(&policy, &expected));
      const ArRegionalSession before = session;
      CHECK(!ArRegionalSession_RequestActorStats(&session, session.revision - 1, &policy) &&
            RegionalSessionTest_Equal(&before, &session));
      CHECK(ArRegionalSession_RequestActorStats(&session, session.revision, &policy));
      CHECK(!memcmp(&before.effective.actor_stats, &session.effective.actor_stats, sizeof(policy)));
      CHECK(ArRegionalSession_BeginActorStats(&session, &snapshot) &&
            !memcmp(&snapshot, &expected, sizeof(snapshot)));
      const uint32_t revision = session.revision;
      CHECK(ArRegionalSession_BeginActorStats(&session, &snapshot) && session.revision == revision);
    }
  ArRegionalActorStatsPolicy policy;
  CHECK(ArRegionalActorStats_Init(&policy, 1));
  CHECK(ArRegionalSession_RequestActorStats(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  ArRegionalActorStatsSnapshot snapshot, saved;
  memset(&snapshot, 0x5a, sizeof(snapshot));
  saved = snapshot;
  CHECK(!ArRegionalSession_BeginActorStats(&session, &snapshot) &&
        !memcmp(&snapshot, &saved, sizeof(snapshot)) &&
        RegionalSessionTest_Equal(&before, &session));
  policy.source[0] = 3;
  CHECK(!ArRegionalSession_RequestActorStats(&session, session.revision, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
}

static void CheckPlatformSkull(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  for (unsigned n = 0; n < 81; ++n) {
    ArRegionalPlatformSkullPolicy policy;
    unsigned digits = n;
    for (unsigned i = 0; i < 4; ++i) {
      policy.source[i] = digits % 3;
      digits /= 3;
    }
    uint8_t snapshot = 255, expected;
    CHECK(ArRegionalPlatformSkull_Resolve(&policy, &expected));
    const ArRegionalSession before = session;
    CHECK(!ArRegionalSession_RequestPlatformSkull(&session, session.revision - 1, &policy) &&
          RegionalSessionTest_Equal(&before, &session));
    CHECK(ArRegionalSession_RequestPlatformSkull(&session, session.revision, &policy));
    CHECK(!memcmp(&before.effective.platform_skull, &session.effective.platform_skull,
                  sizeof(policy)));
    CHECK(ArRegionalSession_BeginPlatformSkull(&session, &snapshot) && snapshot == expected);
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_BeginPlatformSkull(&session, &snapshot) &&
          session.revision == revision);
  }
  ArRegionalPlatformSkullPolicy policy = {{1, 0, 0, 0}};
  CHECK(ArRegionalSession_RequestPlatformSkull(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  uint8_t snapshot = 255;
  CHECK(!ArRegionalSession_BeginPlatformSkull(&session, &snapshot) && snapshot == 255 &&
        RegionalSessionTest_Equal(&before, &session));
  policy.source[0] = 3;
  CHECK(!ArRegionalSession_RequestPlatformSkull(&session, session.revision, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalPlatformSkull_Resolve(&policy, &snapshot) && snapshot == 255);
}

static void CheckCollision(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  for (unsigned n = 0; n < 9; ++n) {
    ArRegionalCollisionPolicy policy = {{n % 3, n / 3}};
    uint8_t snapshot = 255, expected = (n % 3 == 1 ? 1 : 0) | (n / 3 == 1 ? 2 : 0);
    const ArRegionalSession before = session;
    CHECK(!ArRegionalSession_RequestCollision(&session, session.revision - 1, &policy) &&
          RegionalSessionTest_Equal(&before, &session));
    CHECK(ArRegionalSession_RequestCollision(&session, session.revision, &policy));
    CHECK(!memcmp(&before.effective.collision, &session.effective.collision, sizeof(policy)));
    CHECK(ArRegionalSession_BeginCollision(&session, &snapshot) && snapshot == expected);
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_BeginCollision(&session, &snapshot) && session.revision == revision);
  }
  ArRegionalCollisionPolicy policy = {{1, 0}};
  CHECK(ArRegionalSession_RequestCollision(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  uint8_t snapshot = 255;
  CHECK(!ArRegionalSession_BeginCollision(&session, &snapshot) && snapshot == 255 &&
        RegionalSessionTest_Equal(&before, &session));
  policy.source[0] = 3;
  CHECK(!ArRegionalSession_RequestCollision(&session, session.revision, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalCollision_Resolve(&policy, &snapshot) && snapshot == 255);
}

static void CheckVolley(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  for (unsigned source = 0; source < 3; ++source) {
    const ArRegionalSession before = session;
    bool double_shot = false;
    CHECK(!ArRegionalSession_RequestVolley(&session, session.revision - 1, source) &&
          RegionalSessionTest_Equal(&before, &session));
    CHECK(!ArRegionalSession_RequestVolley(&session, session.revision, 3) &&
          RegionalSessionTest_Equal(&before, &session));
    CHECK(ArRegionalSession_RequestVolley(&session, session.revision, source));
    CHECK(session.effective.statue_volley == before.effective.statue_volley);
    CHECK(ArRegionalSession_BeginVolley(&session, &double_shot) && double_shot == (source != 0));
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_BeginVolley(&session, &double_shot) && session.revision == revision);
    CHECK(ArRegionalVolley_Descriptor()->shots[source] == 1u + double_shot);
  }
  CHECK(ArRegionalSession_RequestVolley(&session, session.revision, 0));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  bool double_shot = true;
  CHECK(!ArRegionalSession_BeginVolley(&session, &double_shot) && double_shot &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestVolley(&session, session.revision, 1) &&
        RegionalSessionTest_Equal(&before, &session));
}

static void CheckBosses(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  const unsigned bounds[] = {0, 7, 11, 13, 15, 19, 22, 26, 30, kArRegionalBoss_Count},
                 combinations[] = {2187, 81, 9, 9, 81, 27, 81, 81, 3};
  for (unsigned group = 0; group < 9; ++group)
    for (unsigned n = 0; n < combinations[group]; ++n) {
      ArRegionalBossPolicy policy = {{0}};
      unsigned digits = n;
      for (unsigned i = bounds[group]; i < bounds[group + 1]; ++i) {
        policy.source[i] = digits % 3;
        digits /= 3;
      }
      uint64_t expected = UINT64_MAX, snapshot = UINT64_MAX;
      CHECK(ArRegionalBoss_Resolve(&policy, &expected));
      const ArRegionalSession before = session;
      CHECK(!ArRegionalSession_RequestBosses(&session, session.revision - 1, &policy) &&
            RegionalSessionTest_Equal(&before, &session));
      CHECK(ArRegionalSession_RequestBosses(&session, session.revision, &policy));
      CHECK(!memcmp(&before.effective.bosses, &session.effective.bosses, sizeof(policy)));
      CHECK(ArRegionalSession_BeginBosses(&session, &snapshot) && snapshot == expected);
      const uint32_t revision = session.revision;
      CHECK(ArRegionalSession_BeginBosses(&session, &snapshot) && session.revision == revision);
      for (unsigned i = 0; i < kArRegionalBoss_Count; ++i)
        CHECK(ArRegionalBoss_Value(snapshot, i) ==
              ArRegionalBoss_Descriptor(i)->value[policy.source[i]]);
    }
  ArRegionalBossPolicy policy = {{0}};
  CHECK(ArRegionalSession_RequestBosses(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  uint64_t snapshot = UINT64_MAX;
  CHECK(!ArRegionalSession_BeginBosses(&session, &snapshot) && snapshot == UINT64_MAX &&
        RegionalSessionTest_Equal(&before, &session));
  policy.source[0] = 3;
  CHECK(!ArRegionalSession_RequestBosses(&session, session.revision, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalBoss_Resolve(&policy, &snapshot) && snapshot == UINT64_MAX);
}

static void CheckArrival(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  for (unsigned source = 0; source < 3; ++source)
    for (unsigned continuing = 0; continuing < 2; ++continuing) {
      ArRegionalSession session;
      CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
      CHECK(ArRegionalSession_RequestArrival(&session, session.revision, (ArRegionalSource)source));
      CHECK(!session.arrival_locked && session.effective.arrival == 0);
      bool japanese;
      CHECK(ArRegionalSession_BeginArrival(&session, continuing, &japanese));
      CHECK(session.arrival_locked && japanese == (source == 1 && !continuing));
      const uint32_t revision = session.revision;
      CHECK(ArRegionalSession_BeginArrival(&session, false, &japanese) &&
            revision == session.revision);
      CHECK(ArRegionalSession_RequestArrival(&session, session.revision, source == 1 ? 0 : 1));
      CHECK(ArRegionalSession_BeginArrival(&session, false, &japanese));
      CHECK(japanese == (source == 1 && !continuing));
      const ArRegionalSession before = session;
      CHECK(!ArRegionalSession_RequestArrival(&session, session.revision - 1, 2) &&
            RegionalSessionTest_Equal(&session, &before));
      CHECK(!ArRegionalSession_RequestArrival(&session, session.revision, 3) &&
            RegionalSessionTest_Equal(&session, &before));
    }
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  bool japanese = true;
  CHECK(!ArRegionalSession_BeginArrival(&session, false, &japanese) && japanese &&
        RegionalSessionTest_Equal(&before, &session));
}

static void CheckActionMotion(void) {
  const uint8_t id[16] = {1};
  const ArRegionalCostPolicy defaults = {{0}};
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  /* Exhaust each independently stored group, retaining nonzero peers across
   * groups. Avoid multiplying this session/codec test by every new leaf. */
  const unsigned boundaries[] = {0, 7, kArRegionalActionMotion_Count};
  for (unsigned group = 0; group < 2; ++group) {
    unsigned combinations = 1;
    for (unsigned i = boundaries[group]; i < boundaries[group + 1]; ++i)
      combinations *= 3;
    for (unsigned combination = 0; combination < combinations; ++combination) {
      ArRegionalActionMotionPolicy policy;
      unsigned digits = combination;
      uint16_t expected = 0, snapshot = 0xffff;
      for (unsigned i = 0; i < kArRegionalActionMotion_Count; ++i) {
        policy.source[i] = (i + group) % 3;
        if (i >= boundaries[group] && i < boundaries[group + 1]) {
          policy.source[i] = digits % 3;
          digits /= 3;
        }
        const ArRegionalActionMotionDescriptor *desc =
            ArRegionalActionMotion_Descriptor((ArRegionalActionMotionRule)i);
        if (desc->value[policy.source[i]] != desc->value[0]) expected |= 1u << i;
      }
      const ArRegionalSession before = session;
      CHECK(!ArRegionalSession_RequestActionMotion(&session, session.revision - 1, &policy) &&
            RegionalSessionTest_Equal(&before, &session));
      CHECK(ArRegionalSession_RequestActionMotion(&session, session.revision, &policy));
      CHECK(!memcmp(&session.effective.action_motion, &before.effective.action_motion,
                    sizeof(policy)));
      CHECK(ArRegionalSession_BeginActionMotion(&session, &snapshot) && snapshot == expected);
      const uint32_t revision = session.revision;
      CHECK(ArRegionalSession_BeginActionMotion(&session, &snapshot) &&
            revision == session.revision);
    }
  }
  ArRegionalActionMotionPolicy policy;
  CHECK(ArRegionalActionMotion_Init(&policy, 1));
  CHECK(ArRegionalSession_RequestActionMotion(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  ArRegionalSession before = session;
  uint16_t out = 0xaaaa;
  CHECK(!ArRegionalSession_BeginActionMotion(&session, &out) && out == 0xaaaa &&
        RegionalSessionTest_Equal(&before, &session));
}

static void CheckEmitters(void) {
  const uint8_t id[16] = {13};
  ArRegionalSession session;
  const ArRegionalCostPolicy costs = {{0}};
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  for (unsigned cadence = 0; cadence < 3; ++cadence)
    for (unsigned position = 0; position < 3; ++position) {
      const ArRegionalEmitterPolicy policy = {{cadence, position}};
      const ArRegionalSession before = session;
      uint8_t snapshot = 255;
      CHECK(!ArRegionalSession_RequestEmitters(&session, session.revision - 1, &policy) &&
            RegionalSessionTest_Equal(&session, &before));
      CHECK(ArRegionalSession_RequestEmitters(&session, session.revision, &policy));
      CHECK(!memcmp(&before.effective.emitters, &session.effective.emitters, sizeof(policy)));
      CHECK(ArRegionalSession_BeginEmitters(&session, &snapshot) &&
            snapshot == (cadence | (position == 2 ? 4 : 0)));
      const uint32_t revision = session.revision;
      CHECK(ArRegionalSession_BeginEmitters(&session, &snapshot) && session.revision == revision);
    }
  const ArRegionalEmitterPolicy policy = {{0, 0}};
  CHECK(ArRegionalSession_RequestEmitters(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  uint8_t snapshot = 255;
  CHECK(!ArRegionalSession_BeginEmitters(&session, &snapshot) && snapshot == 255 &&
        RegionalSessionTest_Equal(&session, &before));
}

static void CheckPopulation(void) {
  ArRegionalRules rules = {0};
  for (unsigned combination = 0; combination < 243; ++combination) {
    unsigned digits = combination;
    bool reduced = false;
    for (unsigned i = 0; i < 5; ++i) {
      rules.support.source[i] = digits % 3;
      reduced |= digits % 3 == 1;
      digits /= 3;
    }
    for (unsigned level = 0; level < 3; ++level)
      for (unsigned hint = 0; hint < 3; ++hint)
        for (unsigned tablet = 0; tablet < 3; ++tablet) {
          rules.level_goals = level;
          rules.story.source[0] = hint;
          rules.story.source[1] = tablet;
          CHECK(ArRegionalRules_PopulationCompatible(&rules) ==
                (!reduced || (level == 1 && hint == 1 && tablet == 1)));
        }
  }
  const uint8_t id[16] = {99};
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 99);
  SaveError error;
  const char *path = "regional-population.srm";
  remove(path);
  remove("regional-population.srm.archeckpoint");
  ArRegionalSession session, loaded;
  ArRegionalCostPolicy defaults = {{0}};
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  session.requested.story.source[2] = session.effective.story.source[2] = 1;
  for (unsigned n = 0; n < 6; ++n) {
    const ArRegionalSource source = (n + 1) % 3;
    ArRegionalSession before = session;
    uint32_t revision = session.revision;
    CHECK(!ArRegionalSession_SetPopulationProfile(&session, revision - 1, source) &&
          RegionalSessionTest_Equal(&session, &before));
    CHECK(ArRegionalSession_SetPopulationProfile(&session, revision, source));
    CHECK(session.revision == revision + 1 && session.requested.level_goals == source &&
          session.effective.level_goals == source);
    CHECK(session.requested.story.source[0] == source &&
          session.effective.story.source[1] == source);
    CHECK(session.requested.story.source[2] == 1 && session.effective.story.source[2] == 1);
    for (unsigned i = 0; i < 5; ++i)
      CHECK(session.requested.support.source[i] == source &&
            session.effective.support.source[i] == source);
    before = session;
    CHECK(ArRegionalSession_SetPopulationProfile(&session, session.revision, source) &&
          RegionalSessionTest_Equal(&session, &before));
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, n ? image : NULL, image,
                                 &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&loaded, &session));
    if (source == 1) {
      CHECK(!ArRegionalSession_RequestLevelGoals(&session, session.revision, 0) &&
            RegionalSessionTest_Equal(&session, &before));
      ArRegionalStoryPolicy story = session.requested.story;
      story.source[0] = 0;
      CHECK(!ArRegionalSession_RequestStory(&session, session.revision, &story) &&
            RegionalSessionTest_Equal(&session, &before));
      story = session.requested.story;
      story.source[1] = 2;
      CHECK(!ArRegionalSession_RequestStory(&session, session.revision, &story) &&
            RegionalSessionTest_Equal(&session, &before));
      story = session.requested.story;
      story.source[2] = 0;
      CHECK(ArRegionalSession_RequestStory(&session, session.revision, &story));
      story.source[2] = 1;
      CHECK(ArRegionalSession_RequestStory(&session, session.revision, &story));
    }
  }
  session.revision = UINT32_MAX;
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_SetPopulationProfile(&session, session.revision, 1) &&
        RegionalSessionTest_Equal(&session, &before));
  CHECK(
      !ArRegionalSession_SetPopulationProfile(&session, session.revision, kArRegionalSource_Count));
  session.revision = 1;
  session.requested.support.source[0] = 1;
  CHECK(!ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  remove(path);
  remove("regional-population.srm.archeckpoint");
}

static void CheckConstruction(void) {
  const uint8_t id[16] = {98};
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 98);
  SaveError error;
  const char *path = "regional-construction.srm";
  remove(path);
  remove("regional-construction.srm.archeckpoint");
  ArRegionalSession session, loaded;
  ArRegionalCostPolicy defaults = {{0}};
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  for (unsigned n = 0; n < 6; ++n) {
    const ArRegionalSource source = (ArRegionalSource)((n + 1) % 3);
    const ArRegionalSource old = session.effective.construction;
    CHECK(ArRegionalSession_RequestConstruction(&session, session.revision, source));
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_RequestConstruction(&session, revision, source) &&
          session.revision == revision);
    CHECK(!ArRegionalSession_RequestConstruction(&session, revision - 1, source));
    CHECK(session.effective.construction == old);
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, n ? image : NULL, image,
                                 &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&loaded, &session));
    bool jp = false;
    CHECK(ArRegionalSession_BeginConstruction(&session, &jp) && jp == (source == 1));
    CHECK(session.revision == revision + 1 && session.effective.construction == source);
    CHECK(ArRegionalSession_BeginConstruction(&session, &jp) && session.revision == revision + 1);
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&loaded, &session));
  }
  ArRegionalSession before = session;
  bool jp = true;
  CHECK(!ArRegionalSession_RequestConstruction(&session, session.revision, 3) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_BeginConstruction(&session, NULL) &&
        RegionalSessionTest_Equal(&before, &session));
  session.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_RequestConstruction(&session, session.revision, 1));
  session.requested.construction = 1;
  before = session;
  CHECK(!ArRegionalSession_BeginConstruction(&session, &jp) && jp &&
        RegionalSessionTest_Equal(&before, &session));
  remove(path);
  remove("regional-construction.srm.archeckpoint");
}

static void CheckSimCombat(void) {
  const uint8_t id[16] = {97};
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 97);
  SaveError error;
  const char *path = "regional-sim-combat.srm";
  ArRegionalSession session, loaded;
  ArRegionalCostPolicy defaults = {{0}};
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &defaults));
  ArRegionalSimCombatPolicy policy;
  CHECK(ArRegionalSimCombat_Init(&policy, 1));
  CHECK(ArRegionalSession_RequestSimCombat(&session, session.revision, &policy));
  ArRegionalSimAiPolicy ai;
  CHECK(ArRegionalSimAi_Init(&ai, 1));
  CHECK(ArRegionalSession_RequestSimAi(&session, session.revision, &ai));
  CHECK(!ArRegionalSession_RequestSimAi(&session, session.revision - 1, &ai));
  const ArRegionalSession requested = session;
  CHECK(!ArRegionalSession_RequestSimCombat(&session, session.revision - 1, &policy));
  CHECK(!ArRegionalSession_BeginSimActor(&session, 0, 0) &&
        RegionalSessionTest_Equal(&session, &requested));
  CHECK(ArRegionalSimActors_LoadTown(&session.sim_actors, 0));
  CHECK(ArRegionalSession_BeginSimActor(&session, 0, 0) &&
        session.sim_actors.active[0].combat == 31);
  CHECK(session.sim_actors.active[0].ai == 63 && session.revision == requested.revision + 1);
  CHECK(ArRegionalSimActors_SaveTown(&session.sim_actors, 0));
  CHECK(ArRegionalSimCombat_Init(&policy, 0) &&
        ArRegionalSession_RequestSimCombat(&session, session.revision, &policy));
  ai.source[kArRegionalSimAi_TargetPool] = 0;
  CHECK(ArRegionalSession_RequestSimAi(&session, session.revision, &ai));
  CHECK(ArRegionalSession_BeginSimActor(&session, 0, 1) && !session.sim_actors.active[1].combat &&
        session.sim_actors.active[0].combat == 31);
  CHECK(session.sim_actors.active[0].ai == 63 && session.sim_actors.active[1].ai == 55);
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, NULL, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
        RegionalSessionTest_Equal(&loaded, &session));
  CHECK(ArRegionalSimActors_LoadTown(&loaded.sim_actors, 1));
  CHECK(ArRegionalSession_BeginSimActor(&loaded, 1, 0) && !loaded.sim_actors.active[0].combat);
  CHECK(loaded.sim_actors.active[0].ai == 55);
  CHECK(ArRegionalSimActors_LoadTown(&loaded.sim_actors, 0) &&
        loaded.sim_actors.active[0].combat == 31);
  CHECK(loaded.sim_actors.active[0].ai == 63);
  CHECK(ArRegionalSession_BeginSimActor(&loaded, 0, 0) && !loaded.sim_actors.active[0].combat);
  CHECK(ArRegionalSession_Save(&loaded, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&session, 0, path, image, &error) == kSaveCheckpoint_Ready &&
        RegionalSessionTest_Equal(&loaded, &session));
  CHECK(session.sim_actors.cached[0].combat == 31 &&
        session.sim_actors.active[0].combat == 0); /* old cache, new generation */
  session.revision = UINT32_MAX;
  policy.source[0] = 1;
  CHECK(!ArRegionalSession_RequestSimCombat(&session, session.revision, &policy));
  ai.source[0] = 0;
  CHECK(!ArRegionalSession_RequestSimAi(&session, session.revision, &ai));
  session.requested.sim_combat = policy;
  const ArRegionalSession frozen = session;
  CHECK(!ArRegionalSession_BeginSimActor(&session, 0, 0) &&
        RegionalSessionTest_Equal(&frozen, &session));
  remove(path);
  remove("regional-sim-combat.srm.archeckpoint");
}

static void CheckLevelGoals(void) {
  const uint8_t id[16] = {0x4c};
  ArRegionalSession session, loaded;
  ArRegionalCostPolicy costs;
  CHECK(ArRegionalCosts_Init(&costs, 0));
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  const char *path = "regional-level-codec.srm";
  remove(path);
  remove("regional-level-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 9);
  SaveError error;
  for (unsigned source = 0; source < 3; ++source) {
    CHECK(
        ArRegionalSession_RequestLevelGoals(&session, session.revision, (ArRegionalSource)source));
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_RequestLevelGoals(&session, revision, (ArRegionalSource)source) &&
          session.revision == revision);
    CHECK(!ArRegionalSession_RequestLevelGoals(&session, revision - 1, (ArRegionalSource)source));
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, source ? image : NULL,
                                 image, &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&loaded, &session));
    bool jp;
    CHECK(ArRegionalSession_BeginLevelGoals(&session, &jp) && jp == (source == 1));
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&loaded, &session));
  }
  const ArRegionalSession before = session;
  CHECK(!ArRegionalSession_RequestLevelGoals(&session, session.revision, 3) &&
        RegionalSessionTest_Equal(&session, &before));
  session.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_RequestLevelGoals(&session, session.revision, 1));
  session.requested.level_goals = 1;
  bool sentinel = false;
  CHECK(!ArRegionalSession_BeginLevelGoals(&session, &sentinel) && !sentinel);
  remove(path);
  remove("regional-level-codec.srm.archeckpoint");
}

static void CheckTownStatus(void) {
  const uint8_t id[16] = {0x48};
  ArRegionalSession session, loaded;
  ArRegionalCostPolicy costs;
  CHECK(ArRegionalCosts_Init(&costs, kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  const char *path = "regional-status-codec.srm";
  remove(path);
  remove("regional-status-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 9);
  SaveError error;
  ArRegionalTownStatusPolicy policy;
  for (unsigned combination = 0; combination < 243; ++combination) {
    unsigned digits = combination;
    for (unsigned i = 0; i < kArRegionalTownStatus_Count; ++i) {
      policy.source[i] = (ArRegionalSource)(digits % 3);
      digits /= 3;
    }
    CHECK(ArRegionalSession_RequestTownStatus(&session, session.revision, &policy));
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_RequestTownStatus(&session, revision, &policy) &&
          session.revision == revision);
    CHECK(!ArRegionalSession_RequestTownStatus(&session, revision - 1, &policy));
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path,
                                 combination ? image : NULL, image, &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&session, &loaded));
    ArRegionalTownStatusSnapshot snapshot;
    CHECK(ArRegionalSession_BeginTownStatus(&session, &snapshot));
    for (unsigned i = 0; i < kArRegionalTownStatus_Count; ++i)
      CHECK(snapshot.japanese[i] == (policy.source[i] == 1));
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&session, &loaded));
  }
  const ArRegionalSession before = session;
  policy.source[1] = kArRegionalSource_Count;
  CHECK(!ArRegionalSession_RequestTownStatus(&session, session.revision, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
  ArRegionalTownStatus_Init(&policy, kArRegionalSource_Japan);
  session.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_RequestTownStatus(&session, session.revision, &policy));
  session.requested.town_status = policy;
  ArRegionalTownStatusSnapshot sentinel = {{7, 7, 7, 7, 7}}, unchanged = sentinel;
  CHECK(!ArRegionalSession_BeginTownStatus(&session, &sentinel) &&
        !memcmp(&sentinel, &unchanged, sizeof(sentinel)));
  remove(path);
  remove("regional-status-codec.srm.archeckpoint");
}

static void CheckLairReloads(void) {
  const uint8_t id[16] = {0x47};
  ArRegionalSession session, loaded;
  ArRegionalCostPolicy costs;
  CHECK(ArRegionalCosts_Init(&costs, kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  CHECK(!ArRegionalSession_RequestLairReloads(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalLairReloads_Init(&session.reloads));
  CHECK(ArRegionalLairReloads_ReduceTown(&session.reloads, 3));
  session.reloads.approximate_towns = 2;
  const char *path = "regional-reloads-codec.srm";
  remove(path);
  remove("regional-reloads-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 4);
  SaveError error;
  for (unsigned i = 0; i < 3; ++i) {
    const ArRegionalSource source = (ArRegionalSource)((i + 1) % 3);
    CHECK(ArRegionalSession_RequestLairReloads(&session, session.revision, source));
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_RequestLairReloads(&session, revision, source) &&
          session.revision == revision);
    CHECK(!ArRegionalSession_RequestLairReloads(&session, revision - 1, source));
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, i ? image : NULL, image,
                                 &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&loaded, &session));
    const ArRegionalLairReloads before = session.reloads;
    CHECK(ArRegionalSession_BeginLairReloads(&session));
    CHECK(session.effective.lair_reloads == source &&
          !memcmp(&before, &session.reloads, sizeof(before)));
  }
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_RequestLairReloads(&session, session.revision, kArRegionalSource_Count));
  CHECK(RegionalSessionTest_Equal(&session, &before));
  session.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_RequestLairReloads(&session, session.revision, kArRegionalSource_Japan));
  session.requested.lair_reloads = kArRegionalSource_Japan;
  CHECK(!ArRegionalSession_BeginLairReloads(&session));
  session = before;
  session.reloads.diverged_towns = 4;
  CHECK(!ArRegionalSession_RequestLairReloads(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
        RegionalSessionTest_Equal(&loaded, &session));
  remove(path);
  remove("regional-reloads-codec.srm.archeckpoint");
}

static void CheckScoreFeedback(void) {
  const uint8_t id[16] = {0x73};
  ArRegionalSession session, loaded;
  ArRegionalCostPolicy costs;
  CHECK(ArRegionalCosts_Init(&costs, kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  ArRegionalScorePolicy policy;
  CHECK(ArRegionalScore_Init(&policy, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestScoreFeedback(&session, session.revision, &policy));
  for (unsigned town = 0; town < 6; ++town)
    CHECK(ArRegionalLairHistory_InitTown(&session.lairs, town));
  const char *path = "regional-score-codec.srm";
  remove(path);
  remove("regional-score-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 9);
  SaveError error;
  for (unsigned combination = 0; combination < 81; ++combination) {
    unsigned digits = combination, expected = 0;
    for (unsigned i = 0; i < kArRegionalScore_Count; ++i) {
      policy.source[i] = (ArRegionalSource)(digits % 3);
      digits /= 3;
      if (i < kArRegionalScore_Phase && policy.source[i] == kArRegionalSource_Japan)
        expected |= 4u << i;
    }
    CHECK(ArRegionalSession_RequestScoreFeedback(&session, session.revision, &policy));
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_RequestScoreFeedback(&session, revision, &policy) &&
          session.revision == revision);
    CHECK(!ArRegionalSession_RequestScoreFeedback(&session, revision - 1, &policy));
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path,
                                 combination ? image : NULL, image, &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&loaded, &session));
    ArRegionalLairAccounting snapshot;
    unsigned projection = 99;
    CHECK(ArRegionalSession_BeginLairAccounting(&session, &snapshot));
    CHECK(ArRegionalLairAccounting_Projection(&snapshot, &projection) && projection == expected);
    ArRegionalScoreSnapshot completion;
    const ArRegionalLairHistory history = session.lairs;
    CHECK(ArRegionalSession_BeginScoreCompletion(&session, &completion));
    CHECK(completion.japanese[kArRegionalScore_Phase] ==
          (policy.source[kArRegionalScore_Phase] == kArRegionalSource_Japan));
    CHECK(!memcmp(&session.lairs, &history, sizeof(history)));
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready &&
          RegionalSessionTest_Equal(&loaded, &session));
  }
  const ArRegionalSession before = session;
  policy.source[2] = kArRegionalSource_Count;
  CHECK(!ArRegionalSession_RequestScoreFeedback(&session, session.revision, &policy) &&
        RegionalSessionTest_Equal(&session, &before));
  ArRegionalScore_Init(&policy, kArRegionalSource_Japan);
  session.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_RequestScoreFeedback(&session, session.revision, &policy));
  session = before;
  CHECK(ArRegionalLairHistory_MarkDiverged(&session.lairs, 0));
  CHECK(!ArRegionalSession_RequestScoreFeedback(&session, session.revision, &policy));
  remove(path);
  remove("regional-score-codec.srm.archeckpoint");
}

static void CheckLairSeeds(void) {
  const uint8_t id[16] = {71};
  ArRegionalCostPolicy costs;
  CHECK(ArRegionalCosts_Init(&costs, kArRegionalSource_US));
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  CHECK(!ArRegionalSession_RequestLairSeeds(&session, session.revision, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestHouseCredit(&session, session.revision, kArRegionalSource_Japan));
  for (unsigned town = 0; town < 6; ++town)
    CHECK(ArRegionalLairHistory_InitTown(&session.lairs, town));
  CHECK(ArRegionalSession_RequestLairSeeds(&session, session.revision, kArRegionalSource_Japan));
  CHECK(session.effective.lair_seeds == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestLairSeeds(&session, session.revision - 1, kArRegionalSource_US));
  ArRegionalLairAccounting snapshot;
  CHECK(ArRegionalSession_BeginLairAccounting(&session, &snapshot) &&
        snapshot.seeds == kArRegionalSource_Japan);
  const uint32_t revision = session.revision;
  CHECK(ArRegionalSession_BeginLairAccounting(&session, &snapshot) && revision == session.revision);
  CHECK(ArRegionalSession_RequestHouseCredit(&session, session.revision, kArRegionalSource_Japan));
  CHECK(session.effective.house_credit == kArRegionalSource_US);
  const char *path = "regional-seed-codec.srm";
  remove(path);
  remove("regional-seed-codec.srm.archeckpoint");
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 7);
  SaveError error;
  ArRegionalSession loaded;
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, NULL, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&session, &loaded));
  CHECK(
      !ArRegionalSession_RequestHouseCredit(&session, session.revision - 1, kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginLairAccounting(&session, &snapshot) &&
        snapshot.house_credit == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_RequestLairSeeds(&session, session.revision, kArRegionalSource_Europe));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&session, &loaded) &&
        loaded.effective.lair_seeds == kArRegionalSource_Japan);
  CHECK(ArRegionalLairHistory_MarkDiverged(&session.lairs, 1));
  CHECK(!ArRegionalSession_BeginLairAccounting(&session, &snapshot));
  CHECK(!ArRegionalSession_RequestHouseCredit(&session, session.revision, kArRegionalSource_US));
  CHECK(!ArRegionalSession_RequestLairSeeds(&session, session.revision, kArRegionalSource_US));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(
      &session, &loaded)); /* Quarantine retains both requested/effective choices. */
  remove(path);
  remove("regional-seed-codec.srm.archeckpoint");
}

static void CheckQuakeActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {42};
  ArRegionalCostPolicy costs;
  ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_RequestQuake(&session, 2, kArRegionalSource_Japan) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestQuake(&session, 1, kArRegionalSource_Count) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(ArRegionalSession_RequestQuake(&session, 1, kArRegionalSource_Japan));
  CHECK(session.effective.quake.source[0] == kArRegionalSource_US);
  ArRegionalQuakeSnapshot snapshot;
  CHECK(ArRegionalSession_BeginQuake(&session, &snapshot));
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i)
    CHECK(snapshot.random[i]);
  before = session;
  CHECK(ArRegionalSession_BeginQuake(&session, &snapshot) &&
        RegionalSessionTest_Equal(&before, &session));
  session.requested.quake.source[0] = kArRegionalSource_Europe;
  CHECK(ArRegionalSession_BeginQuake(&session, &snapshot) && !snapshot.random[0] &&
        snapshot.random[1]);
  ArRegionalSource source = kArRegionalSource_Count;
  CHECK(!ArRegionalQuake_GroupSource(&session.effective.quake, &source) &&
        source == kArRegionalSource_Count);
  CHECK(ArRegionalSession_RequestQuake(&session, session.revision, kArRegionalSource_US));
  session.revision = UINT32_MAX;
  before = session;
  CHECK(!ArRegionalSession_BeginQuake(&session, &snapshot) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestQuake(&session, UINT32_MAX, kArRegionalSource_Japan) &&
        RegionalSessionTest_Equal(&before, &session));
}

static void CheckScorePageActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {43};
  ArRegionalCostPolicy costs;
  ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  bool enabled = false;
  CHECK(ArRegionalSession_BeginScorePage(&session, &enabled) && enabled && session.revision == 1);
  CHECK(!ArRegionalSession_RequestScorePage(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestScorePage(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestScorePage(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginScorePage(&session, &enabled) &&
          enabled == (source != kArRegionalSource_Japan));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginScorePage(&session, &enabled) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestScorePage(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX;
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginScorePage(&session, &enabled) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestScorePage(&session, UINT32_MAX, kArRegionalSource_US) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalScorePage_Resolve(kArRegionalSource_Count, &enabled) && enabled);
  CHECK(!ArRegionalScorePage_Resolve(kArRegionalSource_US, NULL));
}

static void CheckLivesDisplayActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {44};
  ArRegionalCostPolicy costs;
  ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  bool zero_based = true;
  CHECK(ArRegionalSession_BeginLivesDisplay(&session, &zero_based) && !zero_based &&
        session.revision == 1);
  CHECK(!ArRegionalSession_RequestLivesDisplay(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestLivesDisplay(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestLivesDisplay(&session, session.revision,
                                                (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginLivesDisplay(&session, &zero_based) &&
          zero_based == (source == kArRegionalSource_Japan));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginLivesDisplay(&session, &zero_based) &&
          session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestLivesDisplay(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX;
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginLivesDisplay(&session, &zero_based) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestLivesDisplay(&session, UINT32_MAX, kArRegionalSource_US) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalLivesDisplay_Resolve(kArRegionalSource_Count, &zero_based) && !zero_based);
  CHECK(!ArRegionalLivesDisplay_Resolve(kArRegionalSource_US, NULL));
}

static void CheckSourcesActivation(void) {
  ArRegionalSession session;
  ArRegionalCostPolicy costs;
  ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  const uint8_t id[16] = {45};
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  ArRegionalSourcesSnapshot snapshot;
  for (unsigned life = 0; life < 3; ++life)
    for (unsigned magic = 0; magic < 3; ++magic) {
      ArRegionalSourcesPolicy policy = {{life, magic}};
      const ArRegionalSession before = session;
      CHECK(!ArRegionalSession_RequestSources(&session, session.revision + 1, &policy));
      CHECK(RegionalSessionTest_Equal(&before, &session));
      CHECK(ArRegionalSession_RequestSources(&session, session.revision, &policy));
      CHECK(!memcmp(&before.effective, &session.effective, sizeof(session.effective)));
      CHECK(ArRegionalSession_BeginSources(&session, &snapshot));
      CHECK(snapshot.automatic[0] == (life != 1) && snapshot.automatic[1] == (magic != 1));
      const uint32_t revision = session.revision;
      CHECK(ArRegionalSession_BeginSources(&session, &snapshot) && session.revision == revision);
    }
  ArRegionalSourcesPolicy policy = {{1, 0}};
  CHECK(ArRegionalSession_RequestSources(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginSources(&session, &snapshot) &&
        RegionalSessionTest_Equal(&before, &session));
  policy.source[0] = 0;
  CHECK(!ArRegionalSession_RequestSources(&session, UINT32_MAX, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestSources(&session, UINT32_MAX, NULL));
}

static void CheckSkullWait(void) {
  ArRegionalSession session;
  ArRegionalCostPolicy costs;
  ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  const uint8_t id[16] = {46};
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  uint16_t frames = 123;
  CHECK(!strcmp(ArRegionalSkullWait_Descriptor()->key, "skull_post_effect_frames"));
  CHECK(!ArRegionalSkullWait_Resolve(kArRegionalSource_Count, &frames) && frames == 123);
  CHECK(!ArRegionalSkullWait_Resolve(kArRegionalSource_US, NULL));
  CHECK(!ArRegionalSession_RequestSkullWait(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestSkullWait(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < 3; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestSkullWait(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginSkullWait(&session, &frames) && frames == (source == 1 ? 0 : 90));
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_BeginSkullWait(&session, &frames) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestSkullWait(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginSkullWait(&session, &frames) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestSkullWait(&session, UINT32_MAX, kArRegionalSource_US) &&
        RegionalSessionTest_Equal(&before, &session));
}

static void CheckStory(void) {
  ArRegionalSession session;
  ArRegionalCostPolicy costs;
  ArRegionalCosts_Init(&costs, 0);
  const uint8_t id[16] = {47};
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  ArRegionalStorySnapshot snapshot;
  for (unsigned n = 0; n < 27; ++n) {
    ArRegionalStoryPolicy policy;
    unsigned digits = n;
    for (unsigned i = 0; i < 3; ++i) {
      policy.source[i] = (ArRegionalSource)(digits % 3);
      digits /= 3;
    }
    const ArRegionalSession before = session;
    CHECK(!ArRegionalSession_RequestStory(&session, session.revision + 1, &policy) &&
          RegionalSessionTest_Equal(&before, &session));
    CHECK(ArRegionalSession_RequestStory(&session, session.revision, &policy));
    CHECK(!memcmp(&before.effective, &session.effective, sizeof(session.effective)));
    CHECK(ArRegionalSession_BeginStory(&session, &snapshot));
    for (unsigned i = 0; i < 3; ++i)
      CHECK(snapshot.value[i] ==
            ArRegionalStory_Descriptor((ArRegionalStoryRule)i)->value[policy.source[i]]);
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_BeginStory(&session, &snapshot) && session.revision == revision);
  }
  ArRegionalStoryPolicy policy = {{1, 0, 2}};
  CHECK(ArRegionalSession_RequestStory(&session, session.revision, &policy));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginStory(&session, &snapshot) &&
        RegionalSessionTest_Equal(&before, &session));
  policy.source[0] = 0;
  CHECK(!ArRegionalSession_RequestStory(&session, UINT32_MAX, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
  policy.source[0] = 3;
  CHECK(!ArRegionalSession_RequestStory(&session, UINT32_MAX, &policy) &&
        RegionalSessionTest_Equal(&before, &session));
}

static void CheckMenuReturnActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {43};
  ArRegionalCostPolicy costs;
  ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  bool enabled = false;
  CHECK(ArRegionalSession_BeginMenuReturn(&session, &enabled) && !enabled && session.revision == 1);
  CHECK(!ArRegionalSession_RequestMenuReturn(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestMenuReturn(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(
        ArRegionalSession_RequestMenuReturn(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginMenuReturn(&session, &enabled) &&
          enabled == (source == kArRegionalSource_Japan));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginMenuReturn(&session, &enabled) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestMenuReturn(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX;
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginMenuReturn(&session, &enabled) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestMenuReturn(&session, UINT32_MAX, kArRegionalSource_US) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalMenuReturn_Resolve(kArRegionalSource_Count, &enabled) && !enabled);
  CHECK(!ArRegionalMenuReturn_Resolve(kArRegionalSource_US, NULL));
}

static void CheckMagicGestureActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {42};
  ArRegionalCostPolicy costs;
  ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  bool gesture = true;
  CHECK(ArRegionalSession_BeginMagicGesture(&session, false, &gesture) && !gesture &&
        session.revision == 1);
  CHECK(!ArRegionalSession_RequestMagicGesture(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestMagicGesture(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const bool previous = gesture;
    CHECK(ArRegionalSession_RequestMagicGesture(&session, session.revision,
                                                (ArRegionalSource)source));
    const ArRegionalSession pending = session;
    CHECK(ArRegionalSession_BeginMagicGesture(&session, false, &gesture) && gesture == previous &&
          RegionalSessionTest_Equal(&session, &pending));
    CHECK(ArRegionalSession_BeginMagicGesture(&session, true, &gesture) &&
          gesture == (source == kArRegionalSource_Japan));
    const uint32_t revision = session.revision;
    CHECK(ArRegionalSession_BeginMagicGesture(&session, true, &gesture) &&
          session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestMagicGesture(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX;
  const ArRegionalSession before = session;
  CHECK(ArRegionalSession_BeginMagicGesture(&session, false, &gesture) && !gesture &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_BeginMagicGesture(&session, true, &gesture) && !gesture &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestMagicGesture(&session, UINT32_MAX, kArRegionalSource_US));
  CHECK(!ArRegionalMagicGesture_Resolve(kArRegionalSource_Count, &gesture) && !gesture);
  CHECK(!ArRegionalMagicGesture_Resolve(kArRegionalSource_US, NULL));
}

static void CheckSpeedRangeActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {43};
  ArRegionalCostPolicy costs;
  ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  uint16_t enabled = 9;
  CHECK(ArRegionalSession_BeginSpeedRange(&session, &enabled) && enabled == 9 &&
        session.revision == 1);
  CHECK(!ArRegionalSession_RequestSpeedRange(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestSpeedRange(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(
        ArRegionalSession_RequestSpeedRange(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginSpeedRange(&session, &enabled) &&
          enabled == (source == kArRegionalSource_Japan ? 7 : 9));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginSpeedRange(&session, &enabled) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestSpeedRange(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX;
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginSpeedRange(&session, &enabled) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestSpeedRange(&session, UINT32_MAX, kArRegionalSource_US) &&
        RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSpeedRange_Resolve(kArRegionalSource_Count, &enabled) && enabled == 9);
  CHECK(!ArRegionalSpeedRange_Resolve(kArRegionalSource_US, NULL));
}

static void CheckSessionState(void) {
  const uint8_t id[16] = {1}, zero[16] = {0};
  ArRegionalCostPolicy defaults;
  CHECK(ArRegionalCosts_Init(&defaults, kArRegionalSource_US));
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_NewGame(&session, 7, zero, &defaults));
  CHECK(RegionalSessionTest_Equal(&before, &session));
  CHECK(ArRegionalSession_RequestCosts(&session, 1, kArRegionalCostGroup_Miracles,
                                       kArRegionalSource_Japan));
  CHECK(session.revision == 2 &&
        session.effective.costs.source[kArRegionalCost_Rain] == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestCosts(&session, 1, kArRegionalCostGroup_Miracles,
                                        kArRegionalSource_US));
  CHECK(session.revision == 2);
  ArRegionalCostSnapshot quote;
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Scrolls, &quote));
  CHECK(session.revision == 2 && quote.price[kArRegionalCost_Rain] == 20);
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Miracles, &quote));
  CHECK(session.revision == 3 && quote.price[kArRegionalCost_Rain] == 16);
  CHECK(ArRegionalSession_RequestCosts(&session, 3, kArRegionalCostGroup_Miracles,
                                       kArRegionalSource_US));
  CHECK(quote.price[kArRegionalCost_Rain] == 16); /* An accepted quote stays frozen. */
  CHECK(session.effective.costs.source[kArRegionalCost_Rain] == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Miracles, &quote));
  CHECK(quote.price[kArRegionalCost_Rain] == 20 && session.revision == 5);
  session.revision = UINT32_MAX;
  before = session;
  CHECK(!ArRegionalSession_RequestCosts(&session, UINT32_MAX, kArRegionalCostGroup_Scrolls,
                                        kArRegionalSource_Japan));
  CHECK(RegionalSessionTest_Equal(&before, &session));
  CHECK(ArRegionalSession_RequestCosts(&session, UINT32_MAX, kArRegionalCostGroup_Scrolls,
                                       kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Scrolls, &quote));
  CHECK(RegionalSessionTest_Equal(&before, &session));
  session.requested.costs.source[kArRegionalCost_Light] = kArRegionalSource_Japan;
  before = session;
  ArRegionalCostSnapshot old_quote = quote;
  CHECK(!ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Scrolls, &quote));
  CHECK(RegionalSessionTest_Equal(&before, &session));
  CHECK(!memcmp(&quote, &old_quote, sizeof(quote)));

  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  before = session;
  CHECK(ArRegionalSession_RequestTimers(&session, 1, kArRegionalSource_Japan));
  CHECK(session.revision == 2);
  CHECK(!memcmp(&before.requested.costs, &session.requested.costs, sizeof(before.requested.costs)));
  CHECK(session.effective.timers.source[0] == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestTimers(&session, 1, kArRegionalSource_US));
  ArRegionalTimerPolicy snapshot;
  CHECK(ArRegionalSession_BeginTimers(&session, &snapshot));
  CHECK(session.revision == 3 && snapshot.source[0] == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_RequestTimers(&session, 3, kArRegionalSource_Europe));
  CHECK(snapshot.source[0] == kArRegionalSource_Japan);
  before = session;
  session.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginTimers(&session, &snapshot));
  CHECK(snapshot.source[0] == kArRegionalSource_Japan);
  CHECK(!memcmp(&session.effective.timers, &before.effective.timers, sizeof(snapshot)));
  CHECK(!ArRegionalSession_RequestTimers(&session, UINT32_MAX, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  bool clear = false;
  CHECK(ArRegionalSession_RequestRetryScore(&session, session.revision, kArRegionalSource_Japan));
  CHECK(session.effective.retry_score == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestRetryScore(&session, 1, kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginRetryScore(&session, &clear) && clear);
  CHECK(session.effective.retry_score == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_RequestRetryScore(&session, session.revision, kArRegionalSource_Europe));
  before = session;
  session.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginRetryScore(&session, &clear) && clear);
  CHECK(session.effective.retry_score == before.effective.retry_score);
  CHECK(!ArRegionalSession_RequestRetryScore(&session, UINT32_MAX, kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  uint16_t wait = 999;
  CHECK(!ArRegionalSession_RequestTownWait(&session, session.revision, kArRegionalSource_Count));
  CHECK(ArRegionalSession_RequestTownWait(&session, session.revision, kArRegionalSource_Japan));
  CHECK(session.effective.town_wait == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestTownWait(&session, 1, kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginTownWait(&session, &wait) && wait == 150);
  CHECK(session.effective.town_wait == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_RequestTownWait(&session, session.revision, kArRegionalSource_Europe));
  session.revision = UINT32_MAX;
  before = session;
  CHECK(!ArRegionalSession_BeginTownWait(&session, &wait) && wait == 150);
  CHECK(RegionalSessionTest_Equal(&before, &session));
  CHECK(ArRegionalSession_RequestTownWait(&session, UINT32_MAX, kArRegionalSource_Europe));
  CHECK(RegionalSessionTest_Equal(&before, &session));
  CHECK(!ArRegionalSession_RequestTownWait(&session, UINT32_MAX, kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  bool reconcile = true;
  CHECK(ArRegionalSession_RequestFishing(&session, session.revision, kArRegionalSource_Europe));
  CHECK(ArRegionalSession_BeginFishing(&session, &wait, &reconcile) && wait == 255 && !reconcile);
  CHECK(ArRegionalSession_RequestFishing(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_BeginFishing(&session, &wait, &reconcile) && wait == 128 && reconcile);
  CHECK(ArRegionalSession_BeginFishing(&session, &wait, &reconcile) && wait == 128 && !reconcile);
  CHECK(ArRegionalSession_RequestFishing(&session, session.revision, kArRegionalSource_US));
  session.revision = UINT32_MAX;
  before = session;
  CHECK(!ArRegionalSession_BeginFishing(&session, &wait, &reconcile) && wait == 128 && !reconcile);
  CHECK(RegionalSessionTest_Equal(&session, &before));
  CHECK(!ArRegionalSession_RequestFishing(&session, UINT32_MAX, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestFishing(&session, UINT32_MAX, kArRegionalSource_Count));
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  ArRegionalDevelopmentSnapshot development = {0};
  CHECK(ArRegionalSession_RequestDevelopment(&session, 1, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestDevelopment(&session, 1, kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginDevelopment(&session, &development) &&
        development.service_divider == 5 && development.long_cycle == 480);
  CHECK(ArRegionalSession_RequestDevelopment(&session, session.revision, kArRegionalSource_US));
  session.revision = UINT32_MAX;
  before = session;
  CHECK(!ArRegionalSession_BeginDevelopment(&session, &development) &&
        development.long_cycle == 480);
  CHECK(RegionalSessionTest_Equal(&session, &before));
  CHECK(!ArRegionalSession_RequestDevelopment(&session, UINT32_MAX, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  ArRegionalRecoverySnapshot recovery = {0};
  unsigned changed = 99;
  CHECK(ArRegionalSession_RequestRecovery(&session, 1, kArRegionalSource_Europe));
  CHECK(ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && !changed);
  CHECK(recovery.cycle_sp && !recovery.angel_calls);
  CHECK(ArRegionalSession_RequestRecovery(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && changed == 3);
  CHECK(!recovery.cycle_sp && recovery.angel_calls == 60);
  CHECK(ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && !changed);
  ArRegionalRecoveryPolicy partial = session.requested.recovery;
  partial.source[kArRegionalRecovery_SP] = kArRegionalSource_US;
  before = session;
  CHECK(!ArRegionalSession_RequestRecoveryPolicy(&session, session.revision, NULL));
  CHECK(!ArRegionalSession_RequestRecoveryPolicy(&session, session.revision - 1, &partial));
  CHECK(RegionalSessionTest_Equal(&session, &before));
  CHECK(ArRegionalSession_RequestRecoveryPolicy(&session, session.revision, &partial));
  CHECK(ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && changed == 1);
  CHECK(recovery.cycle_sp && recovery.angel_calls == 60);
  before = session;
  partial.source[kArRegionalRecovery_Angel] = kArRegionalSource_Count;
  CHECK(!ArRegionalSession_RequestRecoveryPolicy(&session, session.revision, &partial));
  CHECK(RegionalSessionTest_Equal(&session, &before));
  CHECK(ArRegionalSession_RequestRecovery(&session, session.revision, kArRegionalSource_US));
  session.revision = UINT32_MAX;
  before = session;
  CHECK(!ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && changed == 1);
  CHECK(RegionalSessionTest_Equal(&session, &before));
}

int RegionalSessionTest_RunActivation(void) {
  s_failures = 0;
  CheckActorArtwork();
  CheckFire();
  CheckCastHold();
  CheckBosses();
  CheckCollision();
  CheckPlatformSkull();
  CheckActorStats();
  CheckVolley();
  CheckActionMotion();
  CheckEmitters();
  CheckArrival();
  CheckPopulation();
  CheckConstruction();
  CheckSimCombat();
  CheckLevelGoals();
  CheckTownStatus();
  CheckLairReloads();
  CheckScoreFeedback();
  CheckLairSeeds();
  CheckMagicGestureActivation();
  CheckScorePageActivation();
  CheckLivesDisplayActivation();
  CheckSourcesActivation();
  CheckSkullWait();
  CheckStory();
  CheckMenuReturnActivation();
  CheckSpeedRangeActivation();
  CheckQuakeActivation();
  CheckSessionState();
  return s_failures;
}
