#define _POSIX_C_SOURCE 200809L
#include "regional/regional_session.h"
#include "byte_order.h"

#include <stdio.h>
#include <stdlib.h>
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

static int failures;
#define CHECK(c) do { if (!(c)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

static void Image(uint8_t *image, unsigned marker) {
  memset(image, 0, kActRaiserSramSize);
  image[0x100] = (uint8_t)marker;
  memcpy(image + 0x1439, "MASTER", 6);
  Save_RecomputeChecksum(image);
}

static bool EqualSession(const ArRegionalSession *a, const ArRegionalSession *b) {
  return a->slot == b->slot && a->revision == b->revision &&
      !memcmp(a->campaign, b->campaign, sizeof(a->campaign)) &&
      !memcmp(&a->requested.costs, &b->requested.costs, sizeof(a->requested.costs)) &&
      !memcmp(&a->effective.costs, &b->effective.costs, sizeof(a->effective.costs)) &&
      !memcmp(&a->requested.timers, &b->requested.timers, sizeof(a->requested.timers)) &&
      !memcmp(&a->effective.timers, &b->effective.timers, sizeof(a->effective.timers)) &&
      a->requested.retry_score == b->requested.retry_score &&
      a->effective.retry_score == b->effective.retry_score &&
      a->requested.town_wait == b->requested.town_wait && a->effective.town_wait == b->effective.town_wait &&
      a->requested.fishing == b->requested.fishing && a->effective.fishing == b->effective.fishing &&
      !memcmp(&a->requested.development,&b->requested.development,sizeof(a->requested.development)) &&
      !memcmp(&a->effective.development,&b->effective.development,sizeof(a->effective.development)) &&
      !memcmp(&a->requested.recovery, &b->requested.recovery, sizeof(a->requested.recovery)) &&
      !memcmp(&a->effective.recovery, &b->effective.recovery, sizeof(a->effective.recovery)) &&
      !memcmp(&a->requested.quake, &b->requested.quake, sizeof(a->requested.quake)) &&
      !memcmp(&a->effective.quake, &b->effective.quake, sizeof(a->effective.quake)) &&
      a->requested.score_page == b->requested.score_page && a->effective.score_page == b->effective.score_page &&
      a->requested.menu_return == b->requested.menu_return && a->effective.menu_return == b->effective.menu_return &&
      a->requested.speed_range == b->requested.speed_range && a->effective.speed_range == b->effective.speed_range &&
      a->requested.magic_gesture == b->requested.magic_gesture && a->effective.magic_gesture == b->effective.magic_gesture;
}

static void CheckQuakeActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {42};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_RequestQuake(&session, 2, kArRegionalSource_Japan) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestQuake(&session, 1, kArRegionalSource_Count) && EqualSession(&before, &session));
  CHECK(ArRegionalSession_RequestQuake(&session, 1, kArRegionalSource_Japan));
  CHECK(session.effective.quake.source[0] == kArRegionalSource_US);
  ArRegionalQuakeSnapshot snapshot;
  CHECK(ArRegionalSession_BeginQuake(&session, &snapshot));
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i) CHECK(snapshot.random[i]);
  before = session;
  CHECK(ArRegionalSession_BeginQuake(&session, &snapshot) && EqualSession(&before, &session));
  session.requested.quake.source[0] = kArRegionalSource_Europe;
  CHECK(ArRegionalSession_BeginQuake(&session, &snapshot) && !snapshot.random[0] && snapshot.random[1]);
  ArRegionalSource source = kArRegionalSource_Count;
  CHECK(!ArRegionalQuake_GroupSource(&session.effective.quake, &source) && source == kArRegionalSource_Count);
  CHECK(ArRegionalSession_RequestQuake(&session, session.revision, kArRegionalSource_US));
  session.revision = UINT32_MAX; before = session;
  CHECK(!ArRegionalSession_BeginQuake(&session, &snapshot) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestQuake(&session, UINT32_MAX, kArRegionalSource_Japan) && EqualSession(&before, &session));
}

