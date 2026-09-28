/* Native/INI checkpoint rotation, interrupted writes and cold-load recovery. */
#define _POSIX_C_SOURCE 200809L
#include "support/regional_save_fixture.h"
#include "regional_session_test.h"
#include "support/test_check.h"

#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(path) _mkdir(path)
#define REMOVE_DIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MAKE_DIR(path) mkdir(path, 0700)
#define REMOVE_DIR(path) rmdir(path)
#endif

static int s_failures;
#define CHECK(condition) AR_TEST_CHECK(s_failures, condition)

static size_t ReadBytes(const char *path, uint8_t *bytes, size_t capacity) {
  FILE *file = fopen(path, "rb");
  CHECK(file);
  if (!file) return 0;
  size_t size = fread(bytes, 1, capacity, file);
  CHECK(!ferror(file));
  CHECK(fgetc(file) == EOF);
  CHECK(!fclose(file));
  return size;
}

static void CheckDisk(SaveFileFormat format, const char *path, const uint8_t *expected) {
  uint8_t actual[kActRaiserSramSize];
  SaveError error;
  CHECK(Save_LoadFile(format, path, actual, &error));
  CHECK(!memcmp(actual, expected, sizeof(actual)));
}

static void CheckPayloadBoundary(void) {
  const char *path = "actraiser-checkpoint-boundary-test.srm";
  const char *companion = "actraiser-checkpoint-boundary-test.srm.archeckpoint";
  remove(path);
  remove(companion);
  uint8_t a[kActRaiserSramSize], b[kActRaiserSramSize];
  uint8_t payload[kSaveCheckpointPayloadMax + 1], out[kSaveCheckpointPayloadMax];
  RegionalSessionTest_MakeImage(a, 1);
  RegionalSessionTest_MakeImage(b, 2);
  for (size_t i = 0; i < sizeof(payload); ++i)
    payload[i] = (uint8_t)(i * 37u);
  SaveError error = {{0}};
  size_t size = 123;
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, NULL, a, payload,
                              kSaveCheckpointPayloadMax, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, a, b, payload,
                              kSaveCheckpointPayloadMax, RegionalSessionTest_AcceptOpaque, NULL,
                              &error));
  /* Both maximum-size retained records fit; an oversized candidate cannot
   * change the native image or the recoverable journal. */
  CHECK(!SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, b, a, payload, sizeof(payload),
                               RegionalSessionTest_AcceptOpaque, NULL, &error));
  CheckDisk(kSaveFileFormat_NativeSrm, path, b);
  for (unsigned i = 0; i < 2; ++i) {
    CHECK(SaveCheckpoint_Read(path, i ? a : b, out, sizeof(out), &size, &error) ==
          kSaveCheckpoint_Ready);
    CHECK(size == kSaveCheckpointPayloadMax && !memcmp(payload, out, size));
  }
  memset(out, 0xa5, sizeof(out));
  size = 123;
  CHECK(SaveCheckpoint_Read(path, b, out, sizeof(out) - 1, &size, &error) ==
        kSaveCheckpoint_Invalid);
  CHECK(size == 123);
  for (size_t i = 0; i < sizeof(out); ++i)
    CHECK(out[i] == 0xa5);
  remove(path);
  remove(companion);
}

