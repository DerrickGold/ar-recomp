#define _POSIX_C_SOURCE 200809L

#include "save_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

static int s_failures;

#define CHECK(expr) do { \
  if (!(expr)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", \
            __FILE__, __LINE__, #expr); \
    s_failures++; \
  } \
} while (0)

static void MakeFixture(uint8_t image[kActRaiserSramSize]) {
  static const int states[kActRaiserSaveRegionCount] = {0, 2, 3, 4, 0, 2};
  for (int i = 0; i < kActRaiserSramSize; i++)
    image[i] = (uint8_t)((i * 37 + i / 17 + 0x60) & 0xff);
  for (int i = 0; i < kActRaiserSaveRegionCount; i++)
    CHECK(Save_SetRegionState(image, i, states[i]));
  Save_RecomputeChecksum(image);
}

static bool CopyWithRewrite(const char *source, const char *destination,
                            const char *match, const char *replacement,
                            bool duplicate) {
  FILE *in = fopen(source, "r");
  FILE *out = fopen(destination, "w");
  if (!in || !out) {
    if (in) fclose(in);
    if (out) fclose(out);
    return false;
  }
  char line[1024];
  while (fgets(line, sizeof(line), in)) {
    if (!strncmp(line, match, strlen(match))) {
      if (replacement && fputs(replacement, out) < 0) break;
      if (duplicate && fputs(line, out) < 0) break;
      if (!replacement && !duplicate) continue;
      if (replacement) continue;
    }
    if (fputs(line, out) < 0) break;
  }
  bool ok = !ferror(in) && !ferror(out);
  if (fclose(in) != 0) ok = false;
  if (fclose(out) != 0) ok = false;
  return ok;
}

static void TestChecksumAndFields(void) {
  uint8_t image[kActRaiserSramSize];
  MakeFixture(image);
  CHECK(Save_ChecksumValid(image));
  CHECK(Save_StoredChecksum(image) == Save_ComputeChecksum(image));
  image[0x123] ^= 0x80;
  CHECK(!Save_ChecksumValid(image));
  Save_RecomputeChecksum(image);
  CHECK(Save_ChecksumValid(image));

  for (int i = 0; i < kActRaiserSaveRegionCount; i++) {
    static const int states[kActRaiserSaveRegionCount] = {0, 2, 3, 4, 0, 2};
    int progress = -1;
    CHECK(Save_GetRegionState(image, i, &progress));
    CHECK(progress == states[i]);
  }
  CHECK(!Save_SetRegionState(image, 0, 1));
  CHECK(!Save_GetRegionState(image, -1, &(int){0}));
  int parsed = -1;
  CHECK(Save_ParseRegionState("act2-cleared", &parsed) && parsed == 4);
  CHECK(!Save_ParseRegionState("active", &parsed));

#ifdef ACTRAISER_SOURCE_DIR
  /* A real battery save, as a sanity check that the codec agrees with the
   * game's own writer. These fixtures live in test-saves/ and are gitignored
   * (*.srm is ROM-derived and intentionally untracked), so a fresh clone
   * legitimately has none — that is a skip, not a failure. Only a fixture
   * that exists and fails to load or checksum is a real defect. */
  char fixture_path[1024];
  snprintf(fixture_path, sizeof(fixture_path),
           "%s/test-saves/save.sim-blank.bak.srm", ACTRAISER_SOURCE_DIR);
  FILE *fixture = fopen(fixture_path, "rb");
  if (fixture) {
    fclose(fixture);
    uint8_t real[kActRaiserSramSize];
    SaveError error = {{0}};
    CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, fixture_path, real, &error));
    CHECK(Save_ChecksumValid(real));
  } else {
    printf("save system tests: skipping real-save fixture (%s absent)\n",
           fixture_path);
  }
#endif
}