static void CheckScorePageActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {43};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  bool enabled = false;
  CHECK(ArRegionalSession_BeginScorePage(&session, &enabled) && enabled && session.revision == 1);
  CHECK(!ArRegionalSession_RequestScorePage(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestScorePage(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestScorePage(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginScorePage(&session, &enabled) && enabled == (source != kArRegionalSource_Japan));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginScorePage(&session, &enabled) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestScorePage(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX; ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginScorePage(&session, &enabled) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestScorePage(&session, UINT32_MAX, kArRegionalSource_US) && EqualSession(&before, &session));
  CHECK(!ArRegionalScorePage_Resolve(kArRegionalSource_Count, &enabled) && enabled);
  CHECK(!ArRegionalScorePage_Resolve(kArRegionalSource_US, NULL));
}

static void CheckMenuReturnActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {43};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  bool enabled = false;
  CHECK(ArRegionalSession_BeginMenuReturn(&session, &enabled) && !enabled && session.revision == 1);
  CHECK(!ArRegionalSession_RequestMenuReturn(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestMenuReturn(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestMenuReturn(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginMenuReturn(&session, &enabled) && enabled == (source == kArRegionalSource_Japan));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginMenuReturn(&session, &enabled) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestMenuReturn(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX; ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginMenuReturn(&session, &enabled) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestMenuReturn(&session, UINT32_MAX, kArRegionalSource_US) && EqualSession(&before, &session));
  CHECK(!ArRegionalMenuReturn_Resolve(kArRegionalSource_Count, &enabled) && !enabled);
  CHECK(!ArRegionalMenuReturn_Resolve(kArRegionalSource_US, NULL));
}

static void CheckMagicGestureActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16]={42};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs,kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session,0,id,&costs));
  bool gesture=true;
  CHECK(ArRegionalSession_BeginMagicGesture(&session,false,&gesture) && !gesture && session.revision==1);
  CHECK(!ArRegionalSession_RequestMagicGesture(&session,2,kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestMagicGesture(&session,1,kArRegionalSource_Count));
  for (unsigned source=0;source<kArRegionalSource_Count;++source) {
    const bool previous=gesture;
    CHECK(ArRegionalSession_RequestMagicGesture(&session,session.revision,(ArRegionalSource)source));
    const ArRegionalSession pending=session;
    CHECK(ArRegionalSession_BeginMagicGesture(&session,false,&gesture) && gesture==previous && EqualSession(&session,&pending));
    CHECK(ArRegionalSession_BeginMagicGesture(&session,true,&gesture) && gesture==(source==kArRegionalSource_Japan));
    const uint32_t revision=session.revision;
    CHECK(ArRegionalSession_BeginMagicGesture(&session,true,&gesture) && session.revision==revision);
  }
  CHECK(ArRegionalSession_RequestMagicGesture(&session,session.revision,kArRegionalSource_Japan));
  session.revision=UINT32_MAX; const ArRegionalSession before=session;
  CHECK(ArRegionalSession_BeginMagicGesture(&session,false,&gesture) && !gesture && EqualSession(&before,&session));
  CHECK(!ArRegionalSession_BeginMagicGesture(&session,true,&gesture) && !gesture && EqualSession(&before,&session));
  CHECK(!ArRegionalSession_RequestMagicGesture(&session,UINT32_MAX,kArRegionalSource_US));
  CHECK(!ArRegionalMagicGesture_Resolve(kArRegionalSource_Count,&gesture) && !gesture);
  CHECK(!ArRegionalMagicGesture_Resolve(kArRegionalSource_US,NULL));
}

static void CheckSpeedRangeActivation(void) {
  ArRegionalSession session;
  const uint8_t id[16] = {43};
  ArRegionalCostPolicy costs; ArRegionalCosts_Init(&costs, kArRegionalSource_US);
  CHECK(ArRegionalSession_NewGame(&session, 0, id, &costs));
  uint16_t enabled = 9;
  CHECK(ArRegionalSession_BeginSpeedRange(&session, &enabled) && enabled == 9 && session.revision == 1);
  CHECK(!ArRegionalSession_RequestSpeedRange(&session, 2, kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestSpeedRange(&session, 1, kArRegionalSource_Count));
  for (unsigned source = 0; source < kArRegionalSource_Count; ++source) {
    const ArRegionalRules before = session.effective;
    CHECK(ArRegionalSession_RequestSpeedRange(&session, session.revision, (ArRegionalSource)source));
    CHECK(!memcmp(&before, &session.effective, sizeof(before)));
    CHECK(ArRegionalSession_BeginSpeedRange(&session, &enabled) && enabled == (source == kArRegionalSource_Japan ? 7 : 9));
    const unsigned revision = session.revision;
    CHECK(ArRegionalSession_BeginSpeedRange(&session, &enabled) && session.revision == revision);
  }
  CHECK(ArRegionalSession_RequestSpeedRange(&session, session.revision, kArRegionalSource_Japan));
  session.revision = UINT32_MAX; ArRegionalSession before = session;
  CHECK(!ArRegionalSession_BeginSpeedRange(&session, &enabled) && EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestSpeedRange(&session, UINT32_MAX, kArRegionalSource_US) && EqualSession(&before, &session));
  CHECK(!ArRegionalSpeedRange_Resolve(kArRegionalSource_Count, &enabled) && enabled == 9);
  CHECK(!ArRegionalSpeedRange_Resolve(kArRegionalSource_US, NULL));
}

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

static SaveCheckpointStatus AcceptOpaque(const uint8_t *bytes, size_t size, void *context) {
  (void)context;
  return bytes && size ? kSaveCheckpoint_Ready : kSaveCheckpoint_Invalid;
}

static void CheckSessionState(void) {
  const uint8_t id[16] = {1}, zero[16] = {0};
  ArRegionalCostPolicy defaults;
  CHECK(ArRegionalCosts_Init(&defaults, kArRegionalSource_US));
  ArRegionalSession session;
  CHECK(ArRegionalSession_NewGame(&session, 7, id, &defaults));
  ArRegionalSession before = session;
  CHECK(!ArRegionalSession_NewGame(&session, 7, zero, &defaults));
  CHECK(EqualSession(&before, &session));
  CHECK(ArRegionalSession_RequestCosts(&session, 1, kArRegionalCostGroup_Miracles, kArRegionalSource_Japan));
  CHECK(session.revision == 2 && session.effective.costs.source[kArRegionalCost_Rain] == kArRegionalSource_US);
  CHECK(!ArRegionalSession_RequestCosts(&session, 1, kArRegionalCostGroup_Miracles, kArRegionalSource_US));
  CHECK(session.revision == 2);
  ArRegionalCostSnapshot quote;
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Scrolls, &quote));
  CHECK(session.revision == 2 && quote.price[kArRegionalCost_Rain] == 20);
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Miracles, &quote));
  CHECK(session.revision == 3 && quote.price[kArRegionalCost_Rain] == 16);
  CHECK(ArRegionalSession_RequestCosts(&session, 3, kArRegionalCostGroup_Miracles, kArRegionalSource_US));
  CHECK(quote.price[kArRegionalCost_Rain] == 16); /* An accepted quote stays frozen. */
  CHECK(session.effective.costs.source[kArRegionalCost_Rain] == kArRegionalSource_Japan);
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Miracles, &quote));
  CHECK(quote.price[kArRegionalCost_Rain] == 20 && session.revision == 5);
  session.revision = UINT32_MAX;
  before = session;
  CHECK(!ArRegionalSession_RequestCosts(&session, UINT32_MAX, kArRegionalCostGroup_Scrolls, kArRegionalSource_Japan));
  CHECK(EqualSession(&before, &session));
  CHECK(ArRegionalSession_RequestCosts(&session, UINT32_MAX, kArRegionalCostGroup_Scrolls, kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Scrolls, &quote));
  CHECK(EqualSession(&before, &session));
  session.requested.costs.source[kArRegionalCost_Light] = kArRegionalSource_Japan;
  before = session;
  ArRegionalCostSnapshot old_quote = quote;
  CHECK(!ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Scrolls, &quote));
  CHECK(EqualSession(&before, &session));
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
  CHECK(EqualSession(&before, &session));
  CHECK(ArRegionalSession_RequestTownWait(&session, UINT32_MAX, kArRegionalSource_Europe));
  CHECK(EqualSession(&before, &session));
  CHECK(!ArRegionalSession_RequestTownWait(&session, UINT32_MAX, kArRegionalSource_US));
  CHECK(ArRegionalSession_NewGame(&session,7,id,&defaults));
  bool reconcile = true;
  CHECK(ArRegionalSession_RequestFishing(&session,session.revision,kArRegionalSource_Europe));
  CHECK(ArRegionalSession_BeginFishing(&session,&wait,&reconcile) && wait==255 && !reconcile);
  CHECK(ArRegionalSession_RequestFishing(&session,session.revision,kArRegionalSource_Japan));
  CHECK(ArRegionalSession_BeginFishing(&session,&wait,&reconcile) && wait==128 && reconcile);
  CHECK(ArRegionalSession_BeginFishing(&session,&wait,&reconcile) && wait==128 && !reconcile);
  CHECK(ArRegionalSession_RequestFishing(&session,session.revision,kArRegionalSource_US));
  session.revision=UINT32_MAX; before=session;
  CHECK(!ArRegionalSession_BeginFishing(&session,&wait,&reconcile) && wait==128 && !reconcile);
  CHECK(EqualSession(&session,&before));
  CHECK(!ArRegionalSession_RequestFishing(&session,UINT32_MAX,kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestFishing(&session,UINT32_MAX,kArRegionalSource_Count));
  CHECK(ArRegionalSession_NewGame(&session,7,id,&defaults));
  ArRegionalDevelopmentSnapshot development={0};
  CHECK(ArRegionalSession_RequestDevelopment(&session,1,kArRegionalSource_Japan));
  CHECK(!ArRegionalSession_RequestDevelopment(&session,1,kArRegionalSource_US));
  CHECK(ArRegionalSession_BeginDevelopment(&session,&development) && development.service_divider==5 && development.long_cycle==480);
  CHECK(ArRegionalSession_RequestDevelopment(&session,session.revision,kArRegionalSource_US));
  session.revision=UINT32_MAX;before=session;
  CHECK(!ArRegionalSession_BeginDevelopment(&session,&development) && development.long_cycle==480);
  CHECK(EqualSession(&session,&before));
  CHECK(!ArRegionalSession_RequestDevelopment(&session,UINT32_MAX,kArRegionalSource_Japan));
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
  session.requested.recovery.source[kArRegionalRecovery_SP] = kArRegionalSource_US;
  CHECK(ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && changed == 1);
  CHECK(recovery.cycle_sp && recovery.angel_calls == 60);
  CHECK(ArRegionalSession_RequestRecovery(&session, session.revision, kArRegionalSource_US));
  session.revision = UINT32_MAX; before = session;
  CHECK(!ArRegionalSession_BeginRecovery(&session, &recovery, &changed) && changed == 1);
  CHECK(EqualSession(&session, &before));
}

