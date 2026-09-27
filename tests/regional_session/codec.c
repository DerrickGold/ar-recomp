#include "support/regional_test_values.h"
/* Frozen payloads, schema compatibility and malformed-payload rejection. */
#include "regional_session_test.h"
#include "support/test_check.h"

#include <string.h>
#include "regional/regional_profiles.h"
#include "byte_order.h"

static int s_failures;
#define CHECK(condition) AR_TEST_CHECK(s_failures, condition)

static void CheckFeatureCodec(void) {
  const char *path = "actraiser-regional-codec-test.srm";
  const char *companion = "actraiser-regional-codec-test.srm.archeckpoint";
  remove(path);
  remove(companion);
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 7);
  const uint8_t id[16] = {7};
  ArRegionalCostPolicy defaults;
  CHECK(ArRegionalCosts_Init(&defaults, kArRegionalSource_US));
  ArRegionalSession session, loaded;
  CHECK(ArRegionalSession_NewGame(&session, 2, id, &defaults));
  CHECK(ArRegionalLairHistory_InitTown(&session.lairs, 2));
  CHECK(ArRegionalLairHistory_HouseLost(&session.lairs, 2, 0x20, 1));
  CHECK(ArRegionalLairHistory_MarkDiverged(&session.lairs, 2));
  CHECK(ArRegionalLairHistory_AdoptTown(&session.lairs, 5, kArRegionalSource_US,
                                        (uint16_t[4]){0, 1, 301, 65535}));
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Scrolls,
                                       kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTimers(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestRetryScore(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTownWait(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestFishing(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestDevelopment(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestRecovery(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestQuake(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestScorePage(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestMenuReturn(&session, session.revision, kArRegionalSource_Japan));
  SaveError error;
  CHECK(ArRegionalSession_RequestSpeedRange(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestMagicGesture(&session, session.revision, kArRegionalSource_Japan));
  ArRegionalActionMotionPolicy initial_motion = {{1, 2, 0, 1, 0, 2, 1, 1, 2, 1, 1, 2}};
  uint16_t initial_snapshot;
  CHECK(ArRegionalSession_RequestActionMotion(&session, session.revision, &initial_motion));
  CHECK(ArRegionalSession_BeginActionMotion(&session, &initial_snapshot));
  initial_motion.source[0] = 0;
  initial_motion.source[8] = 1;
  CHECK(ArRegionalSession_RequestActionMotion(&session, session.revision, &initial_motion));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, NULL, image, &error));
  uint8_t original[kSaveCheckpointPayloadMax], mutated[kSaveCheckpointPayloadMax];
  size_t size = 0;
  CHECK(SaveCheckpoint_Read(path, image, original, sizeof(original), &size, &error) ==
        kSaveCheckpoint_Ready);
  printf("regional codec: version %u, %u records, %zu bytes\n", ByteOrder_ReadLe16(original + 8),
         ByteOrder_ReadLe16(original + 10), size);
  if (size < 36 || size >= sizeof(original)) return;
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    memcpy(mutated, original, size);
    size_t bytes = size;
    SaveCheckpointStatus expected = kSaveCheckpoint_Invalid;
    switch (mutation) {
    case 0:
      mutated[0] ^= 1;
      break; /* Wrong codec magic. */
    case 1:
      mutated[8] = 255;
      expected = kSaveCheckpoint_Unsupported;
      break;
    case 2:
      memset(mutated + 16, 0, 16);
      break; /* Missing campaign identity. */
    case 3:
      memset(mutated + 32, 0, 4);
      break; /* Invalid generation. */
    case 4:
      mutated[37 + mutated[36]] = 'x';
      expected = kSaveCheckpoint_Unsupported;
      break;
    case 5:
      mutated[37 + mutated[36] + 4] ^= 1;
      expected = kSaveCheckpoint_Unsupported;
      break;
    case 6:
      --bytes;
      break;
    case 7:
      mutated[bytes++] = 0;
      break;
    case 8: { /* Duplicate the first named leaf in place of the second. */
      size_t first = original[36] + 9u, second = original[36 + first] + 9u;
      memcpy(mutated + 36 + first, original + 36, first);
      memcpy(mutated + 36 + first * 2, original + 36 + first + second, size - 36 - first - second);
      bytes = size + first - second;
      break;
    }
    }
    CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, bytes,
                                RegionalSessionTest_AcceptOpaque, NULL, &error));
    loaded = session;
    CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == expected);
    CHECK(RegionalSessionTest_Equal(&loaded, &session));
    CHECK(!ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  }
  /* Reordering valid named fields is supported; they are not enum ordinals. */
  enum {
    records = kArRegionalCostRule_Count + kArRegionalTimerRule_Count + 20 +
              kArRegionalDevelopmentRule_Count + kArRegionalRecovery_Count +
              kArRegionalQuake_Count + kArRegionalLairCount + kArRegionalScore_Count +
              kArRegionalSourceItem_Count + kArRegionalStory_Count + kArRegionalTownStatus_Count +
              kArRegionalSimCombat_Count + kArRegionalSimAi_Count + kArRegionalSupport_Count +
              kArRegionalActionMotion_Count + kArRegionalEmitter_Count + kArRegionalBoss_Count +
              kArRegionalCollision_Count + kArRegionalPlatformSkull_Count +
              kArRegionalActorStat_Count + kArRegionalCastHold_Count + kArRegionalFire_Count +
              kArRegionalDifficultyRule_Count + kArRegionalActionStart_Count +
              kArRegionalMode_Count + kArRegionalPlacement_Count + 1 + kArRegionalArtwork_Count +
              kArRegionalPose_Count + kArRegionalSequence_Count + kArRegionalActorArtwork_Count
  };
  CHECK(ByteOrder_ReadLe16(original + 10) == records);
  if (ByteOrder_ReadLe16(original + 10) != records)
    return; /* Do not cascade into invalid fixture offsets. */
  size_t offsets[records], offset = 36;
  for (unsigned i = 0; i < records; ++i) {
    offsets[i] = offset;
    offset += original[offset] + 9u;
  }
  const size_t rules_end = offset;
  CHECK(offset + kArRegionalLairHistoryEncodedBytes + kArRegionalLairReloadEncodedBytes +
            kArRegionalSimActorsEncodedBytes + 19 ==
        size);
  memcpy(mutated, original, 36);
  offset = 36;
  for (unsigned i = records; i-- > 0;) {
    size_t length = original[offsets[i]] + 9u;
    memcpy(mutated + offset, original + offsets[i], length);
    offset += length;
  }
  memcpy(mutated + offset, original + rules_end, size - rules_end);
  offset += size - rules_end;
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, offset,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &session));

  /* Pricing-only checkpoints migrate in memory with US timer defaults, never
   * assuming that previous prices were a full regional preset. The old on-disk
   * record remains recoverable and can be retained beside a new save. */
  for (unsigned kind = 0; kind < 2; ++kind) {
    memcpy(mutated, original, size);
    const size_t seed_value = offsets[32] + 1 + original[offsets[32]];
    if (!kind)
      memcpy(mutated + seed_value, "eu", 2); /* Same values, inconsistent table-wide source. */
    else
      mutated[seed_value + 4] ^= 1; /* Table edits cannot silently change a retained policy. */
    CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, size,
                                RegionalSessionTest_AcceptOpaque, NULL, &error));
    loaded = session;
    CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) ==
          (kind ? kSaveCheckpoint_Unsupported : kSaveCheckpoint_Invalid));
    CHECK(RegionalSessionTest_Equal(&loaded, &session));
  }
  const size_t price_bytes = offsets[kArRegionalCostRule_Count];
  memcpy(mutated, original, price_bytes);
  memcpy(mutated, "ARPRICE", 8);
  ByteOrder_WriteLe16(mutated + 8, 1);
  ByteOrder_WriteLe16(mutated + 10, kArRegionalCostRule_Count);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, price_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded.requested.costs, &session.requested.costs, sizeof(loaded.requested.costs)));
  for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i)
    CHECK(loaded.requested.timers.source[i] == kArRegionalSource_US &&
          loaded.effective.timers.source[i] == kArRegionalSource_US);
  CHECK(loaded.requested.retry_score == kArRegionalSource_US);
  /* Timer-era v1 preserves timer choices; only the new retry rule defaults. */
  const size_t v1_bytes = offsets[15];
  memcpy(mutated, original, v1_bytes);
  ByteOrder_WriteLe16(mutated + 8, 1);
  ByteOrder_WriteLe16(mutated + 10, 15);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v1_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded.requested.timers, &session.requested.timers,
                sizeof(loaded.requested.timers)));
  CHECK(loaded.requested.retry_score == kArRegionalSource_US &&
        loaded.effective.retry_score == kArRegionalSource_US);
  CHECK(loaded.requested.town_wait == kArRegionalSource_US &&
        loaded.effective.town_wait == kArRegionalSource_US);
  /* Retry-era v2 retains its score rule but cannot imply town timing. */
  const size_t v2_bytes = offsets[16];
  memcpy(mutated, original, v2_bytes);
  ByteOrder_WriteLe16(mutated + 8, 2);
  ByteOrder_WriteLe16(mutated + 10, 16);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v2_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.retry_score == kArRegionalSource_Japan);
  CHECK(loaded.requested.town_wait == kArRegionalSource_US &&
        loaded.effective.town_wait == kArRegionalSource_US);
  const size_t v3_bytes = offsets[17];
  memcpy(mutated, original, v3_bytes);
  ByteOrder_WriteLe16(mutated + 8, 3);
  ByteOrder_WriteLe16(mutated + 10, 17);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v3_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.town_wait == kArRegionalSource_Japan);
  CHECK(loaded.requested.fishing == kArRegionalSource_US &&
        loaded.effective.fishing == kArRegionalSource_US);
  const size_t v4_bytes = offsets[18];
  memcpy(mutated, original, v4_bytes);
  ByteOrder_WriteLe16(mutated + 8, 4);
  ByteOrder_WriteLe16(mutated + 10, 18);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v4_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.fishing == kArRegionalSource_Japan);
  for (unsigned i = 0; i < kArRegionalDevelopmentRule_Count; ++i)
    CHECK(loaded.requested.development.source[i] == kArRegionalSource_US &&
          loaded.effective.development.source[i] == kArRegionalSource_US);
  const size_t v5_bytes = offsets[21];
  memcpy(mutated, original, v5_bytes);
  ByteOrder_WriteLe16(mutated + 8, 5);
  ByteOrder_WriteLe16(mutated + 10, 21);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v5_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.development.source[0] == kArRegionalSource_Japan);
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i)
    CHECK(loaded.requested.recovery.source[i] == kArRegionalSource_US &&
          loaded.effective.recovery.source[i] == kArRegionalSource_US);
  const size_t v6_bytes = offsets[23];
  memcpy(mutated, original, v6_bytes);
  ByteOrder_WriteLe16(mutated + 8, 6);
  ByteOrder_WriteLe16(mutated + 10, 23);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v6_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.recovery.source[0] == kArRegionalSource_Japan);
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i)
    CHECK(loaded.requested.quake.source[i] == kArRegionalSource_US &&
          loaded.effective.quake.source[i] == kArRegionalSource_US);
  const size_t v7_bytes = offsets[28];
  memcpy(mutated, original, v7_bytes);
  ByteOrder_WriteLe16(mutated + 8, 7);
  ByteOrder_WriteLe16(mutated + 10, 28);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v7_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.quake.source[0] == kArRegionalSource_Japan);
  CHECK(loaded.requested.score_page == kArRegionalSource_US &&
        loaded.effective.score_page == kArRegionalSource_US);
  const size_t v8_bytes = offsets[29];
  memcpy(mutated, original, v8_bytes);
  ByteOrder_WriteLe16(mutated + 8, 8);
  ByteOrder_WriteLe16(mutated + 10, 29);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v8_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.score_page == kArRegionalSource_Japan);
  CHECK(loaded.requested.menu_return == kArRegionalSource_US &&
        loaded.effective.menu_return == kArRegionalSource_US);
  const size_t v9_bytes = offsets[30];
  memcpy(mutated, original, v9_bytes);
  ByteOrder_WriteLe16(mutated + 8, 9);
  ByteOrder_WriteLe16(mutated + 10, 30);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v9_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.menu_return == kArRegionalSource_Japan);
  CHECK(loaded.requested.speed_range == kArRegionalSource_US &&
        loaded.effective.speed_range == kArRegionalSource_US);
  const size_t v10_bytes = offsets[31];
  memcpy(mutated, original, v10_bytes);
  ByteOrder_WriteLe16(mutated + 8, 10);
  ByteOrder_WriteLe16(mutated + 10, 31);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v10_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.speed_range == kArRegionalSource_Japan);
  CHECK(loaded.requested.magic_gesture == kArRegionalSource_US &&
        loaded.effective.magic_gesture == kArRegionalSource_US);
  const size_t v11_bytes = offsets[32];
  memcpy(mutated, original, v11_bytes);
  ByteOrder_WriteLe16(mutated + 8, 11);
  ByteOrder_WriteLe16(mutated + 10, 32);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, v11_bytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.magic_gesture == kArRegionalSource_Japan);
  CHECK(!loaded.lairs.initialized_towns && !loaded.lairs.approximate_towns);
  for (unsigned p = 0; p < kArRegionalLairProjections; ++p)
    for (unsigned n = 0; n < kArRegionalLairCount; ++n)
      CHECK(!loaded.lairs.stock[p][n]);
  /* v12 retains its canonical history but has no seed selector. */
  memcpy(mutated, original, v11_bytes);
  ByteOrder_WriteLe16(mutated + 8, 12);
  ByteOrder_WriteLe16(mutated + 10, 32);
  memcpy(mutated + v11_bytes, original + rules_end, kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v11_bytes + kArRegionalLairHistoryEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.lair_seeds == kArRegionalSource_US &&
        loaded.effective.lair_seeds == kArRegionalSource_US);
  CHECK(!memcmp(loaded.lairs.stock, session.lairs.stock, sizeof(session.lairs.stock)));
  const size_t v16_bytes = offsets[61];
  memcpy(mutated, original, v16_bytes);
  ByteOrder_WriteLe16(mutated + 8, 16);
  ByteOrder_WriteLe16(mutated + 10, 61);
  memcpy(mutated + v16_bytes, original + rules_end, kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v16_bytes + kArRegionalLairHistoryEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.lives_display == kArRegionalSource_US &&
        loaded.effective.lives_display == kArRegionalSource_US);
  CHECK(!memcmp(&loaded.requested.score_feedback, &session.requested.score_feedback,
                sizeof(session.requested.score_feedback)));
  CHECK(TestRegional_EqualLairs(&loaded.lairs, &session.lairs));
  CHECK(ArRegionalSession_RequestLivesDisplay(&session, session.revision, kArRegionalSource_Japan));
  /* v13 retains seed policy/history and defaults only the new house rule. */
  const size_t v13_bytes = offsets[56];
  memcpy(mutated, original, v13_bytes);
  ByteOrder_WriteLe16(mutated + 8, 13);
  ByteOrder_WriteLe16(mutated + 10, 56);
  memcpy(mutated + v13_bytes, original + rules_end, kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v13_bytes + kArRegionalLairHistoryEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.house_credit == kArRegionalSource_US &&
        loaded.effective.house_credit == kArRegionalSource_US);
  CHECK(!memcmp(loaded.lairs.stock, session.lairs.stock, sizeof(session.lairs.stock)));
  const size_t v14_bytes = offsets[57];
  memcpy(mutated, original, v14_bytes);
  ByteOrder_WriteLe16(mutated + 8, 14);
  ByteOrder_WriteLe16(mutated + 10, 57);
  memcpy(mutated + v14_bytes, original + rules_end, kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v14_bytes + kArRegionalLairHistoryEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < kArRegionalScore_Count; ++i)
    CHECK(loaded.requested.score_feedback.source[i] == kArRegionalSource_US &&
          loaded.effective.score_feedback.source[i] == kArRegionalSource_US);
  const size_t v15_bytes = offsets[60];
  memcpy(mutated, original, v15_bytes);
  ByteOrder_WriteLe16(mutated + 8, 15);
  ByteOrder_WriteLe16(mutated + 10, 60);
  memcpy(mutated + v15_bytes, original + rules_end, kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v15_bytes + kArRegionalLairHistoryEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.score_feedback.source[kArRegionalScore_Phase] == kArRegionalSource_US &&
        loaded.effective.score_feedback.source[kArRegionalScore_Phase] == kArRegionalSource_US);
  for (unsigned i = 0; i < kArRegionalScore_Phase; ++i) {
    CHECK(loaded.requested.score_feedback.source[i] == session.requested.score_feedback.source[i]);
    CHECK(loaded.effective.score_feedback.source[i] == session.effective.score_feedback.source[i]);
  }
  CHECK(!memcmp(loaded.lairs.stock, session.lairs.stock, sizeof(session.lairs.stock)));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  const size_t v17_bytes = offsets[62];
  memcpy(mutated, original, v17_bytes);
  ByteOrder_WriteLe16(mutated + 8, 17);
  ByteOrder_WriteLe16(mutated + 10, 62);
  memcpy(mutated + v17_bytes, original + rules_end, kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v17_bytes + kArRegionalLairHistoryEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < kArRegionalSourceItem_Count; ++i)
    CHECK(loaded.requested.sources.source[i] == kArRegionalSource_US &&
          loaded.effective.sources.source[i] == kArRegionalSource_US);
  CHECK(TestRegional_EqualLairs(&loaded.lairs, &session.lairs));
  ArRegionalSourcesPolicy sources = {{kArRegionalSource_Japan, kArRegionalSource_Europe}};
  const size_t v18_bytes = offsets[64];
  memcpy(mutated, original, v18_bytes);
  ByteOrder_WriteLe16(mutated + 8, 18);
  ByteOrder_WriteLe16(mutated + 10, 64);
  memcpy(mutated + v18_bytes, original + rules_end, kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v18_bytes + kArRegionalLairHistoryEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.skull_wait == kArRegionalSource_US &&
        loaded.effective.skull_wait == kArRegionalSource_US);
  CHECK(TestRegional_EqualLairs(&loaded.lairs, &session.lairs));
  CHECK(ArRegionalSession_RequestSkullWait(&session, session.revision, kArRegionalSource_Japan));
  uint16_t frames;
  CHECK(ArRegionalSession_BeginSkullWait(&session, &frames) && !frames);
  CHECK(ArRegionalSession_RequestSkullWait(&session, session.revision, kArRegionalSource_Europe));
  const size_t v19_bytes = offsets[65];
  memcpy(mutated, original, v19_bytes);
  ByteOrder_WriteLe16(mutated + 8, 19);
  ByteOrder_WriteLe16(mutated + 10, 65);
  memcpy(mutated + v19_bytes, original + rules_end, kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v19_bytes + kArRegionalLairHistoryEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < kArRegionalStory_Count; ++i)
    CHECK(loaded.requested.story.source[i] == kArRegionalSource_US &&
          loaded.effective.story.source[i] == kArRegionalSource_US);
  CHECK(TestRegional_EqualLairs(&loaded.lairs, &session.lairs));
  ArRegionalStoryPolicy story = {{1, 2, 0}};
  ArRegionalStorySnapshot story_snapshot;
  const size_t v20_bytes = offsets[68];
  memcpy(mutated, original, v20_bytes);
  ByteOrder_WriteLe16(mutated + 8, 20);
  ByteOrder_WriteLe16(mutated + 10, 68);
  memcpy(mutated + v20_bytes, original + rules_end, kArRegionalLairHistoryEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v20_bytes + kArRegionalLairHistoryEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.lair_reloads == kArRegionalSource_US &&
        loaded.effective.lair_reloads == kArRegionalSource_US && !loaded.reloads.initialized_towns);
  CHECK(TestRegional_EqualLairs(&loaded.lairs, &session.lairs));
  const size_t v21_bytes = offsets[69];
  memcpy(mutated, original, v21_bytes);
  ByteOrder_WriteLe16(mutated + 8, 21);
  ByteOrder_WriteLe16(mutated + 10, 69);
  const size_t old_history_size =
      kArRegionalLairHistoryEncodedBytes + kArRegionalLairReloadEncodedBytes;
  memcpy(mutated + v21_bytes, original + rules_end, old_history_size);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v21_bytes + old_history_size, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < kArRegionalTownStatus_Count; ++i)
    CHECK(loaded.requested.town_status.source[i] == kArRegionalSource_US &&
          loaded.effective.town_status.source[i] == kArRegionalSource_US);
  CHECK(TestRegional_EqualLairs(&loaded.lairs, &session.lairs) &&
        TestRegional_EqualReloads(&loaded.reloads, &session.reloads));
  const size_t v22_bytes = offsets[74];
  memcpy(mutated, original, v22_bytes);
  ByteOrder_WriteLe16(mutated + 8, 22);
  ByteOrder_WriteLe16(mutated + 10, 74);
  memcpy(mutated + v22_bytes, original + rules_end, old_history_size);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v22_bytes + old_history_size, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.level_goals == 0 && loaded.effective.level_goals == 0);
  const size_t v23_bytes = offsets[75];
  memcpy(mutated, original, v23_bytes);
  ByteOrder_WriteLe16(mutated + 8, 23);
  ByteOrder_WriteLe16(mutated + 10, 75);
  memcpy(mutated + v23_bytes, original + rules_end, old_history_size);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v23_bytes + old_history_size, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  uint16_t combat;
  CHECK(ArRegionalSimCombat_Resolve(&loaded.requested.sim_combat, &combat) && !combat);
  CHECK(!loaded.sim_actors.active_town_tag && !loaded.sim_actors.cached[0].combat);
  const size_t v24_bytes = offsets[80];
  memcpy(mutated, original, v24_bytes);
  ByteOrder_WriteLe16(mutated + 8, 24);
  ByteOrder_WriteLe16(mutated + 10, 80);
  memcpy(mutated + v24_bytes, original + rules_end, old_history_size);
  const ArRegionalSimActors old_actors = {
      .cached = {{.combat = 31}}, .active = {{.combat = 7}}, .active_town_tag = 1};
  CHECK(ArRegionalSimActors_EncodeVersion(&old_actors, mutated + v24_bytes + old_history_size,
                                          sizeof(mutated) - v24_bytes - old_history_size, 1));
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v24_bytes + old_history_size + kArRegionalSimActorsV1EncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.sim_actors.cached[0].combat == 31 && loaded.sim_actors.active[0].combat == 7 &&
        !loaded.sim_actors.active[0].ai);
  CHECK(ArRegionalSimAi_Resolve(&loaded.requested.sim_ai, &combat) && !combat);
  CHECK(TestRegional_EqualLairs(&loaded.lairs, &session.lairs) &&
        TestRegional_EqualReloads(&loaded.reloads, &session.reloads));
  const size_t v25_bytes = offsets[86];
  memcpy(mutated, original, v25_bytes);
  ByteOrder_WriteLe16(mutated + 8, 25);
  ByteOrder_WriteLe16(mutated + 10, 86);
  memcpy(mutated + v25_bytes, original + rules_end,
         old_history_size + kArRegionalSimActorsEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v25_bytes + old_history_size + kArRegionalSimActorsEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.construction == 0 && loaded.effective.construction == 0);
  const size_t v26_bytes = offsets[87];
  memcpy(mutated, original, v26_bytes);
  ByteOrder_WriteLe16(mutated + 8, 26);
  ByteOrder_WriteLe16(mutated + 10, 87);
  memcpy(mutated + v26_bytes, original + rules_end,
         old_history_size + kArRegionalSimActorsEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v26_bytes + old_history_size + kArRegionalSimActorsEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < 5; ++i)
    CHECK(loaded.requested.support.source[i] == 0 && loaded.effective.support.source[i] == 0);
  CHECK(TestRegional_EqualLairs(&loaded.lairs, &session.lairs) &&
        TestRegional_EqualReloads(&loaded.reloads, &session.reloads));
  CHECK(TestRegional_EqualActors(&loaded.sim_actors, &session.sim_actors));
  const size_t v27_bytes = offsets[92];
  memcpy(mutated, original, v27_bytes);
  ByteOrder_WriteLe16(mutated + 8, 27);
  ByteOrder_WriteLe16(mutated + 10, 92);
  memcpy(mutated + v27_bytes, original + rules_end,
         old_history_size + kArRegionalSimActorsEncodedBytes);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v27_bytes + old_history_size + kArRegionalSimActorsEncodedBytes,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.arrival_locked && !loaded.requested.arrival && !loaded.effective.arrival);
  const size_t v28_bytes = offsets[93];
  memcpy(mutated, original, v28_bytes);
  ByteOrder_WriteLe16(mutated + 8, 28);
  ByteOrder_WriteLe16(mutated + 10, 93);
  memcpy(mutated + v28_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v28_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < kArRegionalActionMotion_Count; ++i)
    CHECK(loaded.requested.action_motion.source[i] == 0 &&
          loaded.effective.action_motion.source[i] == 0);
  const size_t v29_bytes = offsets[100];
  memcpy(mutated, original, v29_bytes);
  ByteOrder_WriteLe16(mutated + 8, 29);
  ByteOrder_WriteLe16(mutated + 10, 100);
  memcpy(mutated + v29_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v29_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < 7; ++i)
    CHECK(loaded.requested.action_motion.source[i] == session.requested.action_motion.source[i] &&
          loaded.effective.action_motion.source[i] == session.effective.action_motion.source[i]);
  for (unsigned i = 7; i < kArRegionalActionMotion_Count; ++i)
    CHECK(!loaded.requested.action_motion.source[i] && !loaded.effective.action_motion.source[i]);
  const size_t v30_bytes = offsets[102];
  memcpy(mutated, original, v30_bytes);
  ByteOrder_WriteLe16(mutated + 8, 30);
  ByteOrder_WriteLe16(mutated + 10, 102);
  memcpy(mutated + v30_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v30_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < 9; ++i)
    CHECK(loaded.requested.action_motion.source[i] == session.requested.action_motion.source[i] &&
          loaded.effective.action_motion.source[i] == session.effective.action_motion.source[i]);
  CHECK(!loaded.requested.action_motion.source[9] && !loaded.effective.action_motion.source[9]);
  const size_t v31_bytes = offsets[103];
  memcpy(mutated, original, v31_bytes);
  ByteOrder_WriteLe16(mutated + 8, 31);
  ByteOrder_WriteLe16(mutated + 10, 103);
  memcpy(mutated + v31_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v31_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < 10; ++i)
    CHECK(loaded.requested.action_motion.source[i] == session.requested.action_motion.source[i] &&
          loaded.effective.action_motion.source[i] == session.effective.action_motion.source[i]);
  for (unsigned i = 10; i < 12; ++i)
    CHECK(!loaded.requested.action_motion.source[i] && !loaded.effective.action_motion.source[i]);
  const size_t v32_bytes = offsets[105];
  memcpy(mutated, original, v32_bytes);
  ByteOrder_WriteLe16(mutated + 8, 32);
  ByteOrder_WriteLe16(mutated + 10, 105);
  memcpy(mutated + v32_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v32_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded.requested.action_motion, &session.requested.action_motion,
                sizeof(session.requested.action_motion)));
  for (unsigned i = 0; i < kArRegionalEmitter_Count; ++i)
    CHECK(!loaded.requested.emitters.source[i] && !loaded.effective.emitters.source[i]);
  const size_t v33_bytes = offsets[107];
  memcpy(mutated, original, v33_bytes);
  ByteOrder_WriteLe16(mutated + 8, 33);
  ByteOrder_WriteLe16(mutated + 10, 107);
  memcpy(mutated + v33_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v33_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.statue_volley && !loaded.effective.statue_volley);
  CHECK(!memcmp(&loaded.requested.emitters, &session.requested.emitters,
                sizeof(session.requested.emitters)));
  const size_t v34_bytes = offsets[108];
  memcpy(mutated, original, v34_bytes);
  ByteOrder_WriteLe16(mutated + 8, 34);
  ByteOrder_WriteLe16(mutated + 10, 108);
  memcpy(mutated + v34_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v34_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.statue_volley == session.requested.statue_volley &&
        loaded.effective.statue_volley == session.effective.statue_volley);
  for (unsigned i = 0; i < kArRegionalBoss_Count; ++i)
    CHECK(!loaded.requested.bosses.source[i] && !loaded.effective.bosses.source[i]);
  const size_t v35_bytes = offsets[114];
  memcpy(mutated, original, v35_bytes);
  ByteOrder_WriteLe16(mutated + 8, 35);
  ByteOrder_WriteLe16(mutated + 10, 114);
  memcpy(mutated + v35_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v35_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < 6; ++i)
    CHECK(loaded.requested.bosses.source[i] == session.requested.bosses.source[i] &&
          loaded.effective.bosses.source[i] == session.effective.bosses.source[i]);
  CHECK(!loaded.requested.bosses.source[6] && !loaded.effective.bosses.source[6]);
  const size_t v36_bytes = offsets[115];
  memcpy(mutated, original, v36_bytes);
  ByteOrder_WriteLe16(mutated + 8, 36);
  ByteOrder_WriteLe16(mutated + 10, 115);
  memcpy(mutated + v36_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v36_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < kArRegionalCollision_Count; ++i)
    CHECK(!loaded.requested.collision.source[i] && !loaded.effective.collision.source[i]);
  const size_t v37_bytes = offsets[117];
  memcpy(mutated, original, v37_bytes);
  ByteOrder_WriteLe16(mutated + 8, 37);
  ByteOrder_WriteLe16(mutated + 10, 117);
  memcpy(mutated + v37_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v37_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < kArRegionalPlatformSkull_Count; ++i)
    CHECK(!loaded.requested.platform_skull.source[i] && !loaded.effective.platform_skull.source[i]);
  const size_t v38_bytes = offsets[121];
  memcpy(mutated, original, v38_bytes);
  ByteOrder_WriteLe16(mutated + 8, 38);
  ByteOrder_WriteLe16(mutated + 10, 121);
  memcpy(mutated + v38_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v38_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < kArRegionalActorStat_Count; ++i)
    CHECK(!loaded.requested.actor_stats.source[i] && !loaded.effective.actor_stats.source[i]);
  const size_t v39_bytes = offsets[184];
  memcpy(mutated, original, v39_bytes);
  ByteOrder_WriteLe16(mutated + 8, 39);
  ByteOrder_WriteLe16(mutated + 10, 184);
  memcpy(mutated + v39_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v39_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < kArRegionalActorStat_BaseCount; ++i)
    CHECK(loaded.requested.actor_stats.source[i] == session.requested.actor_stats.source[i] &&
          loaded.effective.actor_stats.source[i] == session.effective.actor_stats.source[i]);
  for (unsigned i = kArRegionalActorStat_BaseCount; i < kArRegionalActorStat_Count; ++i)
    CHECK(!loaded.requested.actor_stats.source[i] && !loaded.effective.actor_stats.source[i]);
  const size_t v40_bytes = offsets[187];
  memcpy(mutated, original, v40_bytes);
  ByteOrder_WriteLe16(mutated + 8, 40);
  ByteOrder_WriteLe16(mutated + 10, 187);
  memcpy(mutated + v40_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v40_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < 7; ++i)
    CHECK(loaded.requested.bosses.source[i] == session.requested.bosses.source[i] &&
          loaded.effective.bosses.source[i] == session.effective.bosses.source[i]);
  for (unsigned i = 7; i < kArRegionalBoss_Count; ++i)
    CHECK(!loaded.requested.bosses.source[i] && !loaded.effective.bosses.source[i]);
  const size_t v41_bytes = offsets[191];
  memcpy(mutated, original, v41_bytes);
  ByteOrder_WriteLe16(mutated + 8, 41);
  ByteOrder_WriteLe16(mutated + 10, 191);
  memcpy(mutated + v41_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v41_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 0; i < 3; ++i)
    CHECK(!loaded.requested.cast_hold.source[i] && !loaded.effective.cast_hold.source[i]);
  const size_t v42_bytes = offsets[194];
  memcpy(mutated, original, v42_bytes);
  ByteOrder_WriteLe16(mutated + 8, 42);
  ByteOrder_WriteLe16(mutated + 10, 194);
  memcpy(mutated + v42_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v42_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 11; i < kArRegionalBoss_Count; ++i)
    CHECK(!loaded.requested.bosses.source[i] && !loaded.effective.bosses.source[i]);
  const size_t v43_bytes = offsets[196];
  memcpy(mutated, original, v43_bytes);
  ByteOrder_WriteLe16(mutated + 8, 43);
  ByteOrder_WriteLe16(mutated + 10, 196);
  memcpy(mutated + v43_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v43_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 13; i < kArRegionalBoss_Count; ++i)
    CHECK(!loaded.requested.bosses.source[i] && !loaded.effective.bosses.source[i]);
  const size_t v44_bytes = offsets[200];
  memcpy(mutated, original, v44_bytes);
  ByteOrder_WriteLe16(mutated + 8, 44);
  ByteOrder_WriteLe16(mutated + 10, 200);
  memcpy(mutated + v44_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v44_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 13; i < kArRegionalBoss_Count; ++i)
    CHECK(!loaded.requested.bosses.source[i] && !loaded.effective.bosses.source[i]);
  for (unsigned i = 0; i < 4; ++i)
    CHECK(!loaded.requested.fire_enemy.source[i] && !loaded.effective.fire_enemy.source[i]);
  const size_t v45_bytes = offsets[202];
  memcpy(mutated, original, v45_bytes);
  ByteOrder_WriteLe16(mutated + 8, 45);
  ByteOrder_WriteLe16(mutated + 10, 202);
  memcpy(mutated + v45_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v45_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 15; i < kArRegionalBoss_Count; ++i)
    CHECK(!loaded.requested.bosses.source[i] && !loaded.effective.bosses.source[i]);
  const size_t v46_bytes = offsets[206];
  memcpy(mutated, original, v46_bytes);
  ByteOrder_WriteLe16(mutated + 8, 46);
  ByteOrder_WriteLe16(mutated + 10, 206);
  memcpy(mutated + v46_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v46_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.action_motion.source[kArRegionalActionMotion_HeadWithdrawal] &&
        !loaded.effective.action_motion.source[kArRegionalActionMotion_HeadWithdrawal]);
  const size_t v47_bytes = offsets[207];
  memcpy(mutated, original, v47_bytes);
  ByteOrder_WriteLe16(mutated + 8, 47);
  ByteOrder_WriteLe16(mutated + 10, 207);
  memcpy(mutated + v47_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v47_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 19; i < kArRegionalBoss_Count; ++i)
    CHECK(!loaded.requested.bosses.source[i] && !loaded.effective.bosses.source[i]);
  const size_t v48_bytes = offsets[210];
  memcpy(mutated, original, v48_bytes);
  ByteOrder_WriteLe16(mutated + 8, 48);
  ByteOrder_WriteLe16(mutated + 10, 210);
  memcpy(mutated + v48_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v48_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 22; i < kArRegionalBoss_Count; ++i)
    CHECK(!loaded.requested.bosses.source[i] && !loaded.effective.bosses.source[i]);
  const size_t v49_bytes = offsets[214];
  memcpy(mutated, original, v49_bytes);
  ByteOrder_WriteLe16(mutated + 8, 49);
  ByteOrder_WriteLe16(mutated + 10, 214);
  memcpy(mutated + v49_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v49_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.action_motion.source[kArRegionalActionMotion_TreeSeeds] &&
        !loaded.effective.action_motion.source[kArRegionalActionMotion_TreeSeeds]);
  const size_t v50_bytes = offsets[215];
  memcpy(mutated, original, v50_bytes);
  ByteOrder_WriteLe16(mutated + 8, 50);
  ByteOrder_WriteLe16(mutated + 10, 215);
  memcpy(mutated + v50_bytes, original + rules_end, size - rules_end - 10);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v50_bytes + size - rules_end - 10, RegionalSessionTest_AcceptOpaque,
                              NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.difficulty.level && !loaded.effective.difficulty.level);
  for (unsigned i = 0; i < kArRegionalDifficultyRule_Count; ++i)
    CHECK(!loaded.requested.difficulty.source[i] && !loaded.effective.difficulty.source[i]);
  const size_t v51_bytes = offsets[220];
  memcpy(mutated, original, v51_bytes);
  ByteOrder_WriteLe16(mutated + 8, 51);
  ByteOrder_WriteLe16(mutated + 10, 220);
  memcpy(mutated + v51_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v51_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.score_lives && !loaded.effective.score_lives);
  const size_t v52_bytes = offsets[221];
  memcpy(mutated, original, v52_bytes);
  ByteOrder_WriteLe16(mutated + 8, 52);
  ByteOrder_WriteLe16(mutated + 10, 221);
  memcpy(mutated + v52_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v52_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.action_start.source[0] && !loaded.effective.action_start.source[1]);
  const size_t v53_bytes = offsets[223];
  memcpy(mutated, original, v53_bytes);
  ByteOrder_WriteLe16(mutated + 8, 53);
  ByteOrder_WriteLe16(mutated + 10, 223);
  memcpy(mutated + v53_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v53_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.spell_inventory && !loaded.effective.spell_inventory);
  const size_t v54_bytes = offsets[224];
  memcpy(mutated, original, v54_bytes);
  ByteOrder_WriteLe16(mutated + 8, 54);
  ByteOrder_WriteLe16(mutated + 10, 224);
  memcpy(mutated + v54_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v54_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.mode_entry.source[0] && !loaded.effective.mode_entry.source[1]);
  const size_t v55_bytes = offsets[226];
  memcpy(mutated, original, v55_bytes);
  ByteOrder_WriteLe16(mutated + 8, 55);
  ByteOrder_WriteLe16(mutated + 10, 226);
  memcpy(mutated + v55_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v55_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 26; i < 30; ++i)
    CHECK(!loaded.requested.bosses.source[i] && !loaded.effective.bosses.source[i]);
  const size_t v56_bytes = offsets[230];
  memcpy(mutated, original, v56_bytes);
  ByteOrder_WriteLe16(mutated + 8, 56);
  ByteOrder_WriteLe16(mutated + 10, 230);
  memcpy(mutated + v56_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v56_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.bosses.source[30] && !loaded.effective.bosses.source[30]);
  const size_t v57_bytes = offsets[231];
  memcpy(mutated, original, v57_bytes);
  ByteOrder_WriteLe16(mutated + 8, 57);
  ByteOrder_WriteLe16(mutated + 10, 231);
  memcpy(mutated + v57_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v57_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.hazards && !loaded.effective.hazards);
  const size_t v58_bytes = offsets[232];
  memcpy(mutated, original, v58_bytes);
  ByteOrder_WriteLe16(mutated + 8, 58);
  ByteOrder_WriteLe16(mutated + 10, 232);
  memcpy(mutated + v58_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v58_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.terrain && !loaded.effective.terrain);
  const size_t v59_bytes = offsets[233];
  memcpy(mutated, original, v59_bytes);
  ByteOrder_WriteLe16(mutated + 8, 59);
  ByteOrder_WriteLe16(mutated + 10, 233);
  memcpy(mutated + v59_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v59_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.music && !loaded.effective.music);
  uint8_t music = 255;
  const size_t v61_bytes = offsets[236];
  memcpy(mutated, original, v61_bytes);
  ByteOrder_WriteLe16(mutated + 8, 61);
  ByteOrder_WriteLe16(mutated + 10, 236);
  memcpy(mutated + v61_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v61_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.mosaic && !loaded.effective.mosaic);
  const size_t v62_bytes = offsets[237];
  memcpy(mutated, original, v62_bytes);
  ByteOrder_WriteLe16(mutated + 8, 62);
  ByteOrder_WriteLe16(mutated + 10, 237);
  memcpy(mutated + v62_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v62_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.artwork.source[0] && !loaded.effective.artwork.source[0]);
  const size_t v63_bytes = offsets[238];
  memcpy(mutated, original, v63_bytes);
  ByteOrder_WriteLe16(mutated + 8, 63);
  ByteOrder_WriteLe16(mutated + 10, 238);
  memcpy(mutated + v63_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v63_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.artwork.source[1] && !loaded.effective.artwork.source[1]);
  const size_t v64_bytes = offsets[239];
  memcpy(mutated, original, v64_bytes);
  ByteOrder_WriteLe16(mutated + 8, 64);
  ByteOrder_WriteLe16(mutated + 10, 239);
  memcpy(mutated + v64_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v64_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned i = 2; i < 5; ++i)
    CHECK(!loaded.requested.artwork.source[i] && !loaded.effective.artwork.source[i]);
  uint8_t art = 255;
  const size_t v68_bytes = offsets[247];
  memcpy(mutated, original, v68_bytes);
  ByteOrder_WriteLe16(mutated + 8, 68);
  ByteOrder_WriteLe16(mutated + 10, 247);
  memcpy(mutated + v68_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v68_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  for (unsigned area = 0; area < 7; ++area)
    CHECK(!loaded.requested.actor_artwork.source[area] &&
          !loaded.effective.actor_artwork.source[area]);
  for (unsigned area = 0; area < 7; ++area)
    for (unsigned source = 0; source < 3; ++source) {
      ArRegionalActorArtworkPolicy policy = session.requested.actor_artwork;
      policy.source[area] = source;
      CHECK(ArRegionalSession_RequestActorArtwork(&session, session.revision, &policy));
      CHECK(
          ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
      CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready &&
            RegionalSessionTest_Equal(&loaded, &session));
      bool enabled;
      CHECK(ArRegionalSession_BeginActorArtwork(&session, area, &enabled) &&
            enabled == (source == 1));
      CHECK(
          ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
      CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready &&
            RegionalSessionTest_Equal(&loaded, &session));
    }
  const size_t v67_bytes = offsets[245];
  memcpy(mutated, original, v67_bytes);
  ByteOrder_WriteLe16(mutated + 8, 67);
  ByteOrder_WriteLe16(mutated + 10, 245);
  memcpy(mutated + v67_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v67_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.sequences.source[0] && !loaded.requested.sequences.source[1] &&
        !loaded.effective.sequences.source[0] && !loaded.effective.sequences.source[1]);
  bool sequence;
  for (unsigned a = 0; a < 3; ++a)
    for (unsigned b = 0; b < 3; ++b) {
      const ArRegionalSequencePolicy policy = {{a, b}};
      CHECK(!ArRegionalSession_RequestSequences(&session, session.revision - 1, &policy));
      CHECK(ArRegionalSession_RequestSequences(&session, session.revision, &policy));
      CHECK(
          ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
      CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready &&
            RegionalSessionTest_Equal(&loaded, &session));
      const ArRegionalSource previous = session.effective.sequences.source[1];
      CHECK(ArRegionalSession_BeginSequence(&session, 0, &sequence) && sequence == (a == 1));
      CHECK(session.effective.sequences.source[1] == previous);
      CHECK(ArRegionalSession_BeginSequence(&session, 1, &sequence) && sequence == (b == 1));
    }
  CHECK(!ArRegionalSession_RequestSequences(&session, session.revision,
                                            &(ArRegionalSequencePolicy){{3, 0}}));
  CHECK(!ArRegionalSession_RequestSequences(&session, session.revision,
                                            &(ArRegionalSequencePolicy){{0, 3}}));
  CHECK(!ArRegionalSession_RequestSequences(&session, session.revision, NULL));
  CHECK(!ArRegionalSession_BeginSequence(NULL, 0, &sequence) &&
        !ArRegionalSession_BeginSequence(&session, 0, NULL));
  CHECK(!ArRegionalSession_BeginSequence(&session, 2, &sequence));
  ArRegionalSession sequence_exhausted = session;
  sequence_exhausted.revision = UINT32_MAX;
  CHECK(ArRegionalSession_RequestSequences(&sequence_exhausted, UINT32_MAX,
                                           &sequence_exhausted.requested.sequences));
  CHECK(!ArRegionalSession_RequestSequences(&sequence_exhausted, UINT32_MAX,
                                            &(ArRegionalSequencePolicy){{1, 0}}));
  sequence_exhausted.requested.sequences.source[0] = 1;
  sequence = false;
  CHECK(!ArRegionalSession_BeginSequence(&sequence_exhausted, 0, &sequence) && !sequence);
  const size_t v66_bytes = offsets[243];
  memcpy(mutated, original, v66_bytes);
  ByteOrder_WriteLe16(mutated + 8, 66);
  ByteOrder_WriteLe16(mutated + 10, 243);
  memcpy(mutated + v66_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v66_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.poses.source[0] && !loaded.requested.poses.source[1]);
  uint8_t poses;
  for (unsigned a = 0; a < 3; ++a)
    for (unsigned b = 0; b < 3; ++b) {
      const ArRegionalPosePolicy policy = {{a, b}};
      CHECK(!ArRegionalSession_RequestPoses(&session, session.revision - 1, &policy));
      CHECK(ArRegionalSession_RequestPoses(&session, session.revision, &policy));
      CHECK(
          ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
      CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready &&
            RegionalSessionTest_Equal(&loaded, &session));
      CHECK(ArRegionalSession_BeginPoses(&session, &poses) &&
            poses == ((a == 1) | ((b == 1) << 1)));
    }
  CHECK(
      !ArRegionalSession_RequestPoses(&session, session.revision, &(ArRegionalPosePolicy){{3, 0}}));
  CHECK(
      !ArRegionalSession_RequestPoses(&session, session.revision, &(ArRegionalPosePolicy){{0, 3}}));
  CHECK(!ArRegionalSession_RequestPoses(&session, session.revision, NULL));
  CHECK(!ArRegionalSession_BeginPoses(NULL, &poses) &&
        !ArRegionalSession_BeginPoses(&session, NULL));
  ArRegionalSession pose_exhausted = session;
  pose_exhausted.revision = UINT32_MAX;
  CHECK(
      ArRegionalSession_RequestPoses(&pose_exhausted, UINT32_MAX, &pose_exhausted.requested.poses));
  CHECK(!ArRegionalSession_RequestPoses(&pose_exhausted, UINT32_MAX,
                                        &(ArRegionalPosePolicy){{1, 0}}));
  pose_exhausted.requested.poses.source[0] = 1;
  poses = 0xa5;
  CHECK(!ArRegionalSession_BeginPoses(&pose_exhausted, &poses) && poses == 0xa5);
  const size_t v65_bytes = offsets[242];
  memcpy(mutated, original, v65_bytes);
  ByteOrder_WriteLe16(mutated + 8, 65);
  ByteOrder_WriteLe16(mutated + 10, 242);
  memcpy(mutated + v65_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v65_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.artwork.source[kArRegionalArtwork_TitleBackground] &&
        !loaded.effective.artwork.source[kArRegionalArtwork_TitleBackground]);
  CHECK(ArRegionalSession_RequestArtwork(&session, session.revision,
                                         kArRegionalArtwork_TitleBackground, 1));
  CHECK(ArRegionalSession_BeginArtwork(&session, &art) && !(art & kArRegionalArtwork_TitleMask));
  CHECK(ArRegionalSession_BeginTownArtwork(&session, &art) &&
        !(art & kArRegionalArtwork_TitleMask));
  CHECK(ArRegionalSession_BeginTitleArtwork(&session, &art) && art == kArRegionalArtwork_TitleMask);
  CHECK(ArRegionalSession_RequestArtwork(&session, session.revision,
                                         kArRegionalArtwork_TitleBackground, 0));
  CHECK(ArRegionalSession_BeginTitleArtwork(&session, &art) && !art);
  CHECK(!ArRegionalSession_BeginTitleArtwork(NULL, &art) &&
        !ArRegionalSession_BeginTitleArtwork(&session, NULL));
  CHECK(!ArRegionalSession_RequestArtwork(&session, session.revision - 1, 0, 1));
  CHECK(!ArRegionalSession_RequestArtwork(&session, session.revision, 0, 3));
  CHECK(!ArRegionalSession_RequestArtwork(&session, session.revision, kArRegionalArtwork_Count, 1));
  CHECK(ArRegionalSession_RequestArtwork(&session, session.revision, 0, 1));
  CHECK(!session.effective.artwork.source[0] && !ArRegionalSession_BeginArtwork(&session, NULL));
  CHECK(ArRegionalSession_BeginArtwork(&session, &art) && art == 1);
  CHECK(ArRegionalSession_RequestArtwork(&session, session.revision, 0, 2));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.artwork.source[0] == 2 && loaded.effective.artwork.source[0] == 1);
  ArRegionalSession art_exhausted = session;
  art_exhausted.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginArtwork(&art_exhausted, &art) && art == 1);
  CHECK(!ArRegionalSession_RequestArtwork(&art_exhausted, UINT32_MAX, 0, 0));
  CHECK(ArRegionalSession_RequestArtwork(&session, session.revision, 1, 2));
  CHECK(ArRegionalSession_BeginArtwork(&session, &art) && art == 2);
  CHECK(ArRegionalSession_RequestArtwork(&session, session.revision, 1, 0));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.artwork.source[1] && loaded.effective.artwork.source[1] == 2);
  for (unsigned rule = 2; rule < 5; ++rule) {
    CHECK(ArRegionalSession_RequestArtwork(&session, session.revision, rule, 1));
    CHECK(ArRegionalSession_BeginArtwork(&session, &art) && !art);
    CHECK(!session.effective.artwork.source[rule]);
    CHECK(ArRegionalSession_BeginTownArtwork(&session, &art) && art == (1u << rule));
    CHECK(session.effective.artwork.source[rule] == 1);
    CHECK(ArRegionalSession_RequestArtwork(&session, session.revision, rule, 2));
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
    CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
    CHECK(loaded.requested.artwork.source[rule] == 2 && loaded.effective.artwork.source[rule] == 1);
    ArRegionalSession exhausted = session;
    exhausted.revision = UINT32_MAX;
    CHECK(!ArRegionalSession_BeginTownArtwork(&exhausted, &art) && art == (1u << rule));
    CHECK(ArRegionalSession_BeginTownArtwork(&session, &art) && !art);
  }
  CHECK(!ArRegionalSession_BeginTownArtwork(NULL, &art) &&
        !ArRegionalSession_BeginTownArtwork(&session, NULL));
  uint8_t mosaic = 255;
  CHECK(!ArRegionalSession_RequestMosaic(&session, session.revision - 1, 1));
  CHECK(!ArRegionalSession_RequestMosaic(&session, session.revision, 3));
  CHECK(ArRegionalSession_RequestMosaic(&session, session.revision, 1));
  CHECK(!session.effective.mosaic && !ArRegionalSession_BeginMosaic(&session, NULL));
  CHECK(ArRegionalSession_BeginMosaic(&session, &mosaic) && mosaic == 1);
  CHECK(ArRegionalSession_RequestMosaic(&session, session.revision, 2));
  CHECK(session.effective.mosaic == 1);
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.mosaic == 2 && loaded.effective.mosaic == 1);
  ArRegionalSession mosaic_exhausted = session;
  mosaic_exhausted.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginMosaic(&mosaic_exhausted, &mosaic) && mosaic == 1);
  CHECK(!ArRegionalSession_RequestMosaic(&mosaic_exhausted, UINT32_MAX, 0));
  const size_t v60_bytes = offsets[234];
  memcpy(mutated, original, v60_bytes);
  ByteOrder_WriteLe16(mutated + 8, 60);
  ByteOrder_WriteLe16(mutated + 10, 234);
  memcpy(mutated + v60_bytes, original + rules_end, size - rules_end);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated,
                              v60_bytes + size - rules_end, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!loaded.requested.placements.enemies && !loaded.effective.placements.enemies);
  CHECK(!loaded.requested.placements.pickups && !loaded.effective.placements.pickups);
  ArRegionalPlacementPolicy placements = {2, 1}, placement_snapshot = {0};
  CHECK(!ArRegionalSession_RequestPlacements(&session, session.revision - 1, &placements));
  CHECK(!ArRegionalSession_RequestPlacements(&session, session.revision, NULL));
  CHECK(!ArRegionalSession_RequestPlacements(&session, session.revision,
                                             &(ArRegionalPlacementPolicy){3, 0}));
  CHECK(ArRegionalSession_RequestPlacements(&session, session.revision, &placements));
  CHECK(!session.effective.placements.enemies &&
        !ArRegionalSession_BeginPlacements(&session, NULL));
  CHECK(ArRegionalSession_BeginPlacements(&session, &placement_snapshot));
  CHECK(placement_snapshot.enemies == 2 && placement_snapshot.pickups == 1);
  placements = (ArRegionalPlacementPolicy){1, 2};
  CHECK(ArRegionalSession_RequestPlacements(&session, session.revision, &placements));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.placements.enemies == 1 && loaded.requested.placements.pickups == 2);
  CHECK(loaded.effective.placements.enemies == 2 && loaded.effective.placements.pickups == 1);
  ArRegionalSession placement_exhausted = session;
  placement_exhausted.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginPlacements(&placement_exhausted, &placement_snapshot) &&
        placement_snapshot.enemies == 2);
  CHECK(
      !ArRegionalSession_RequestPlacements(&placement_exhausted, UINT32_MAX, &placement_snapshot));
  CHECK(!ArRegionalSession_RequestMusic(&session, session.revision - 1, 1));
  CHECK(!ArRegionalSession_RequestMusic(&session, session.revision, 3));
  CHECK(ArRegionalSession_RequestMusic(&session, session.revision, 1));
  CHECK(!session.effective.music && !ArRegionalSession_BeginMusic(&session, NULL));
  CHECK(ArRegionalSession_BeginMusic(&session, &music) && music == 1);
  CHECK(ArRegionalSession_RequestMusic(&session, session.revision, 2));
  CHECK(session.effective.music == 1);
  ArRegionalSession music_exhausted = session;
  music_exhausted.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginMusic(&music_exhausted, &music) && music == 1);
  CHECK(!ArRegionalSession_RequestMusic(&music_exhausted, UINT32_MAX, 0));
  uint8_t terrain = 255;
  CHECK(!ArRegionalSession_RequestTerrain(&session, session.revision - 1, 1));
  CHECK(!ArRegionalSession_RequestTerrain(&session, session.revision, 3));
  CHECK(ArRegionalSession_RequestTerrain(&session, session.revision, 1));
  CHECK(!session.effective.terrain);
  CHECK(!ArRegionalSession_BeginTerrain(&session, NULL));
  CHECK(ArRegionalSession_BeginTerrain(&session, &terrain) && terrain == 1);
  CHECK(ArRegionalSession_RequestTerrain(&session, session.revision, 2));
  CHECK(session.effective.terrain == 1);
  ArRegionalSession terrain_exhausted = session;
  terrain_exhausted.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginTerrain(&terrain_exhausted, &terrain) && terrain == 1);
  CHECK(!ArRegionalSession_RequestTerrain(&terrain_exhausted, UINT32_MAX, 0));
  uint8_t hazards;
  CHECK(!ArRegionalSession_RequestHazards(&session, session.revision - 1, 1));
  CHECK(!ArRegionalSession_RequestHazards(&session, session.revision, 3));
  CHECK(ArRegionalSession_RequestHazards(&session, session.revision, 1));
  CHECK(!session.effective.hazards);
  CHECK(ArRegionalSession_BeginHazards(&session, &hazards) && hazards == 1);
  CHECK(ArRegionalSession_RequestHazards(&session, session.revision, 2));
  CHECK(session.effective.hazards == 1);
  ArRegionalSession exhausted = session;
  exhausted.revision = UINT32_MAX;
  CHECK(!ArRegionalSession_BeginHazards(&exhausted, &hazards) && hazards == 1);
  CHECK(!ArRegionalSession_RequestHazards(&exhausted, UINT32_MAX, 0));
  ArRegionalModePolicy mode_policy = {{2, 1}};
  uint8_t mode_snapshot;
  CHECK(!ArRegionalSession_RequestModeEntry(&session, session.revision - 1, &mode_policy));
  CHECK(ArRegionalSession_RequestModeEntry(&session, session.revision, &mode_policy));
  CHECK(ArRegionalSession_BeginModeEntry(&session, &mode_snapshot) && mode_snapshot == 1);
  mode_policy.source[1] = 2;
  CHECK(ArRegionalSession_RequestModeEntry(&session, session.revision, &mode_policy));
  bool inventory;
  CHECK(!ArRegionalSession_RequestInventory(&session, session.revision - 1, 2));
  CHECK(!ArRegionalSession_RequestInventory(&session, session.revision, 3));
  CHECK(ArRegionalSession_RequestInventory(&session, session.revision, 2));
  CHECK(!session.effective.spell_inventory);
  CHECK(ArRegionalSession_BeginInventory(&session, &inventory) && inventory);
  CHECK(ArRegionalSession_RequestInventory(&session, session.revision, 0));
  CHECK(session.effective.spell_inventory == 2);
  ArRegionalActionStartPolicy start_policy = {{1, 2}};
  ArRegionalActionStartSnapshot start_snapshot;
  CHECK(!ArRegionalSession_RequestActionStart(&session, session.revision - 1, &start_policy));
  CHECK(ArRegionalSession_RequestActionStart(&session, session.revision, &start_policy));
  CHECK(!session.effective.action_start.source[0]);
  CHECK(ArRegionalSession_BeginActionStart(&session, &start_snapshot) &&
        start_snapshot.spares == 2 && start_snapshot.health == 8);
  start_policy.source[0] = 0;
  CHECK(ArRegionalSession_RequestActionStart(&session, session.revision, &start_policy));
  CHECK(session.effective.action_start.source[0] == 1);
  bool score_lives;
  CHECK(!ArRegionalSession_RequestScoreLives(&session, session.revision - 1, 2));
  CHECK(!ArRegionalSession_RequestScoreLives(&session, session.revision, 3));
  CHECK(ArRegionalSession_RequestScoreLives(&session, session.revision, 2));
  CHECK(!session.effective.score_lives);
  CHECK(ArRegionalSession_BeginScoreLives(&session, &score_lives) && score_lives);
  CHECK(ArRegionalSession_RequestScoreLives(&session, session.revision, 0));
  CHECK(session.effective.score_lives == 2);
  ArRegionalDifficultyPolicy difficulty_policy = {{2, 1, 2, 0, 2}, kArRegionalDifficulty_Beginner};
  ArRegionalDifficultySnapshot difficulty_snapshot;
  const ArRegionalSession before_difficulty = session;
  CHECK(!ArRegionalSession_RequestDifficulty(&session, session.revision - 1, &difficulty_policy));
  CHECK(RegionalSessionTest_Equal(&before_difficulty, &session));
  CHECK(ArRegionalSession_RequestDifficulty(&session, session.revision, &difficulty_policy));
  CHECK(!session.effective.difficulty.level);
  CHECK(ArRegionalSession_BeginDifficulty(&session, &difficulty_snapshot) &&
        difficulty_snapshot.spawn_hp == 2 && difficulty_snapshot.timer_reload == 71 &&
        difficulty_snapshot.single_tendril_bob && !difficulty_snapshot.skip_dragon_attack);
  difficulty_policy.level = kArRegionalDifficulty_Expert;
  CHECK(ArRegionalSession_RequestDifficulty(&session, session.revision, &difficulty_policy));
  CHECK(session.effective.difficulty.level == kArRegionalDifficulty_Beginner);
  ArRegionalFirePolicy fire_policy = {{1, 0, 2, 1}};
  uint8_t fire_snapshot;
  CHECK(ArRegionalSession_RequestFire(&session, session.revision, &fire_policy));
  CHECK(ArRegionalSession_BeginFire(&session, &fire_snapshot) && fire_snapshot == 9);
  fire_policy.source[1] = 1;
  CHECK(ArRegionalSession_RequestFire(&session, session.revision, &fire_policy));
  ArRegionalCastHoldPolicy hold_policy = {{2, 0, 1}};
  uint8_t hold_snapshot;
  CHECK(ArRegionalSession_RequestCastHold(&session, session.revision, &hold_policy));
  CHECK(ArRegionalSession_BeginCastHold(&session, &hold_snapshot) && hold_snapshot == 1);
  hold_policy.source[1] = 2;
  CHECK(ArRegionalSession_RequestCastHold(&session, session.revision, &hold_policy));
  ArRegionalActorStatsPolicy stat_policy;
  ArRegionalActorStatsSnapshot stat_snapshot;
  for (unsigned i = 0; i < kArRegionalActorStat_Count; ++i)
    stat_policy.source[i] = i % 3;
  CHECK(ArRegionalSession_RequestActorStats(&session, session.revision, &stat_policy));
  CHECK(ArRegionalSession_BeginActorStats(&session, &stat_snapshot));
  for (unsigned i = 0; i < kArRegionalActorStat_Count; ++i)
    stat_policy.source[i] = (i + 1) % 3;
  CHECK(ArRegionalSession_RequestActorStats(&session, session.revision, &stat_policy));
  ArRegionalPlatformSkullPolicy skull_policy = {{1, 0, 2, 1}};
  uint8_t skull_snapshot;
  CHECK(ArRegionalSession_RequestPlatformSkull(&session, session.revision, &skull_policy));
  CHECK(ArRegionalSession_BeginPlatformSkull(&session, &skull_snapshot) && skull_snapshot == 9);
  skull_policy.source[1] = 1;
  CHECK(ArRegionalSession_RequestPlatformSkull(&session, session.revision, &skull_policy));
  ArRegionalCollisionPolicy collision_policy = {{1, 2}};
  uint8_t collision_snapshot;
  CHECK(ArRegionalSession_RequestCollision(&session, session.revision, &collision_policy));
  CHECK(ArRegionalSession_BeginCollision(&session, &collision_snapshot) && collision_snapshot == 1);
  collision_policy.source[1] = 1;
  CHECK(ArRegionalSession_RequestCollision(&session, session.revision, &collision_policy));
  ArRegionalBossPolicy boss_policy = {{1, 2, 0, 2, 1, 2, 0, 1, 1, 0, 2, 1, 0, 2, 0, 1,
                                       2, 0, 2, 1, 2, 1, 1, 2, 0, 2, 2, 1, 0, 2, 1}};
  uint64_t boss_snapshot;
  CHECK(ArRegionalSession_RequestBosses(&session, session.revision, &boss_policy));
  CHECK(ArRegionalSession_BeginBosses(&session, &boss_snapshot));
  boss_policy.source[5] = 1;
  boss_policy.source[6] = 1;
  boss_policy.source[7] = 0;
  boss_policy.source[9] = 1;
  boss_policy.source[11] = 0;
  boss_policy.source[12] = 1;
  boss_policy.source[13] = 0;
  boss_policy.source[14] = 2;
  boss_policy.source[15] = 0;
  boss_policy.source[16] = 0;
  boss_policy.source[17] = 2;
  boss_policy.source[18] = 1;
  boss_policy.source[19] = 0;
  boss_policy.source[20] = 1;
  boss_policy.source[21] = 2;
  boss_policy.source[22] = 0;
  boss_policy.source[23] = 0;
  boss_policy.source[24] = 2;
  boss_policy.source[25] = 0;
  boss_policy.source[26] = 0;
  boss_policy.source[27] = 2;
  boss_policy.source[28] = 2;
  boss_policy.source[29] = 1;
  boss_policy.source[30] = 0;
  CHECK(ArRegionalSession_RequestBosses(&session, session.revision, &boss_policy));
  bool double_shot;
  CHECK(ArRegionalSession_RequestVolley(&session, session.revision, 2));
  CHECK(ArRegionalSession_BeginVolley(&session, &double_shot) && double_shot);
  CHECK(ArRegionalSession_RequestVolley(&session, session.revision, 0));
  ArRegionalEmitterPolicy emitter_policy = {{2, 1}};
  uint8_t emitter_snapshot;
  CHECK(ArRegionalSession_RequestEmitters(&session, session.revision, &emitter_policy));
  CHECK(ArRegionalSession_BeginEmitters(&session, &emitter_snapshot) && emitter_snapshot == 2);
  emitter_policy.source[1] = 2;
  CHECK(ArRegionalSession_RequestEmitters(&session, session.revision, &emitter_policy));
  ArRegionalActionMotionPolicy motion = {{1, 0, 2, 1, 0, 1, 2, 1, 2, 1, 2, 1, 1}};
  uint16_t motion_snapshot;
  CHECK(ArRegionalSession_RequestActionMotion(&session, session.revision, &motion));
  CHECK(ArRegionalSession_BeginActionMotion(&session, &motion_snapshot) &&
        motion_snapshot == 0x1aa9);
  motion.source[3] = 0;
  motion.source[8] = 1;
  motion.source[12] = 2;
  CHECK(ArRegionalSession_RequestActionMotion(&session, session.revision, &motion));
  CHECK(ArRegionalSession_RequestStory(&session, session.revision, &story));
  CHECK(ArRegionalSession_BeginStory(&session, &story_snapshot));
  story.source[2] = 1;
  CHECK(ArRegionalSession_RequestStory(&session, session.revision, &story));
  ArRegionalSourcesSnapshot sources_snapshot;
  CHECK(ArRegionalSession_RequestSources(&session, session.revision, &sources));
  CHECK(ArRegionalSession_RequestArrival(&session, session.revision, kArRegionalSource_Japan));
  bool japanese;
  CHECK(ArRegionalSession_BeginArrival(&session, false, &japanese) && japanese);
  CHECK(ArRegionalSession_RequestArrival(&session, session.revision, kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginSources(&session, &sources_snapshot));
  sources.source[1] = kArRegionalSource_Japan;
  CHECK(ArRegionalSession_RequestSources(&session, session.revision, &sources));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &session));
  remove(path);
  remove(companion);
}