static void TestNativeAndIniCodecs(void) {
  static const char native_path[] = "actraiser-save-codec-test.srm";
  static const char ini_path[] = "actraiser-save-codec-test.ini";
  static const char edited_path[] = "actraiser-save-codec-edited.ini";
  static const char missing_path[] = "actraiser-save-codec-missing.ini";
  static const char duplicate_path[] = "actraiser-save-codec-duplicate.ini";
  static const char short_path[] = "actraiser-save-codec-short.srm";
  remove(native_path); remove(ini_path); remove(edited_path);
  remove(missing_path); remove(duplicate_path); remove(short_path);

  uint8_t original[kActRaiserSramSize];
  uint8_t decoded[kActRaiserSramSize];
  uint8_t sentinel[kActRaiserSramSize];
  MakeFixture(original);
  memset(sentinel, 0xa5, sizeof(sentinel));
  SaveError error = {{0}};

  CHECK(Save_WriteFile(kSaveFileFormat_NativeSrm, native_path,
                       original, &error));
  memset(decoded, 0, sizeof(decoded));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path,
                      decoded, &error));
  CHECK(!memcmp(decoded, original, sizeof(decoded)));

  CHECK(Save_WriteFile(kSaveFileFormat_Ini, ini_path, original, &error));
  memset(decoded, 0, sizeof(decoded));
  CHECK(Save_LoadFile(kSaveFileFormat_Ini, ini_path, decoded, &error));
  CHECK(!memcmp(decoded, original, sizeof(decoded)));

  CHECK(CopyWithRewrite(ini_path, edited_path, "bloodpool = ",
                        "bloodpool = act2-cleared\n", false));
  CHECK(Save_LoadFile(kSaveFileFormat_Ini, edited_path, decoded, &error));
  for (int i = 0; i < kActRaiserSramSize; i++) {
    bool expected_change = i == g_save_region_fields[1].offset ||
        (i >= kActRaiserSramChecksumOffset &&
         i < kActRaiserSramChecksumOffset + 4);
    if (!expected_change) CHECK(decoded[i] == original[i]);
  }
  int bloodpool_state = -1;
  CHECK(Save_GetRegionState(decoded, 1, &bloodpool_state));
  CHECK(bloodpool_state == kSaveRegionState_Act2Cleared);
  CHECK(Save_ChecksumValid(decoded));

  CHECK(CopyWithRewrite(ini_path, missing_path, "0040 = ", NULL, false));
  memcpy(decoded, sentinel, sizeof(decoded));
  CHECK(!Save_LoadFile(kSaveFileFormat_Ini, missing_path, decoded, &error));
  CHECK(!memcmp(decoded, sentinel, sizeof(decoded)));

  CHECK(CopyWithRewrite(ini_path, duplicate_path, "0000 = ", NULL, true));
  memcpy(decoded, sentinel, sizeof(decoded));
  CHECK(!Save_LoadFile(kSaveFileFormat_Ini, duplicate_path, decoded, &error));
  CHECK(!memcmp(decoded, sentinel, sizeof(decoded)));

  FILE *short_file = fopen(short_path, "wb");
  CHECK(short_file != NULL);
  if (short_file) {
    CHECK(fwrite(original, 1, sizeof(original) - 1, short_file) ==
          sizeof(original) - 1);
    fclose(short_file);
  }
  memcpy(decoded, sentinel, sizeof(decoded));
  CHECK(!Save_LoadFile(kSaveFileFormat_NativeSrm, short_path,
                       decoded, &error));
  CHECK(!memcmp(decoded, sentinel, sizeof(decoded)));

  remove(native_path); remove(ini_path); remove(edited_path);
  remove(missing_path); remove(duplicate_path); remove(short_path);
}

