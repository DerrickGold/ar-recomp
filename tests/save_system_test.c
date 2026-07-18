#define _POSIX_C_SOURCE 200809L

#include "save_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  char fixture_path[1024];
  snprintf(fixture_path, sizeof(fixture_path), "%s/save.sim-blank.bak.srm",
           ACTRAISER_SOURCE_DIR);
  uint8_t real[kActRaiserSramSize];
  SaveError error = {{0}};
  CHECK(Save_LoadFile(kSaveFileFormat_NativeSrm, fixture_path, real, &error));
  CHECK(Save_ChecksumValid(real));
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

int main(void) {
  TestChecksumAndFields();
  TestNativeAndIniCodecs();
  TestRuntimeTransactions();
  if (s_failures) {
    fprintf(stderr, "save system tests: %d failure(s)\n", s_failures);
    return 1;
  }
  fprintf(stderr, "save system tests: pass\n");
  return 0;
}