static void CheckRandomizerRecipe(void) {
  const char *path = "regional-randomizer-codec.srm",
             *companion = "regional-randomizer-codec.srm.archeckpoint";
  remove(path);
  remove(companion);
  const uint8_t id[16] = {17};
  const ArRegionalCostPolicy costs = {{0}};
  ArRegionalSession session, loaded;
  SaveError error;
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 17);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  session.randomizer = RandomizerConfig_Default();
  session.randomizer.enabled = true;
  session.randomizer.seed = 987654321;
  session.randomizer.regional_action = true;
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, NULL, image, &error));
  uint8_t payload[kSaveCheckpointPayloadMax], mutated[kSaveCheckpointPayloadMax];
  size_t size = 0;
  CHECK(SaveCheckpoint_Read(path, image, payload, sizeof(payload), &size, &error) ==
        kSaveCheckpoint_Ready);
  CHECK(ByteOrder_ReadLe16(payload + 8) == 70 && size == 8966 + kRandomizerConfigBytes);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.randomizer.seed == 987654321 && loaded.randomizer.regional_action);
  const ArRegionalSession before = loaded;
  for (unsigned bad = 0; bad < 4; ++bad) {
    memcpy(mutated, payload, size);
    size_t length = size;
    if (bad == 0) mutated[size - kRandomizerConfigBytes + 8] = 2; /* future generator */
    if (bad == 1) mutated[size - 1] = 1;                          /* reserved byte */
    if (bad == 2) --length;
    if (bad == 3) ByteOrder_WriteLe32(mutated + size - kRandomizerConfigBytes + 10, 1000000000);
    CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image, mutated, length,
                                RegionalSessionTest_AcceptOpaque, NULL, &error));
    const SaveCheckpointStatus status = ArRegionalSession_Load(&loaded, 0, path, image, &error);
    CHECK(status == (bad == 0 ? kSaveCheckpoint_Unsupported : kSaveCheckpoint_Invalid));
    CHECK(!memcmp(&loaded, &before, sizeof(loaded)));
    /* A newer/corrupt bound recipe must not be rotated away by a normal save. */
    CHECK(!ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  }
  remove(path);
  remove(companion);
}
/* Captured from the pre-refactor v69 encoder. These fingerprints lock the
 * exact payload bytes, not merely agreement between a new encoder/decoder. */