static void TestLegacyMigration(void) {
  static const char legacy_path[] = "actraiser-save-legacy-test.srm";
  static const char native_path[] = "actraiser-save-migrated-test.srm";
  static const char ini_path[] = "actraiser-save-migrated-test.ini";
  remove(legacy_path); remove(native_path); remove(ini_path);

  uint8_t original[kActRaiserSramSize];
  uint8_t live[kActRaiserSramSize];
  uint8_t disk[kActRaiserSramSize];
  SaveError error = {{0}};
  MakeFixture(original);
  CHECK(Save_WriteFile(kSaveFileFormat_NativeSrm, legacy_path,
                       original, &error));

  memset(live, 0x60, sizeof(live));
  CHECK(SaveSystem_Attach(live, sizeof(live), kSaveBackend_NativeSrm,
                          native_path, ini_path, &error));
  CHECK(SaveSystem_MigrateLegacyNative(legacy_path, &error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path, disk, &error));
  CHECK(!memcmp(original, disk, sizeof(disk)));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, legacy_path, disk, &error));

  /* A selected INI backend converts the legacy native image rather than
   * populating an inactive native path. */
  remove(native_path); remove(ini_path);
  CHECK(SaveSystem_Attach(live, sizeof(live), kSaveBackend_Ini,
                          native_path, ini_path, &error));
  CHECK(SaveSystem_MigrateLegacyNative(legacy_path, &error));
  CHECK(Save_LoadFile(kSaveFileFormat_Ini, ini_path, disk, &error));
  CHECK(!memcmp(original, disk, sizeof(disk)));

  remove(legacy_path); remove(native_path); remove(ini_path);
  remove("actraiser-save-migrated-test.srm.tmp");
  remove("actraiser-save-migrated-test.ini.tmp");
}

