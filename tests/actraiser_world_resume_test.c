#include "support/test_assert.h"
#include "actraiser/enhancements/actraiser_world_resume.h"
#include "actraiser_game.h"
#include "byte_order.h"
#include "save/save_system.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static uint8_t s_wram[kSnesWramSize], s_before[kSnesWramSize];

static void NativeFocus(void) {
  memset(s_wram, 0, sizeof(s_wram));
  ByteOrder_WriteLe16(s_wram + kActRaiserWram_WorldFocusX, 768);
  ByteOrder_WriteLe16(s_wram + kActRaiserWram_WorldFocusY, 512);
  ByteOrder_WriteLe16(s_wram + kActRaiserWram_WorldZoomTarget, 1034);
}

static void Visit(uint8_t town) {
  s_wram[kActRaiserWram_MapGroup] = kActRaiserMapGroup_NonAction;
  s_wram[kActRaiserWram_CurrentMap] = town;
  ActRaiserWorldResume_Observe(s_wram, sizeof(s_wram), true);
}

static void TestResume(SaveBackend backend) {
  const char *native = "world-resume-test.srm", *ini = "world-resume-test.ini";
  const char *path = backend == kSaveBackend_NativeSrm ? native : ini;
  const SaveFileFormat format = backend == kSaveBackend_NativeSrm
      ? kSaveFileFormat_NativeSrm : kSaveFileFormat_Ini;
  char bookmark[128];
  snprintf(bookmark, sizeof(bookmark), "%s.artown", path);
  remove(bookmark);
  uint8_t live[kActRaiserSramSize] = {0}, original[kActRaiserSramSize], disk[kActRaiserSramSize];
  for (unsigned i = 0; i < 6; ++i) assert(Save_SetRegionState(live, i, 2));
  Save_RecomputeChecksum(live);
  memcpy(original, live, sizeof(live));
  SaveError error = {{0}};
  assert(Save_WriteFile(format, path, live, &error));
  assert(SaveSystem_Attach(live, sizeof(live), backend, native, ini, &error));
  assert(SaveSystem_LoadActive(&error));
  NativeFocus();
  memcpy(s_before, s_wram, sizeof(s_wram));
  ActRaiserWorldResume_Begin(s_wram, sizeof(s_wram), true, true, true);
  assert(!memcmp(s_before, s_wram, sizeof(s_wram))); /* old saves retain Fillmore */

  static const uint16_t centers[6][2] = {
      {768,512}, {512,512}, {256,640}, {256,384}, {640,896}, {384,128}};
  for (uint8_t town = 1; town <= 6; ++town) {
    Visit(town);
    /* Leaving SIM or flying over a different location does not record it. */
    Visit(kActRaiserNonActionMap_SkyPalace);
    Visit(kActRaiserNonActionMap_WorldMap);
    s_wram[kActRaiserWram_WorldLocation] = 7;
    ActRaiserWorldResume_Observe(s_wram, sizeof(s_wram), true);
    assert(SaveSystem_FlushTownVisit(&error));
    assert(SaveSystem_Attach(live, sizeof(live), backend, native, ini, &error));
    assert(SaveSystem_LoadActive(&error));
    NativeFocus();
    ActRaiserWorldResume_Begin(s_wram, sizeof(s_wram), true, true, true);
    assert(ByteOrder_ReadLe16(s_wram + kActRaiserWram_WorldFocusX) == centers[town-1][0]);
    assert(ByteOrder_ReadLe16(s_wram + kActRaiserWram_WorldFocusY) == centers[town-1][1]);
    assert(ByteOrder_ReadLe16(s_wram + kActRaiserWram_Bg1CameraX) == centers[town-1][0]-128);
    assert(ByteOrder_ReadLe16(s_wram + kActRaiserWram_Bg1CameraY) == centers[town-1][1]-112);
    assert(s_wram[kActRaiserWram_WorldLocation] == town);
    assert(ByteOrder_ReadLe16(s_wram + kActRaiserWram_WorldZoomTarget) == 1034);
    assert(!memcmp(original, live, sizeof(live)));
  }
  assert(Save_LoadFile(format, path, disk, &error));
  assert(!memcmp(original, disk, sizeof(disk))); /* visits never save gameplay */

  /* Northwall -> Palace -> Progress Log -> shutdown -> Continue. The bookmark
   * must follow the committed image, not be lost when saving outside SIM. */
  Visit(6);
  Visit(kActRaiserNonActionMap_SkyPalace);
  assert(SaveSystem_BeginNativeWrite(&error));
  live[100] ^= 1;
  Save_RecomputeChecksum(live);
  assert(SaveSystem_EndNativeWrite(true, &error));
  assert(SaveSystem_AutoPersistIfChanged(&error));
  assert(SaveSystem_FlushTownVisit(&error));
  assert(SaveSystem_Attach(live, sizeof(live), backend, native, ini, &error));
  assert(SaveSystem_LoadActive(&error));
  NativeFocus();
  ActRaiserWorldResume_Begin(s_wram, sizeof(s_wram), true, true, true);
  assert(s_wram[kActRaiserWram_WorldLocation] == 6);
  assert(ByteOrder_ReadLe16(s_wram + kActRaiserWram_WorldFocusX) == 384);
  assert(ByteOrder_ReadLe16(s_wram + kActRaiserWram_WorldFocusY) == 128);

  /* Disabled QoL setting and protected replay/recording leave all WRAM alone. */
  for (unsigned mode = 0; mode < 4; ++mode) {
    NativeFocus();
    if (mode == 2) s_wram[kActRaiserWram_WorldEmergenceState] = 1;
    if (mode == 3) s_wram[kActRaiserWram_DeathHeimProgress] = 8;
    memcpy(s_before, s_wram, sizeof(s_wram));
    ActRaiserWorldResume_Begin(s_wram, sizeof(s_wram), true, mode != 0, mode != 1);
    assert(!memcmp(s_before, s_wram, sizeof(s_wram)));
  }
  s_wram[kActRaiserWram_CurrentMap] = 1;
  ActRaiserWorldResume_Observe(s_wram, sizeof(s_wram), false);
  assert(SaveSystem_FlushTownVisit(&error));
  assert(SaveSystem_BeginTownVisits(true) == 6);

  /* Unsaved New Game may not overwrite an occupied campaign's bookmark. */
  NativeFocus();
  memcpy(s_before, s_wram, sizeof(s_wram));
  ActRaiserWorldResume_Begin(s_wram, sizeof(s_wram), false, true, true);
  assert(!memcmp(s_before, s_wram, sizeof(s_wram)));
  Visit(2);
  assert(SaveSystem_FlushTownVisit(&error));
  assert(SaveSystem_BeginTownVisits(true) == 6);
  assert(SaveSystem_BeginTownVisits(false) == 0);
  Visit(2);
  assert(SaveSystem_BeginNativeWrite(&error));
  live[100] ^= 1;
  assert(SaveSystem_FlushTownVisit(&error)); /* half-written SRAM is never bound */
  Save_RecomputeChecksum(live);
  assert(SaveSystem_EndNativeWrite(true, &error));
  assert(SaveSystem_AutoPersistIfChanged(&error));
  assert(SaveSystem_FlushTownVisit(&error));
  assert(SaveSystem_BeginTownVisits(true) == 2);
  /* Rebind on subsequent saves even without another town visit. */
  live[101] ^= 1;
  Save_RecomputeChecksum(live);
  assert(SaveSystem_AutoPersistIfChanged(&error));
  assert(SaveSystem_FlushTownVisit(&error));
  assert(SaveSystem_BeginTownVisits(true) == 2);

  /* Equal-image import is still a different campaign, with no inherited focus. */
  assert(SaveSystem_Import(path, false, &error));
  assert(SaveSystem_FlushTownVisit(&error));
  assert(SaveSystem_BeginTownVisits(true) == 0);
  Visit(3);
  assert(SaveSystem_FlushTownVisit(&error));
  /* External replacement/stale backup invalidates the exact-image binding. */
  assert(Save_WriteFile(format, path, original, &error));
  assert(SaveSystem_LoadActive(&error));
  assert(SaveSystem_BeginTownVisits(true) == 0);
  Visit(3);
  assert(SaveSystem_FlushTownVisit(&error));

  /* Malformed, future, truncated and tampered optional metadata are ignored. */
  uint8_t bytes[25];
  FILE *file = fopen(bookmark, "rb");
  assert(file && fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
  assert(fgetc(file) == EOF && fclose(file) == 0);
  for (unsigned variant = 0; variant < 4; ++variant) {
    uint8_t bad[26];
    memcpy(bad, bytes, sizeof(bytes));
    bad[25] = 0;
    if (variant == 0) bad[6] = '2';
    if (variant == 1) bad[16] = 4;
    assert(Save_WriteCompanionFile(bookmark, bad, variant == 2 ? 24 : variant == 3 ? 26 : 25,
                                   &error));
    assert(SaveSystem_BeginTownVisits(true) == 0);
  }
  assert(Save_WriteCompanionFile(bookmark, bytes, sizeof(bytes), &error));
  assert(SaveSystem_BeginTownVisits(true) == 3);
  /* Only developed towns in the saved campaign can be restored. */
  assert(Save_SetRegionState(live, 2, 0));
  Save_RecomputeChecksum(live);
  assert(SaveSystem_AutoPersistIfChanged(&error));
  assert(SaveSystem_FlushTownVisit(&error));
  assert(SaveSystem_BeginTownVisits(true) == 0);

  /* A different save path/slot has no bookmark, even with identical SRAM. */
  const char *other = "world-resume-other.srm";
  assert(Save_WriteFile(kSaveFileFormat_NativeSrm, other, original, &error));
  assert(SaveSystem_Attach(live, sizeof(live), kSaveBackend_NativeSrm, other, ini, &error));
  assert(SaveSystem_LoadActive(&error));
  assert(SaveSystem_BeginTownVisits(true) == 0);
  remove(other);

  /* Failed optional writes retain a retry; ordinary repeated observations do
   * not dirty a successfully saved bookmark or perform a new write. */
  assert(SaveSystem_Attach(live, sizeof(live), backend, native, ini, &error));
  assert(SaveSystem_LoadActive(&error));
  (void)SaveSystem_BeginTownVisits(true);
  remove(bookmark);
#ifdef _WIN32
  assert(_mkdir(bookmark) == 0);
#else
  assert(mkdir(bookmark, 0700) == 0);
#endif
  Visit(4);
  assert(!SaveSystem_FlushTownVisit(&error));
#ifdef _WIN32
  assert(_rmdir(bookmark) == 0);
#else
  assert(rmdir(bookmark) == 0);
#endif
  assert(SaveSystem_FlushTownVisit(&error));
  assert(SaveSystem_BeginTownVisits(true) == 4);
  assert(remove(bookmark) == 0);
  Visit(4);
  assert(SaveSystem_FlushTownVisit(&error));
  assert(fopen(bookmark, "rb") == NULL);
  remove(native);
  remove(ini);
}

int main(void) {
  TestResume(kSaveBackend_NativeSrm);
  TestResume(kSaveBackend_Ini);
  puts("world resume tests: pass");
  return 0;
}