static void CheckCodecGolden(void) {
  static const uint64_t expected[] = {
      UINT64_C(0x5d38674da232b3cb), UINT64_C(0xfa206a7d77317ca6), UINT64_C(0xbb3c832ff7fee12e),
      UINT64_C(0x1b0fc42ae8ae5198), UINT64_C(0x586cd2132ad8f62d), UINT64_C(0x6c6474454a06c131),
      UINT64_C(0x449db3d71f758f42), UINT64_C(0x463cea1055f6b4bb), UINT64_C(0xef7356c0b78dc313),
  };
  const char *path = "regional-codec-golden.srm";
  const char *companion = "regional-codec-golden.srm.archeckpoint";
  remove(path);
  remove(companion);
  uint8_t image[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(image, 42);
  for (unsigned fixture = 0; fixture < 9; ++fixture) {
    ArRegionalSession session, loaded;
    const uint8_t id[16] = {42};
    const ArRegionalCostPolicy costs = {{0}};
    CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
    for (unsigned town = 0; town < 6; ++town)
      CHECK(ArRegionalLairHistory_InitTown(&session.lairs, town));
    CHECK(ArRegionalLairReloads_Init(&session.reloads));
    CHECK(ArRegionalProfiles_Expand(&session.requested, kArRegionalProfile_Gameplay, fixture / 3,
                                    &session.requested));
    CHECK(ArRegionalProfiles_Expand(&session.requested, kArRegionalProfile_Presentation,
                                    fixture / 3, &session.requested));
    CHECK(ArRegionalProfiles_Expand(&session.effective, kArRegionalProfile_Gameplay, fixture % 3,
                                    &session.effective));
    CHECK(ArRegionalProfiles_Expand(&session.effective, kArRegionalProfile_Presentation,
                                    fixture % 3, &session.effective));
    // Population conversion is atomic; the other families may still be pending.
    session.effective.support = session.requested.support;
    session.effective.level_goals = session.requested.level_goals;
    session.effective.story = session.requested.story;
    session.effective.town_status = session.requested.town_status;
    session.requested.difficulty.level = fixture / 3;
    session.effective.difficulty.level = fixture % 3;
    SaveError error;
    CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, fixture ? image : NULL,
                                 image, &error));
    uint8_t payload[kSaveCheckpointPayloadMax];
    size_t size = 0;
    CHECK(SaveCheckpoint_Read(path, image, payload, sizeof(payload), &size, &error) ==
          kSaveCheckpoint_Ready);
    CHECK(size == 8966);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i)
      hash = (hash ^ payload[i]) * UINT64_C(1099511628211);
    CHECK(hash == expected[fixture]);
    CHECK(ArRegionalSession_Load(&loaded, 0, path, image, &error) == kSaveCheckpoint_Ready);
    CHECK(!memcmp(&loaded.requested, &session.requested, sizeof(session.requested)));
    CHECK(!memcmp(&loaded.effective, &session.effective, sizeof(session.effective)));
  }
  remove(path);
  remove(companion);
}

int RegionalSessionTest_RunCodec(void) {
  s_failures = 0;
  CheckRandomizerRecipe();
  CheckCodecGolden();
  CheckFeatureCodec();
  return s_failures;
}