static void TestRuntimeTransactions(void) {
  static const char native_path[] = "actraiser-save-active-test.srm";
  static const char ini_path[] = "actraiser-save-active-test.ini";
  static const char import_path[] = "actraiser-save-import-test.srm";
  static const char export_path[] = "actraiser-save-export-test.ini";
  remove(native_path); remove(ini_path); remove(import_path);
  remove(export_path);

  uint8_t original[kActRaiserSramSize];
  uint8_t live[kActRaiserSramSize];
  uint8_t disk[kActRaiserSramSize];
  MakeFixture(original);
  SaveError error = {{0}};
  CHECK(Save_WriteFile(kSaveFileFormat_NativeSrm, native_path,
                       original, &error));
  memset(live, 0x60, sizeof(live));
  CHECK(SaveSystem_Attach(live, sizeof(live), kSaveBackend_NativeSrm,
                          native_path, ini_path, &error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(!memcmp(live, original, sizeof(live)));

  CHECK(Save_SetRegionState(live, 0, 4));
  Save_RecomputeChecksum(live);
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path, disk, &error));
  CHECK(!memcmp(live, disk, sizeof(live)));

  /* A bridge sidecar mutation is session-only until the ROM performs a real
   * save, represented here by a native town-block change. Range resync must
   * hide only the sidecar/checksum; the later town change commits everything. */
  memcpy(live + 0x1d70, "AXB1", 4);
  Save_RecomputeChecksum(live);
  SaveSystem_ResyncShadowRange(0x1d70, 4);
  SaveSystem_ResyncShadowRange(kActRaiserSramChecksumOffset, 4);
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path, disk, &error));
  CHECK(memcmp(disk + 0x1d70, "AXB1", 4) != 0);
  live[0x0600] ^= 0x01;
  Save_RecomputeChecksum(live);
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path, disk, &error));
  CHECK(!memcmp(live, disk, sizeof(live)));

  int edits[kActRaiserSaveRegionCount] = {-1, -1, -1, -1, -1, 3};
  CHECK(!SaveSystem_ApplyRegionEdits(edits, false, false, false, &error));
  CHECK(SaveSystem_ApplyRegionEdits(edits, true, false, false, &error));
  CHECK((live[g_save_region_fields[5].offset] * 2 +
         (live[0x13b6 + 5 * 2] & 1)) == 3);
  SaveEditRequest status_edits;
  SaveEditRequest_Clear(&status_edits);
  status_edits.master_level = 17;
  status_edits.master_hp = 24;
  status_edits.master_mp = 0;
  status_edits.lives = 9;
  status_edits.angel_sp_current = 321;
  status_edits.angel_sp_max = 999;
  status_edits.angel_hp_current = 0;
  status_edits.angel_hp_max = 24;
  status_edits.message_speed = 9;
  status_edits.player_name_set = true;
  snprintf(status_edits.player_name, sizeof(status_edits.player_name),
           "CODEX");
  status_edits.professional_mode = 1;
  status_edits.death_heim_state = 4;
  status_edits.equipped_magic = 2;
  status_edits.magic_slots[0] = 4;
  status_edits.magic_slots[1] = 1;
  status_edits.magic_slots[2] = 2;
  status_edits.magic_slots[3] = 3;
  static const int item_values[8] = {
    0x00, 0x05, 0x06, 0x07, 0x0a, 0x0f, 0x12, 0x14,
  };
  memcpy(status_edits.item_slots, item_values,
         sizeof(status_edits.item_slots));
  status_edits.scores[0][0] = 99990;
  status_edits.scores[5][1] = 12340;
  CHECK(SaveSystem_ApplyEdits(&status_edits, true, false, false, &error));
  CHECK(live[0x1442] == 17 && live[0x1443] == 0);
  CHECK(live[0x1444] == 24 && live[0x1445] == 0);
  CHECK(live[0x1446] == 0 && live[0x1447] == 0);
  CHECK(live[0x145c] == 8);
  CHECK(live[0x1433] == 0x41 && live[0x1434] == 0x01);
  CHECK(live[0x1435] == 0xe7 && live[0x1436] == 0x03);
  CHECK(live[0x1437] == 0 && live[0x1438] == 24);
  CHECK(live[0x13b1] == 9);
  CHECK(!memcmp(live + 0x1439, "CODEX\0\0\0\0", 9));
  char player_name[kActRaiserPlayerNameStorageBytes];
  CHECK(SaveSystem_CopyPlayerName(player_name, sizeof(player_name)));
  CHECK(!strcmp(player_name, "CODEX"));
  CHECK(!SaveSystem_CopyPlayerName(player_name, 3));
  CHECK(!memcmp(live + 0x1ff0, "ACT", 3));
  CHECK(live[0x120c] == 3 && (live[0x1240] & 1));
  CHECK(live[0x144a] == 4 && live[0x144b] == 1 &&
        live[0x144c] == 0x82 && live[0x144d] == 3);
  CHECK(live[0x145d] == 2);
  for (int i = 0; i < 8; i++) CHECK(live[0x1453 + i] == item_values[i]);
  CHECK(live[0x1464] == 0x99 && live[0x1465] == 0x99);
  CHECK(live[0x147a] == 0x34 && live[0x147b] == 0x12);
  CHECK(Save_ChecksumValid(live));

  uint8_t before_invalid[kActRaiserSramSize];
  memcpy(before_invalid, live, sizeof(before_invalid));
  SaveEditRequest invalid_edits;
  SaveEditRequest_Clear(&invalid_edits);
  invalid_edits.item_slots[3] = 1;
  CHECK(!SaveSystem_ApplyEdits(&invalid_edits, true, false, false, &error));
  CHECK(!memcmp(before_invalid, live, sizeof(live)));

  SaveEditRequest_Clear(&invalid_edits);
  for (int i = 0; i < 4; i++) invalid_edits.magic_slots[i] = 0;
  invalid_edits.equipped_magic = 4;
  CHECK(!SaveSystem_ApplyEdits(&invalid_edits, true, false, false, &error));
  CHECK(!memcmp(before_invalid, live, sizeof(live)));
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path, disk, &error));
  CHECK((disk[g_save_region_fields[5].offset] * 2 +
         (disk[0x13b6 + 5 * 2] & 1)) != 3);
  CHECK(disk[0x1442] != 17);

  edits[5] = -1;
  edits[4] = 2;
  CHECK(SaveSystem_ApplyRegionEdits(edits, true, true, false, &error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path, disk, &error));
  int marahna_state = -1;
  CHECK(Save_GetRegionState(disk, 4, &marahna_state));
  CHECK(marahna_state == kSaveRegionState_Act1Cleared);
  CHECK(SaveSystem_Export(kSaveFileFormat_Ini, export_path, &error));
  CHECK(Save_LoadFile(kSaveFileFormat_Ini, export_path, disk, &error));
  CHECK(!memcmp(live, disk, sizeof(live)));

  uint8_t imported[kActRaiserSramSize];
  memcpy(imported, original, sizeof(imported));
  CHECK(Save_SetRegionState(imported, 3, 4));
  Save_RecomputeChecksum(imported);
  CHECK(Save_WriteFile(kSaveFileFormat_NativeSrm, import_path,
                       imported, &error));
  CHECK(SaveSystem_Import(import_path, false, &error));
  CHECK(!memcmp(live, imported, sizeof(live)));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path, disk, &error));
  CHECK(!memcmp(disk, imported, sizeof(disk)));

  /* Selecting INI at the next boot makes only that target authoritative.
   * Per-frame persistence must not update the still-present native file. */
  uint8_t ini_original[kActRaiserSramSize];
  uint8_t native_before[kActRaiserSramSize];
  memcpy(ini_original, original, sizeof(ini_original));
  CHECK(Save_SetRegionState(ini_original, 1, 4));
  Save_RecomputeChecksum(ini_original);
  CHECK(Save_WriteFile(kSaveFileFormat_Ini, ini_path, ini_original, &error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path,
                      native_before, &error));
  memset(live, 0x60, sizeof(live));
  CHECK(SaveSystem_Attach(live, sizeof(live), kSaveBackend_Ini,
                          native_path, ini_path, &error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(!memcmp(live, ini_original, sizeof(live)));
  CHECK(Save_SetRegionState(live, 2, 3));
  Save_RecomputeChecksum(live);
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(Save_LoadFile(kSaveFileFormat_Ini, ini_path, disk, &error));
  CHECK(!memcmp(live, disk, sizeof(live)));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, native_path, disk, &error));
  CHECK(!memcmp(native_before, disk, sizeof(disk)));

  remove(native_path); remove(ini_path); remove(import_path);
  remove(export_path);
  remove("actraiser-save-active-test.srm.tmp");
  remove("actraiser-save-active-test.ini.tmp");
}