static void CheckPersistence(SaveFileFormat format, const char *path) {
  char companion[128], companion_tmp[132], native_tmp[128];
  snprintf(companion, sizeof(companion), "%s.archeckpoint", path);
  snprintf(companion_tmp, sizeof(companion_tmp), "%s.tmp", companion);
  snprintf(native_tmp, sizeof(native_tmp), "%s.tmp", path);
  remove(path);
  remove(companion);
  remove(companion_tmp);
  remove(native_tmp);
  uint8_t a[kActRaiserSramSize], b[kActRaiserSramSize], c[kActRaiserSramSize];
  RegionalSessionTest_MakeImage(a, 1);
  RegionalSessionTest_MakeImage(b, 2);
  RegionalSessionTest_MakeImage(c, 3);
  const uint8_t first_id[16] = {1, 2, 3}, next_id[16] = {4, 5, 6};
  ArRegionalCostPolicy defaults;
  CHECK(ArRegionalCosts_Init(&defaults, kArRegionalSource_US));
  ArRegionalSession session, loaded;
  CHECK(ArRegionalSession_NewGame(&session, 0, first_id, &defaults));
  CHECK(ArRegionalLairHistory_InitTown(&session.lairs, 0));
  CHECK(ArRegionalLairHistory_KillAttempt(&session.lairs, 0));
  CHECK(ArRegionalLairHistory_AdoptTown(&session.lairs, 5, kArRegionalSource_US,
                                        (uint16_t[4]){20, 0, 301, 65535}));
  loaded = session;
  SaveError error;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Missing);
  CHECK(RegionalSessionTest_Equal(&loaded, &session));
  /* Companion failure cannot create or replace the native file. */
  CHECK(!MAKE_DIR(companion_tmp));
  CHECK(!TestRegional_Save(&session, format, path, NULL, a, &error));
  FILE *probe = fopen(path, "rb");
  CHECK(!probe);
  if (probe) fclose(probe);
  CHECK(!REMOVE_DIR(companion_tmp));
  /* Crash window for the very first save: journal exists but native replace
   * failed. The exact retry is allowed without losing either payload. */
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(!TestRegional_Save(&session, format, path, NULL, a, &error));
  CHECK(!REMOVE_DIR(native_tmp));
  CHECK(TestRegional_Save(&session, format, path, NULL, a, &error));
  CheckDisk(format, path, a);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &session));
  CHECK(ArRegionalSession_Load(&loaded, 1, path, a, &error) == kSaveCheckpoint_Mismatch);
  CHECK(RegionalSessionTest_Equal(&loaded, &session));

  /* Save a pending choice without modifying native SRAM. The blocked native
   * temporary path proves this path needs only an atomic companion replace. */
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Miracles,
                                       kArRegionalSource_Japan));
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(TestRegional_Save(&session, format, path, a, a, &error));
  CHECK(!REMOVE_DIR(native_tmp));
  CheckDisk(format, path, a);
  ArRegionalSession saved_a = session;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &saved_a));

  ArRegionalCostSnapshot quote;
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Miracles, &quote));
  CHECK(quote.price[kArRegionalCost_Lightning] == 12);
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Miracles,
                                       kArRegionalSource_US));
  ArRegionalSession saved_b = session;
  /* Failure AFTER journaling must cold-load the OLD image's metadata. */
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(!TestRegional_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, a);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &saved_a));
  CHECK(!REMOVE_DIR(native_tmp));
  CHECK(TestRegional_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, b);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &saved_b));
  /* Quitting an unsaved New Game leaves the saved campaign untouched. */
  CHECK(ArRegionalSession_NewGame(&session, 0, next_id, &defaults));
  CheckDisk(format, path, b);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &saved_b));
  CHECK(TestRegional_Save(&session, format, path, b, c, &error));
  CheckDisk(format, path, c);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &session));
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Scrolls,
                                       kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTimers(&session, session.revision, kArRegionalSource_Japan));
  CHECK(TestRegional_Save(&session, format, path, c, c, &error));
  /* A metadata-only edit must not discard the preceding native checkpoint. */
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &saved_b));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Mismatch);
  CHECK(RegionalSessionTest_Equal(&loaded, &saved_b));

  /* Same name/checksum is insufficient: compare bytes beyond the retail sum. */
  memcpy(a, c, sizeof(a));
  a[0x1ff8] = 42;
  CHECK(Save_ChecksumValid(a) && Save_ComputeChecksum(a) == Save_ComputeChecksum(c));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Mismatch);

  /* Two retained native images/payloads plus the small journal envelope.
   * Do not size this fixture around today's regional payload length. */
  enum { journal_capacity =
      64 + 2 * (kActRaiserSramSize + kSaveCheckpointPayloadMax + kSaveNameCapacity) };
  uint8_t journal[journal_capacity], after[journal_capacity];
  size_t journal_size = ReadBytes(companion, journal, sizeof(journal));
  CHECK(journal_size > 8192);
  /* External save replacement cannot be overwritten using stale session data. */
  CHECK(Save_WriteFile(format, path, a, &error));
  CHECK(!TestRegional_Save(&session, format, path, c, b, &error));
  CheckDisk(format, path, a);
  CHECK(ReadBytes(companion, after, sizeof(after)) == journal_size);
  CHECK(!memcmp(journal, after, journal_size));
  CHECK(Save_WriteFile(format, path, c, &error));

  /* Future outer schema, corruption and truncation are NOT legacy Missing. */
  memcpy(after, journal, journal_size);
  after[8] = 3;
  CHECK(Save_WriteCompanionFile(companion, after, journal_size, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Unsupported);
  CHECK(!TestRegional_Save(&session, format, path, c, c, &error));
  CheckDisk(format, path, c);
  CHECK(ReadBytes(companion, after, sizeof(after)) == journal_size && after[8] == 3);
  memcpy(after, journal, journal_size);
  after[100] ^= 1;
  CHECK(Save_WriteCompanionFile(companion, after, journal_size, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Invalid);
  const size_t cuts[] = {1, 7, 12, 8192};
  for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
    CHECK(Save_WriteCompanionFile(companion, journal, cuts[i], &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Invalid);
    CHECK(!TestRegional_Save(&session, format, path, c, c, &error));
  }
  CHECK(Save_WriteCompanionFile(companion, journal, journal_size, &error));

  /* Future feature schema inside a valid storage envelope also stays intact. */
  uint8_t payload[kSaveCheckpointPayloadMax], future[kSaveCheckpointPayloadMax];
  size_t payload_size = 0;
  CHECK(SaveCheckpoint_Read(path, c, payload, sizeof(payload), &payload_size, &error) ==
        kSaveCheckpoint_Ready);
  memcpy(future, payload, payload_size);
  future[8] = 255;
  CHECK(SaveCheckpoint_Commit(format, path, c, c, future, payload_size,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Unsupported);
  CHECK(!TestRegional_Save(&session, format, path, c, c, &error));
  /* Even a nonmatching retained payload cannot be silently erased on rotation. */
  CHECK(SaveCheckpoint_Commit(format, path, c, a, payload, payload_size,
                              RegionalSessionTest_AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  journal_size = ReadBytes(companion, journal, sizeof(journal));
  CHECK(!TestRegional_Save(&session, format, path, a, b, &error));
  CHECK(ReadBytes(companion, after, sizeof(after)) == journal_size);
  CHECK(!memcmp(journal, after, journal_size));
  CheckDisk(format, path, a);

  remove(path);
  remove(companion);
  remove(companion_tmp);
  remove(native_tmp);
  /* A new campaign may replace a pre-feature save with no companion. Failed
   * replacement must keep the old image recognizably legacy, not corrupt. */
  CHECK(Save_WriteFile(format, path, a, &error));
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(!TestRegional_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, a);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Missing);
  CHECK(!REMOVE_DIR(native_tmp));
  CHECK(TestRegional_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, b);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(RegionalSessionTest_Equal(&loaded, &session));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Missing);
  remove(path);
  remove(companion);
  remove(companion_tmp);
  remove(native_tmp);
}

int RegionalSessionTest_RunPersistence(void) {
  s_failures = 0;
  CheckPayloadBoundary();
  CheckPersistence(kSaveFileFormat_NativeSrm, "actraiser-regional-session-test.srm");
  CheckPersistence(kSaveFileFormat_Ini, "actraiser-regional-session-test.ini");
  return s_failures;
}