static void CheckPersistence(SaveFileFormat format, const char *path) {
  char companion[128], companion_tmp[132], native_tmp[128];
  snprintf(companion, sizeof(companion), "%s.archeckpoint", path);
  snprintf(companion_tmp, sizeof(companion_tmp), "%s.tmp", companion);
  snprintf(native_tmp, sizeof(native_tmp), "%s.tmp", path);
  remove(path); remove(companion); remove(companion_tmp); remove(native_tmp);
  uint8_t a[kActRaiserSramSize], b[kActRaiserSramSize], c[kActRaiserSramSize];
  Image(a, 1); Image(b, 2); Image(c, 3);
  const uint8_t first_id[16] = {1, 2, 3}, next_id[16] = {4, 5, 6};
  ArRegionalCostPolicy defaults;
  CHECK(ArRegionalCosts_Init(&defaults, kArRegionalSource_US));
  ArRegionalSession session, loaded;
  CHECK(ArRegionalSession_NewGame(&session, 0, first_id, &defaults));
  loaded = session;
  SaveError error;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Missing);
  CHECK(EqualSession(&loaded, &session));
  /* Companion failure cannot create or replace the native file. */
  CHECK(!MAKE_DIR(companion_tmp));
  CHECK(!ArRegionalSession_Save(&session, format, path, NULL, a, &error));
  FILE *probe = fopen(path, "rb"); CHECK(!probe); if (probe) fclose(probe);
  CHECK(!REMOVE_DIR(companion_tmp));
  /* Crash window for the very first save: journal exists but native replace
   * failed. The exact retry is allowed without losing either payload. */
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(!ArRegionalSession_Save(&session, format, path, NULL, a, &error));
  CHECK(!REMOVE_DIR(native_tmp));
  CHECK(ArRegionalSession_Save(&session, format, path, NULL, a, &error));
  CheckDisk(format, path, a);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));
  CHECK(ArRegionalSession_Load(&loaded, 1, path, a, &error) == kSaveCheckpoint_Mismatch);
  CHECK(EqualSession(&loaded, &session));

  /* Save a pending choice without modifying native SRAM. The blocked native
   * temporary path proves this path needs only an atomic companion replace. */
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Miracles, kArRegionalSource_Japan));
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(ArRegionalSession_Save(&session, format, path, a, a, &error));
  CHECK(!REMOVE_DIR(native_tmp));
  CheckDisk(format, path, a);
  ArRegionalSession saved_a = session;
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_a));

  ArRegionalCostSnapshot quote;
  CHECK(ArRegionalSession_BeginCosts(&session, kArRegionalCostGroup_Miracles, &quote));
  CHECK(quote.price[kArRegionalCost_Lightning] == 12);
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Miracles, kArRegionalSource_US));
  ArRegionalSession saved_b = session;
  /* Failure AFTER journaling must cold-load the OLD image's metadata. */
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(!ArRegionalSession_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, a);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_a));
  CHECK(!REMOVE_DIR(native_tmp));
  CHECK(ArRegionalSession_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, b);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_b));
  /* Quitting an unsaved New Game leaves the saved campaign untouched. */
  CHECK(ArRegionalSession_NewGame(&session, 0, next_id, &defaults));
  CheckDisk(format, path, b);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_b));
  CHECK(ArRegionalSession_Save(&session, format, path, b, c, &error));
  CheckDisk(format, path, c);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Scrolls, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTimers(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_Save(&session, format, path, c, c, &error));
  /* A metadata-only edit must not discard the preceding native checkpoint. */
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &saved_b));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Mismatch);
  CHECK(EqualSession(&loaded, &saved_b));

  /* Same name/checksum is insufficient: compare bytes beyond the retail sum. */
  memcpy(a, c, sizeof(a)); a[0x1ff8] = 42;
  CHECK(Save_ChecksumValid(a) && Save_ComputeChecksum(a) == Save_ComputeChecksum(c));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Mismatch);

  uint8_t journal[26000], after[26000];
  size_t journal_size = ReadBytes(companion, journal, sizeof(journal));
  CHECK(journal_size > 8192);
  /* External save replacement cannot be overwritten using stale session data. */
  CHECK(Save_WriteFile(format, path, a, &error));
  CHECK(!ArRegionalSession_Save(&session, format, path, c, b, &error));
  CheckDisk(format, path, a);
  CHECK(ReadBytes(companion, after, sizeof(after)) == journal_size);
  CHECK(!memcmp(journal, after, journal_size));
  CHECK(Save_WriteFile(format, path, c, &error));

  /* Future outer schema, corruption and truncation are NOT legacy Missing. */
  memcpy(after, journal, journal_size); after[8] = 2;
  CHECK(Save_WriteCompanionFile(companion, after, journal_size, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Unsupported);
  CHECK(!ArRegionalSession_Save(&session, format, path, c, c, &error));
  CheckDisk(format, path, c);
  CHECK(ReadBytes(companion, after, sizeof(after)) == journal_size && after[8] == 2);
  memcpy(after, journal, journal_size); after[100] ^= 1;
  CHECK(Save_WriteCompanionFile(companion, after, journal_size, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Invalid);
  const size_t cuts[] = {1, 7, 12, 8192};
  for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
    CHECK(Save_WriteCompanionFile(companion, journal, cuts[i], &error));
    CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Invalid);
    CHECK(!ArRegionalSession_Save(&session, format, path, c, c, &error));
  }
  CHECK(Save_WriteCompanionFile(companion, journal, journal_size, &error));

  /* Future feature schema inside a valid storage envelope also stays intact. */
  uint8_t payload[kSaveCheckpointPayloadMax], future[kSaveCheckpointPayloadMax];
  size_t payload_size = 0;
  CHECK(SaveCheckpoint_Read(path, c, payload, sizeof(payload), &payload_size, &error) == kSaveCheckpoint_Ready);
  memcpy(future, payload, payload_size); future[8] = 12;
  CHECK(SaveCheckpoint_Commit(format, path, c, c, future, payload_size, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, c, &error) == kSaveCheckpoint_Unsupported);
  CHECK(!ArRegionalSession_Save(&session, format, path, c, c, &error));
  /* Even a nonmatching retained payload cannot be silently erased on rotation. */
  CHECK(SaveCheckpoint_Commit(format, path, c, a, payload, payload_size, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Ready);
  journal_size = ReadBytes(companion, journal, sizeof(journal));
  CHECK(!ArRegionalSession_Save(&session, format, path, a, b, &error));
  CHECK(ReadBytes(companion, after, sizeof(after)) == journal_size);
  CHECK(!memcmp(journal, after, journal_size));
  CheckDisk(format, path, a);

  remove(path); remove(companion); remove(companion_tmp); remove(native_tmp);
  /* A new campaign may replace a pre-feature save with no companion. Failed
   * replacement must keep the old image recognizably legacy, not corrupt. */
  CHECK(Save_WriteFile(format, path, a, &error));
  CHECK(!MAKE_DIR(native_tmp));
  CHECK(!ArRegionalSession_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, a);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Missing);
  CHECK(!REMOVE_DIR(native_tmp));
  CHECK(ArRegionalSession_Save(&session, format, path, a, b, &error));
  CheckDisk(format, path, b);
  CHECK(ArRegionalSession_Load(&loaded, 0, path, b, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));
  CHECK(ArRegionalSession_Load(&loaded, 0, path, a, &error) == kSaveCheckpoint_Missing);
  remove(path); remove(companion); remove(companion_tmp); remove(native_tmp);
}