static void TestLocalizedNameExtension(SaveBackend backend) {
  static const char native_path[] = "actraiser-name-extension-test.srm";
  static const char ini_path[] = "actraiser-name-extension-test.ini";
  const char *localized_path = backend == kSaveBackend_NativeSrm
                                   ? "actraiser-name-extension-test.srm.arname"
                                   : "actraiser-name-extension-test.ini.arname";
  const char *active_path =
      backend == kSaveBackend_NativeSrm ? native_path : ini_path;
  const SaveFileFormat format = backend == kSaveBackend_NativeSrm
                                    ? kSaveFileFormat_NativeSrm
                                    : kSaveFileFormat_Ini;
  remove(native_path); remove(ini_path); remove(localized_path);

  uint8_t live[kActRaiserSramSize];
  MakeFixture(live);
  memcpy(live + 0x1439, "OLD\0\0\0\0\0\0", 9);
  Save_RecomputeChecksum(live);
  SaveError error = {{0}};
  CHECK(SaveSystem_Attach(live, sizeof(live), backend, native_path, ini_path,
                          &error));
  CHECK(SaveSystem_WriteActive(&error));

  /* Unicode names remain associated with the exact retail save and native
   * compatibility mirror without changing the SRAM image. */
  static const char localized_name[] = "E\xCC\x81lise";
  uint8_t before[kActRaiserSramSize], disk[kActRaiserSramSize];
  memcpy(before, live, sizeof(before));
  CHECK(!SaveSystem_SetLocalizedPlayerName(localized_name, "BAD\x01"));
  CHECK(!SaveSystem_SetLocalizedPlayerName(localized_name, "123456789"));
  CHECK(SaveSystem_SetLocalizedPlayerName(localized_name, "ELISE"));
  /* An accepted WRAM name precedes native saving. No early SRAM/sidecar
   * mutation, and repeated persistence attempts must not discard the name. */
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(!memcmp(before, live, sizeof(before)));
  CHECK(Save_LoadFile(format, active_path, disk, &error));
  CHECK(!memcmp(before, disk, sizeof(before)));
  FILE *sidecar = fopen(localized_path, "rb");
  CHECK(sidecar == NULL);
  if (sidecar)
    fclose(sidecar);
  char unicode_name[64];
  CHECK(!SaveSystem_CopyLocalizedPlayerName("OLD", unicode_name,
                                            sizeof(unicode_name)));
  CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE", unicode_name,
                                           sizeof(unicode_name)));
  CHECK(!strcmp(unicode_name, localized_name));
  /* Native game code performs its ordinary save, including its checksum. */
  memcpy(live + 0x1439, "ELISE\0\0\0\0", 9);
  Save_RecomputeChecksum(live);
  memcpy(before, live, sizeof(before));
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(!memcmp(before, live, sizeof(before)));
  CHECK(Save_LoadFile(format, active_path, disk, &error));
  CHECK(!memcmp(before, disk, sizeof(before)) && Save_ChecksumValid(disk));
  memset(live, 0, sizeof(live));
  CHECK(SaveSystem_Attach(live, sizeof(live), backend, native_path, ini_path,
                          &error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE", unicode_name,
                                           sizeof(unicode_name)));
  CHECK(!strcmp(unicode_name, localized_name));

  /* Ordinary gameplay changes rebind the companion to the new checksum. */
  live[0x1200] ^= 1;
  Save_RecomputeChecksum(live);
  char temporary_path[128];
  snprintf(temporary_path, sizeof(temporary_path), "%s.tmp", localized_path);
  /* Force a companion-only write failure after the native save succeeds. */
#ifdef _WIN32
  CHECK(_mkdir(temporary_path) == 0);
#else
  CHECK(mkdir(temporary_path, 0700) == 0);
#endif
  CHECK(!SaveSystem_AutoPersistIfChanged(&error));
  CHECK(Save_LoadFile(format, active_path, disk, &error));
  CHECK(!memcmp(live, disk, sizeof(disk)) && Save_ChecksumValid(disk));
#ifdef _WIN32
  CHECK(_rmdir(temporary_path) == 0);
#else
  CHECK(rmdir(temporary_path) == 0);
#endif
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE", unicode_name,
                                           sizeof(unicode_name)));

  SaveEditRequest edits;
  SaveEditRequest_Clear(&edits);
  edits.master_level = 10;
  CHECK(SaveSystem_ApplyEdits(&edits, true, true, false, &error));
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(SaveSystem_CopyLocalizedPlayerName("ELISE", unicode_name,
                                           sizeof(unicode_name)));

  /* Embedded NUL cannot silently shorten a length-delimited Unicode name. */
  sidecar = fopen(localized_path, "r+b");
  CHECK(sidecar != NULL);
  if (sidecar) {
    CHECK(fseek(sidecar, -1, SEEK_END) == 0);
    CHECK(fputc(0, sidecar) != EOF);
    CHECK(fclose(sidecar) == 0);
  }
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(!SaveSystem_CopyLocalizedPlayerName("ELISE", unicode_name,
                                            sizeof(unicode_name)));
  CHECK(SaveSystem_SetLocalizedPlayerName(localized_name, "ELISE"));
  CHECK(SaveSystem_AutoPersistIfChanged(&error));

  /* Replacing the native save invalidates the checksum-bound extension. */
  live[0x1200] ^= 1;
  Save_RecomputeChecksum(live);
  CHECK(Save_WriteFile(format, active_path, live, &error));
  memset(live, 0, sizeof(live));
  CHECK(SaveSystem_Attach(live, sizeof(live), backend, native_path, ini_path,
                          &error));
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(!SaveSystem_CopyLocalizedPlayerName("ELISE", unicode_name,
                                            sizeof(unicode_name)));

  /* A missing/invalid companion cannot make an otherwise valid save fail. */
  remove(localized_path);
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(Save_ChecksumValid(live));
  CHECK(!SaveSystem_CopyLocalizedPlayerName("ELISE", unicode_name,
                                            sizeof(unicode_name)));
  sidecar = fopen(localized_path, "wb");
  CHECK(sidecar != NULL);
  if (sidecar) {
    fputs("invalid sidecar", sidecar);
    fclose(sidecar);
  }
  CHECK(SaveSystem_LoadActive(&error));
  CHECK(Save_ChecksumValid(live));
  CHECK(!SaveSystem_CopyLocalizedPlayerName("ELISE", unicode_name,
                                            sizeof(unicode_name)));

  CHECK(SaveSystem_SetLocalizedPlayerName(localized_name, "ELISE"));
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  /* Importing another image with the same ASCII name still retires the old
   * metadata; the save checksum, not just the name, binds disk companions. */
  live[0x1200] ^= 1;
  Save_RecomputeChecksum(live);
  CHECK(Save_WriteFile(format, active_path, live, &error));
  CHECK(SaveSystem_Import(active_path, false, &error));
  CHECK(!SaveSystem_CopyLocalizedPlayerName("ELISE", unicode_name,
                                            sizeof(unicode_name)));

  remove(native_path); remove(ini_path); remove(localized_path);
  remove("actraiser-name-extension-test.srm.tmp");
  remove("actraiser-name-extension-test.srm.arname.tmp");
}