static void CheckFeatureCodec(void) {
  const char *path = "actraiser-regional-codec-test.srm";
  const char *companion = "actraiser-regional-codec-test.srm.archeckpoint";
  remove(path); remove(companion);
  uint8_t image[kActRaiserSramSize]; Image(image, 7);
  const uint8_t id[16] = {7};
  ArRegionalCostPolicy defaults;
  CHECK(ArRegionalCosts_Init(&defaults, kArRegionalSource_US));
  ArRegionalSession session, loaded;
  CHECK(ArRegionalSession_NewGame(&session, 2, id, &defaults));
  CHECK(ArRegionalSession_RequestCosts(&session, session.revision, kArRegionalCostGroup_Scrolls, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTimers(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestRetryScore(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestTownWait(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestFishing(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestDevelopment(&session,session.revision,kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestRecovery(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestQuake(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestScorePage(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestMenuReturn(&session, session.revision, kArRegionalSource_Japan));
  SaveError error;
  CHECK(ArRegionalSession_RequestSpeedRange(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_RequestMagicGesture(&session, session.revision, kArRegionalSource_Japan));
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, NULL, image, &error));
  uint8_t original[kSaveCheckpointPayloadMax], mutated[kSaveCheckpointPayloadMax];
  size_t size = 0;
  CHECK(SaveCheckpoint_Read(path, image, original, sizeof(original), &size, &error) == kSaveCheckpoint_Ready);
  if (size < 36 || size >= sizeof(original)) return;
  for (unsigned mutation = 0; mutation < 9; ++mutation) {
    memcpy(mutated, original, size);
    size_t bytes = size;
    SaveCheckpointStatus expected = kSaveCheckpoint_Invalid;
    switch (mutation) {
      case 0: mutated[0] ^= 1; break; /* Wrong codec magic. */
      case 1: mutated[8] = 12; expected = kSaveCheckpoint_Unsupported; break;
      case 2: memset(mutated + 16, 0, 16); break; /* Missing campaign identity. */
      case 3: memset(mutated + 32, 0, 4); break; /* Invalid generation. */
      case 4: mutated[37 + mutated[36]] = 'x'; expected = kSaveCheckpoint_Unsupported; break;
      case 5: mutated[37 + mutated[36] + 4] ^= 1; expected = kSaveCheckpoint_Unsupported; break;
      case 6: --bytes; break;
      case 7: mutated[bytes++] = 0; break;
      case 8: { /* Duplicate the first named leaf in place of the second. */
        size_t first = original[36] + 9u, second = original[36 + first] + 9u;
        memcpy(mutated + 36 + first, original + 36, first);
        memcpy(mutated + 36 + first * 2, original + 36 + first + second,
               size - 36 - first - second);
        bytes = size + first - second;
        break;
      }
    }
    CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
                                mutated, bytes, AcceptOpaque, NULL, &error));
    loaded = session;
    CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == expected);
    CHECK(EqualSession(&loaded, &session));
    CHECK(!ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  }
  /* Reordering valid named fields is supported; they are not enum ordinals. */
  const unsigned records = kArRegionalCostRule_Count + kArRegionalTimerRule_Count + 7 + kArRegionalDevelopmentRule_Count + kArRegionalRecovery_Count + kArRegionalQuake_Count;
  CHECK(ByteOrder_ReadLe16(original + 10) == records);
  size_t offsets[kArRegionalCostRule_Count + kArRegionalTimerRule_Count + 7 + kArRegionalDevelopmentRule_Count + kArRegionalRecovery_Count + kArRegionalQuake_Count], offset = 36;
  for (unsigned i = 0; i < records; ++i) {
    offsets[i] = offset;
    offset += original[offset] + 9u;
  }
  CHECK(offset == size);
  memcpy(mutated, original, 36); offset = 36;
  for (unsigned i = records; i-- > 0;) {
    size_t length = original[offsets[i]] + 9u;
    memcpy(mutated + offset, original + offsets[i], length);
    offset += length;
  }
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
                              mutated, offset, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));

  /* Pricing-only checkpoints migrate in memory with US timer defaults, never
   * assuming that previous prices were a full regional preset. The old on-disk
   * record remains recoverable and can be retained beside a new save. */
  const size_t price_bytes = offsets[kArRegionalCostRule_Count];
  memcpy(mutated, original, price_bytes);
  memcpy(mutated, "ARPRICE", 8);
  ByteOrder_WriteLe16(mutated + 8, 1);
  ByteOrder_WriteLe16(mutated + 10, kArRegionalCostRule_Count);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, price_bytes, AcceptOpaque, NULL, &error));
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
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v1_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(!memcmp(&loaded.requested.timers, &session.requested.timers, sizeof(loaded.requested.timers)));
  CHECK(loaded.requested.retry_score == kArRegionalSource_US &&
        loaded.effective.retry_score == kArRegionalSource_US);
  CHECK(loaded.requested.town_wait == kArRegionalSource_US && loaded.effective.town_wait == kArRegionalSource_US);
  /* Retry-era v2 retains its score rule but cannot imply town timing. */
  const size_t v2_bytes = offsets[16];
  memcpy(mutated, original, v2_bytes);
  ByteOrder_WriteLe16(mutated + 8, 2);
  ByteOrder_WriteLe16(mutated + 10, 16);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v2_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.retry_score == kArRegionalSource_Japan);
  CHECK(loaded.requested.town_wait == kArRegionalSource_US && loaded.effective.town_wait == kArRegionalSource_US);
  const size_t v3_bytes = offsets[17];
  memcpy(mutated,original,v3_bytes);
  ByteOrder_WriteLe16(mutated+8,3); ByteOrder_WriteLe16(mutated+10,17);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v3_bytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.town_wait == kArRegionalSource_Japan);
  CHECK(loaded.requested.fishing == kArRegionalSource_US && loaded.effective.fishing == kArRegionalSource_US);
  const size_t v4_bytes=offsets[18];memcpy(mutated,original,v4_bytes);
  ByteOrder_WriteLe16(mutated+8,4);ByteOrder_WriteLe16(mutated+10,18);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v4_bytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.fishing==kArRegionalSource_Japan);
  for(unsigned i=0;i<kArRegionalDevelopmentRule_Count;++i)
    CHECK(loaded.requested.development.source[i]==kArRegionalSource_US && loaded.effective.development.source[i]==kArRegionalSource_US);
  const size_t v5_bytes = offsets[21]; memcpy(mutated, original, v5_bytes);
  ByteOrder_WriteLe16(mutated + 8, 5); ByteOrder_WriteLe16(mutated + 10, 21);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v5_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.development.source[0] == kArRegionalSource_Japan);
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i)
    CHECK(loaded.requested.recovery.source[i] == kArRegionalSource_US &&
          loaded.effective.recovery.source[i] == kArRegionalSource_US);
  const size_t v6_bytes = offsets[23]; memcpy(mutated, original, v6_bytes);
  ByteOrder_WriteLe16(mutated + 8, 6); ByteOrder_WriteLe16(mutated + 10, 23);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v6_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.recovery.source[0] == kArRegionalSource_Japan);
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i)
    CHECK(loaded.requested.quake.source[i] == kArRegionalSource_US &&
          loaded.effective.quake.source[i] == kArRegionalSource_US);
  const size_t v7_bytes = offsets[28]; memcpy(mutated, original, v7_bytes);
  ByteOrder_WriteLe16(mutated + 8, 7); ByteOrder_WriteLe16(mutated + 10, 28);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v7_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.quake.source[0] == kArRegionalSource_Japan);
  CHECK(loaded.requested.score_page == kArRegionalSource_US && loaded.effective.score_page == kArRegionalSource_US);
  const size_t v8_bytes = offsets[29]; memcpy(mutated, original, v8_bytes);
  ByteOrder_WriteLe16(mutated + 8, 8); ByteOrder_WriteLe16(mutated + 10, 29);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v8_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.score_page == kArRegionalSource_Japan);
  CHECK(loaded.requested.menu_return == kArRegionalSource_US && loaded.effective.menu_return == kArRegionalSource_US);
  const size_t v9_bytes = offsets[30]; memcpy(mutated, original, v9_bytes);
  ByteOrder_WriteLe16(mutated + 8, 9); ByteOrder_WriteLe16(mutated + 10, 30);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm, path, image, image,
      mutated, v9_bytes, AcceptOpaque, NULL, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(loaded.requested.menu_return == kArRegionalSource_Japan);
  CHECK(loaded.requested.speed_range == kArRegionalSource_US && loaded.effective.speed_range == kArRegionalSource_US);
  const size_t v10_bytes=offsets[31]; memcpy(mutated,original,v10_bytes);
  ByteOrder_WriteLe16(mutated+8,10); ByteOrder_WriteLe16(mutated+10,31);
  CHECK(SaveCheckpoint_Commit(kSaveFileFormat_NativeSrm,path,image,image,
      mutated,v10_bytes,AcceptOpaque,NULL,&error));
  CHECK(ArRegionalSession_Load(&loaded,2,path,image,&error)==kSaveCheckpoint_Ready);
  CHECK(loaded.requested.speed_range==kArRegionalSource_Japan);
  CHECK(loaded.requested.magic_gesture==kArRegionalSource_US && loaded.effective.magic_gesture==kArRegionalSource_US);
  CHECK(ArRegionalSession_Save(&session, kSaveFileFormat_NativeSrm, path, image, image, &error));
  CHECK(ArRegionalSession_Load(&loaded, 2, path, image, &error) == kSaveCheckpoint_Ready);
  CHECK(EqualSession(&loaded, &session));
  remove(path); remove(companion);
}

int main(void) {
  CheckMagicGestureActivation();
  CheckScorePageActivation();
  CheckMenuReturnActivation();
  CheckSpeedRangeActivation();
  CheckQuakeActivation();
  CheckSessionState();
  CheckPersistence(kSaveFileFormat_NativeSrm, "actraiser-regional-session-test.srm");
  CheckPersistence(kSaveFileFormat_Ini, "actraiser-regional-session-test.ini");
  CheckFeatureCodec();
  if (failures) fprintf(stderr, "%d regional session failures\n", failures);
  else puts("regional sessions: activation, cold load, recovery and native/INI checkpoint tests passed");
  return failures ? 1 : 0;
}