static void TestNativeWriteBoundary(SaveBackend backend) {
  const char *native = "actraiser-save-boundary-test.srm";
  const char *ini = "actraiser-save-boundary-test.ini";
  const char *path = backend == kSaveBackend_Ini ? ini : native;
  SaveFileFormat format = backend == kSaveBackend_Ini
      ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm;
  uint8_t live[kActRaiserSramSize], original[kActRaiserSramSize], disk[kActRaiserSramSize];
  MakeFixture(live);
  memcpy(original, live, sizeof(live));
  remove(native); remove(ini);
  SaveError error = {{0}};
  CHECK(SaveSystem_Attach(live, sizeof(live), backend, native, ini, &error));
  memset(disk, 0xa5, sizeof(disk));
  CHECK(!SaveSystem_CopyDurableImage(disk));
  CHECK(disk[0] == 0xa5);
  CHECK(SaveSystem_WriteActive(&error));
  CHECK(SaveSystem_CopyDurableImage(disk));
  CHECK(!memcmp(original, disk, sizeof(disk)));

  /* Session-only shadow changes must never alter the durable identity. */
  live[100] ^= 1;
  Save_RecomputeChecksum(live);
  SaveSystem_ResyncShadow();
  CHECK(SaveSystem_CopyDurableImage(disk));
  CHECK(!memcmp(original, disk, sizeof(disk)));

  CHECK(SaveSystem_BeginNativeWrite(&error));
  CHECK(!SaveSystem_BeginNativeWrite(&error));
  for (int i = 0; i < 8; ++i) {
    live[200 + i] ^= 1;
    CHECK(SaveSystem_AutoPersistIfChanged(&error));
    CHECK(Save_LoadFile(format, path, disk, &error));
    CHECK(!memcmp(original, disk, sizeof(disk)));
  }
  CHECK(!SaveSystem_WriteActive(&error));
  CHECK(!SaveSystem_LoadActive(&error));
  CHECK(!SaveSystem_Import(path, false, &error));
  CHECK(!SaveSystem_Export(format, path, &error));
  SaveEditRequest edits;
  SaveEditRequest_Clear(&edits);
  edits.master_level = 5;
  CHECK(!SaveSystem_ApplyEdits(&edits, true, true, false, &error));
  Save_RecomputeChecksum(live);
  CHECK(SaveSystem_EndNativeWrite(true, &error));
  CHECK(!SaveSystem_EndNativeWrite(true, &error));
  CHECK(SaveSystem_AutoPersistIfChanged(&error));
  CHECK(SaveSystem_CopyDurableImage(disk));
  CHECK(!memcmp(live, disk, sizeof(disk)));
  memcpy(original, disk, sizeof(disk));

  /* An interrupted/invalid writer must not be flushed on shutdown, even if
   * somebody recomputes a checksum afterward. Reload is explicit recovery. */
  for (int abort = 0; abort < 2; ++abort) {
    CHECK(SaveSystem_BeginNativeWrite(&error));
    live[200] ^= 1;
    CHECK(!SaveSystem_EndNativeWrite(!abort, &error));
    Save_RecomputeChecksum(live);
    CHECK(!SaveSystem_AutoPersistIfChanged(&error));
    CHECK(!SaveSystem_WriteActive(&error));
    CHECK(!SaveSystem_BeginNativeWrite(&error));
    CHECK(Save_LoadFile(format, path, disk, &error));
    CHECK(!memcmp(original, disk, sizeof(disk)));
    CHECK(SaveSystem_LoadActive(&error));
    CHECK(!memcmp(original, live, sizeof(live)));
  }
  CHECK(SaveSystem_ApplyEdits(&edits, true, true, false, &error));
  CHECK(SaveSystem_CopyDurableImage(disk));
  CHECK(!memcmp(live, disk, sizeof(disk)));
  CHECK(Save_WriteFile(format, path, original, &error));
  CHECK(SaveSystem_Import(path, false, &error));
  CHECK(SaveSystem_CopyDurableImage(disk));
  CHECK(!memcmp(original, disk, sizeof(disk)));
  remove(native); remove(ini);
}

static void TestRecoveryWithoutFeatureHost(void) {
  const char *path = "actraiser-recovery-test.srm";
  const char *directory = "actraiser-recovery-test-copy";
  const char *copy = "actraiser-recovery-test-copy/save.srm";
  uint8_t live[kActRaiserSramSize], original[kActRaiserSramSize], disk[kActRaiserSramSize];
  MakeFixture(live); memcpy(original, live, sizeof(live));
  SaveError error = {{0}};
  CHECK(SaveSystem_Attach(live, sizeof(live), kSaveBackend_NativeSrm,
                         path, "unused-recovery-test.ini", &error));
  CHECK(SaveSystem_WriteActive(&error));
  CHECK(!SaveSystem_CreateRecoveryCopy(NULL, &error));
  CHECK(!SaveSystem_CreateRecoveryCopy("", &error));
  char long_path[600]; memset(long_path, 'x', sizeof(long_path)); long_path[599] = 0;
  CHECK(!SaveSystem_CreateRecoveryCopy(long_path, &error));
  CHECK(SaveSystem_CreateRecoveryCopy(directory, &error));
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, copy, disk, &error));
  CHECK(!memcmp(disk, original, sizeof(disk)));
  CHECK(!SaveSystem_CreateRecoveryCopy(directory, &error));
  /* Aborted native saves remain blocked even if the checksum is repaired. */
  CHECK(SaveSystem_BeginNativeWrite(&error));
  CHECK(!SaveSystem_EndNativeWrite(false, &error));
  CHECK(!SaveSystem_CreateRecoveryCopy("actraiser-recovery-must-not-exist", &error));
  CHECK(!memcmp(live, original, sizeof(live)));
  CHECK(SaveSystem_CopyDurableImage(disk) && !memcmp(disk, original, sizeof(disk)));
  remove(copy);
#ifdef _WIN32
  CHECK(_rmdir(directory) == 0);
#else
  CHECK(rmdir(directory) == 0);
#endif
  remove(path);
}

int main(void) {
  TestChecksumAndFields();
  TestNativeAndIniCodecs();
  TestLegacyMigration();
  TestRuntimeTransactions();
  TestNativeWriteBoundary(kSaveBackend_NativeSrm);
  TestNativeWriteBoundary(kSaveBackend_Ini);
  TestLocalizedNameExtension(kSaveBackend_NativeSrm);
  TestLocalizedNameExtension(kSaveBackend_Ini);
  TestRecoveryWithoutFeatureHost();
  if (s_failures) {
    fprintf(stderr, "save system tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "save system tests: pass\n");
  return 0;
}
