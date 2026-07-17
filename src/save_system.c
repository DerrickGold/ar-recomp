#include "save_system.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

const SaveFieldDesc g_save_region_fields[kActRaiserSaveRegionCount] = {
  { "fillmore",  "Fillmore",  0x1200 },
  { "bloodpool", "Bloodpool", 0x1202 },
  { "kasandora", "Kasandora", 0x1204 },
  { "aitos",     "Aitos",     0x1206 },
  { "marahna",   "Marahna",   0x1208 },
  { "northwall", "Northwall", 0x120a },
};

enum {
  /* The third-party template uses European offsets for this block and shifts
   * them back two bytes for USA. These USA offsets map linearly to the WRAM
   * save-stat block at $0282-$02AC by subtracting $11B1. */
  kSaveMessageSpeed = 0x13b1,
  kSaveRegionAct2Flags = 0x13b6,
  kSaveDeathHeimState = 0x120c,
  kSaveDeathHeimUnlocked = 0x1240,
  kSaveAngelSpCurrent = 0x1433,
  kSaveAngelSpMax = 0x1435,
  kSaveAngelHpCurrent = 0x1437,
  kSaveAngelHpMax = 0x1438,
  kSavePlayerName = 0x1439,
  kSaveMasterLevel = 0x1442,
  kSaveMasterHp = 0x1444,
  kSaveMasterMp = 0x1446,
  kSaveMagicSlots = 0x144a,
  kSaveItemSlots = 0x1453,
  kSaveLives = 0x145c,
  kSaveEquippedMagic = 0x145d,
  kSaveScores = 0x1464,
  kSaveProfessionalMode = 0x1ff0,
};

typedef struct SaveRuntime {
  uint8_t *live;
  size_t size;
  SaveBackend backend;
  char native_path[512];
  char ini_path[512];
  uint8_t shadow[kActRaiserSramSize];
  bool shadow_valid;
  bool backup_taken;
} SaveRuntime;

static SaveRuntime s_runtime;

static bool Fail(SaveError *error, const char *format, ...) {
  if (error) {
    va_list args;
    va_start(args, format);
    vsnprintf(error->message, sizeof(error->message), format, args);
    va_end(args);
  }
  return false;
}

static void ClearError(SaveError *error) {
  if (error) error->message[0] = 0;
}

static uint16_t ReadLe16(const uint8_t *data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void WriteLe16(uint8_t *data, uint16_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
}

static uint32_t ReadLe32(const uint8_t *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void WriteLe32(uint8_t *data, uint32_t value) {
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)(value >> 16);
  data[3] = (uint8_t)(value >> 24);
}

uint32_t Save_ComputeChecksum(const uint8_t sram[kActRaiserSramSize]) {
  uint16_t xored = 0;
  uint16_t summed = 0;
  if (!sram) return 0;
  for (int offset = 0; offset < kActRaiserSramChecksumOffset; offset += 2) {
    uint16_t word = ReadLe16(sram + offset);
    xored ^= word;
    summed = (uint16_t)(summed + word);
  }
  return ((uint32_t)xored << 16) | summed;
}

uint32_t Save_StoredChecksum(const uint8_t sram[kActRaiserSramSize]) {
  return sram ? ReadLe32(sram + kActRaiserSramChecksumOffset) : 0;
}

bool Save_ChecksumValid(const uint8_t sram[kActRaiserSramSize]) {
  return sram && Save_StoredChecksum(sram) == Save_ComputeChecksum(sram);
}

uint32_t Save_RecomputeChecksum(uint8_t sram[kActRaiserSramSize]) {
  uint32_t checksum = Save_ComputeChecksum(sram);
  if (sram) WriteLe32(sram + kActRaiserSramChecksumOffset, checksum);
  return checksum;
}

void SaveEditRequest_Clear(SaveEditRequest *edits) {
  if (!edits) return;
  memset(edits, 0xff, sizeof(*edits));
  edits->player_name_set = false;
  edits->player_name[0] = 0;
}

bool Save_GetRegionState(const uint8_t sram[kActRaiserSramSize],
                         int region, int *value) {
  if (!sram || !value || region < 0 || region >= kActRaiserSaveRegionCount)
    return false;
  int base = sram[g_save_region_fields[region].offset];
  int act2 = sram[kSaveRegionAct2Flags + region * 2] & 1;
  *value = base * 2 + act2;
  return *value == kSaveRegionState_Act1 ||
      *value == kSaveRegionState_Act1Cleared ||
      *value == kSaveRegionState_Act2 ||
      *value == kSaveRegionState_Act2Cleared;
}

bool Save_SetRegionState(uint8_t sram[kActRaiserSramSize],
                         int region, int value) {
  if (!sram || region < 0 || region >= kActRaiserSaveRegionCount ||
      (value != kSaveRegionState_Act1 &&
       value != kSaveRegionState_Act1Cleared &&
       value != kSaveRegionState_Act2 &&
       value != kSaveRegionState_Act2Cleared))
    return false;
  sram[g_save_region_fields[region].offset] = (uint8_t)(value / 2);
  uint8_t *flag = &sram[kSaveRegionAct2Flags + region * 2];
  *flag = (uint8_t)((*flag & ~1u) | (value & 1));
  return true;
}

const char *Save_RegionStateName(int value) {
  switch (value) {
    case kSaveRegionState_Act1: return "act1";
    case kSaveRegionState_Act1Cleared: return "act1-cleared";
    case kSaveRegionState_Act2: return "act2";
    case kSaveRegionState_Act2Cleared: return "act2-cleared";
  }
  return NULL;
}

bool Save_ParseRegionState(const char *text, int *value) {
  if (!text || !value) return false;
  static const int states[] = {
    kSaveRegionState_Act1, kSaveRegionState_Act1Cleared,
    kSaveRegionState_Act2, kSaveRegionState_Act2Cleared,
  };
  for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
    if (!strcmp(text, Save_RegionStateName(states[i]))) {
      *value = states[i];
      return true;
    }
  }
  return false;
}

static char *TrimLeft(char *text) {
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    text++;
  return text;
}

static void TrimRight(char *text) {
  size_t length = strlen(text);
  while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                    text[length - 1] == '\r' || text[length - 1] == '\n'))
    text[--length] = 0;
}

static void StripComment(char *text) {
  for (char *p = text; *p; p++) {
    if ((*p == ';' || *p == '#') &&
        (p == text || p[-1] == ' ' || p[-1] == '\t')) {
      *p = 0;
      break;
    }
  }
  TrimRight(text);
}

static int HexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

static bool DecodeHexChunk(const char *text, uint8_t out[64]) {
  if (!text || strlen(text) != 128) return false;
  for (int i = 0; i < 64; i++) {
    int high = HexDigit(text[i * 2]);
    int low = HexDigit(text[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    out[i] = (uint8_t)((high << 4) | low);
  }
  return true;
}

static bool LoadNative(const char *path,
                       uint8_t scratch[kActRaiserSramSize],
                       SaveError *error) {
  FILE *file = fopen(path, "rb");
  if (!file) return Fail(error, "cannot read %s: %s", path, strerror(errno));
  size_t size = fread(scratch, 1, kActRaiserSramSize, file);
  int extra = fgetc(file);
  bool io_ok = !ferror(file);
  fclose(file);
  if (!io_ok) return Fail(error, "error reading %s", path);
  if (size != kActRaiserSramSize || extra != EOF)
    return Fail(error, "%s must be exactly %d bytes", path,
                kActRaiserSramSize);
  if (!Save_ChecksumValid(scratch))
    return Fail(error, "%s has an invalid ActRaiser checksum", path);
  return true;
}

static int FindRegion(const char *key) {
  for (int i = 0; i < kActRaiserSaveRegionCount; i++)
    if (!strcmp(key, g_save_region_fields[i].key)) return i;
  return -1;
}

static bool LoadIni(const char *path,
                    uint8_t scratch[kActRaiserSramSize],
                    SaveError *error) {
  FILE *file = fopen(path, "r");
  if (!file) return Fail(error, "cannot read %s: %s", path, strerror(errno));

  enum { kSection_None, kSection_Meta, kSection_Regions, kSection_Raw } section;
  section = kSection_None;
  bool raw_seen[kActRaiserSramSize / 64] = {0};
  bool region_seen[kActRaiserSaveRegionCount] = {0};
  int region_values[kActRaiserSaveRegionCount] = {0};
  bool format_seen = false, version_seen = false, size_seen = false;
  bool success = true;
  char line[1024];
  int line_number = 0;
  memset(scratch, 0, kActRaiserSramSize);

  while (success && fgets(line, sizeof(line), file)) {
    line_number++;
    char *key = TrimLeft(line);
    TrimRight(key);
    if (!key[0] || key[0] == ';' || key[0] == '#') continue;
    if (key[0] == '[') {
      if (!strcmp(key, "[Meta]")) section = kSection_Meta;
      else if (!strcmp(key, "[Regions]")) section = kSection_Regions;
      else if (!strcmp(key, "[Raw]")) section = kSection_Raw;
      else section = kSection_None;
      continue;
    }
    char *equals = strchr(key, '=');
    if (!equals) {
      success = Fail(error, "%s:%d: expected key = value", path,
                     line_number);
      break;
    }
    *equals = 0;
    TrimRight(key);
    char *value = TrimLeft(equals + 1);
    StripComment(value);

    if (section == kSection_Meta) {
      if (!strcmp(key, "format")) {
        if (format_seen || strcmp(value, "actraiser-sram"))
          success = Fail(error, "%s:%d: invalid/duplicate format", path,
                         line_number);
        format_seen = true;
      } else if (!strcmp(key, "version")) {
        if (version_seen || strcmp(value, "1"))
          success = Fail(error, "%s:%d: unsupported/duplicate version", path,
                         line_number);
        version_seen = true;
      } else if (!strcmp(key, "size")) {
        char *end = NULL;
        long declared = strtol(value, &end, 0);
        if (size_seen || !end || *end || declared != kActRaiserSramSize)
          success = Fail(error, "%s:%d: invalid/duplicate SRAM size", path,
                         line_number);
        size_seen = true;
      } else if (!strcmp(key, "rom") && strcmp(value, "usa")) {
        success = Fail(error, "%s:%d: unsupported ROM '%s'", path,
                       line_number, value);
      }
    } else if (section == kSection_Regions) {
      int region = FindRegion(key);
      if (region >= 0) {
        int parsed = 0;
        if (region_seen[region] ||
            !Save_ParseRegionState(value, &parsed)) {
          success = Fail(error, "%s:%d: invalid/duplicate region '%s'", path,
                         line_number, key);
        } else {
          region_seen[region] = true;
          region_values[region] = parsed;
        }
      }
    } else if (section == kSection_Raw) {
      char *end = NULL;
      long offset = strtol(key, &end, 16);
      if (!end || *end || strlen(key) != 4 || offset < 0 ||
          offset >= kActRaiserSramSize || (offset & 63)) {
        success = Fail(error, "%s:%d: invalid raw chunk '%s'", path,
                       line_number, key);
      } else {
        int chunk = (int)offset / 64;
        if (raw_seen[chunk] || !DecodeHexChunk(value, scratch + offset))
          success = Fail(error, "%s:%d: invalid/duplicate raw chunk '%s'",
                         path, line_number, key);
        else
          raw_seen[chunk] = true;
      }
    }
  }
  if (ferror(file) && success)
    success = Fail(error, "error reading %s", path);
  fclose(file);
  if (!success) return false;
  if (!format_seen || !version_seen || !size_seen)
    return Fail(error, "%s is missing required [Meta] keys", path);
  for (int i = 0; i < kActRaiserSramSize / 64; i++)
    if (!raw_seen[i])
      return Fail(error, "%s is missing raw chunk %04X", path, i * 64);
  if (!Save_ChecksumValid(scratch))
    return Fail(error, "%s raw image has an invalid checksum", path);

  for (int i = 0; i < kActRaiserSaveRegionCount; i++)
    if (region_seen[i])
      Save_SetRegionState(scratch, i, region_values[i]);
  Save_RecomputeChecksum(scratch);
  return true;
}

bool Save_LoadFile(SaveFileFormat format, const char *path,
                   uint8_t out[kActRaiserSramSize], SaveError *error) {
  ClearError(error);
  if (!path || !path[0] || !out) return Fail(error, "invalid save load request");
  uint8_t scratch[kActRaiserSramSize];
  bool ok = format == kSaveFileFormat_Ini
      ? LoadIni(path, scratch, error) : LoadNative(path, scratch, error);
  if (ok) memcpy(out, scratch, sizeof(scratch));
  return ok;
}

typedef bool (*WriteBodyFn)(FILE *file, const void *context,
                            SaveError *error);

static bool ReplaceFile(const char *temporary, const char *path) {
#ifdef _WIN32
  return MoveFileExA(temporary, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return rename(temporary, path) == 0;
#endif
}

static bool WriteAtomic(const char *path, WriteBodyFn body,
                        const void *context, SaveError *error) {
  size_t length = strlen(path);
  char *temporary = (char *)malloc(length + 5);
  if (!temporary) return Fail(error, "out of memory writing %s", path);
  memcpy(temporary, path, length);
  memcpy(temporary + length, ".tmp", 5);
  FILE *file = fopen(temporary, "wb");
  if (!file) {
    bool result = Fail(error, "cannot write %s: %s", temporary,
                       strerror(errno));
    free(temporary);
    return result;
  }
  bool success = body(file, context, error);
  if (fflush(file) != 0 || ferror(file))
    success = Fail(error, "error flushing %s", temporary);
  if (fclose(file) != 0)
    success = Fail(error, "error closing %s", temporary);
  if (success && !ReplaceFile(temporary, path))
    success = Fail(error, "cannot replace %s: %s", path, strerror(errno));
  if (!success) remove(temporary);
  free(temporary);
  return success;
}

static bool WriteNativeBody(FILE *file, const void *context,
                            SaveError *error) {
  if (fwrite(context, 1, kActRaiserSramSize, file) != kActRaiserSramSize)
    return Fail(error, "error writing native SRAM");
  return true;
}

static bool WriteIniBody(FILE *file, const void *context,
                         SaveError *error) {
  uint8_t image[kActRaiserSramSize];
  memcpy(image, context, sizeof(image));
  Save_RecomputeChecksum(image);
  if (fprintf(file,
      "; ActRaiser Recompiled lossless battery save\n"
      "[Meta]\nformat = actraiser-sram\nversion = 1\n"
      "size = 0x2000\nrom = usa\n\n[Regions]\n") < 0)
    return Fail(error, "error writing INI metadata");
  for (int i = 0; i < kActRaiserSaveRegionCount; i++) {
    int progress = 0;
    if (!Save_GetRegionState(image, i, &progress))
      return Fail(error, "invalid %s state bytes $%02X/$%02X",
                  g_save_region_fields[i].label,
                  image[g_save_region_fields[i].offset],
                  image[kSaveRegionAct2Flags + i * 2] & 1);
    if (fprintf(file, "%s = %s\n", g_save_region_fields[i].key,
                Save_RegionStateName(progress)) < 0)
      return Fail(error, "error writing INI regions");
  }
  if (fprintf(file, "\n[Raw]\n") < 0)
    return Fail(error, "error writing INI raw header");
  static const char hex[] = "0123456789abcdef";
  char encoded[129];
  encoded[128] = 0;
  for (int offset = 0; offset < kActRaiserSramSize; offset += 64) {
    for (int i = 0; i < 64; i++) {
      encoded[i * 2] = hex[image[offset + i] >> 4];
      encoded[i * 2 + 1] = hex[image[offset + i] & 15];
    }
    if (fprintf(file, "%04x = %s\n", offset, encoded) < 0)
      return Fail(error, "error writing INI raw chunk %04X", offset);
  }
  return true;
}

bool Save_WriteFile(SaveFileFormat format, const char *path,
                    const uint8_t sram[kActRaiserSramSize], SaveError *error) {
  ClearError(error);
  if (!path || !path[0] || !sram)
    return Fail(error, "invalid save write request");
  return WriteAtomic(path,
                     format == kSaveFileFormat_Ini
                         ? WriteIniBody : WriteNativeBody,
                     sram, error);
}

static const char *ActivePath(void) {
  return s_runtime.backend == kSaveBackend_Ini
      ? s_runtime.ini_path : s_runtime.native_path;
}

static SaveFileFormat ActiveFormat(void) {
  return s_runtime.backend == kSaveBackend_Ini
      ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm;
}

bool SaveSystem_Attach(uint8_t *live_sram, size_t size,
                       SaveBackend backend,
                       const char *native_path, const char *ini_path,
                       SaveError *error) {
  ClearError(error);
  if (!live_sram || size != kActRaiserSramSize ||
      backend < 0 || backend >= kSaveBackend_Count ||
      !native_path || !native_path[0] || !ini_path || !ini_path[0])
    return Fail(error, "ActRaiser save system requires one 8192-byte SRAM");
  if (strlen(native_path) >= sizeof(s_runtime.native_path) ||
      strlen(ini_path) >= sizeof(s_runtime.ini_path))
    return Fail(error, "save path exceeds the %zu-byte runtime limit",
                sizeof(s_runtime.native_path) - 1);
  memset(&s_runtime, 0, sizeof(s_runtime));
  s_runtime.live = live_sram;
  s_runtime.size = size;
  s_runtime.backend = backend;
  snprintf(s_runtime.native_path, sizeof(s_runtime.native_path), "%s",
           native_path);
  snprintf(s_runtime.ini_path, sizeof(s_runtime.ini_path), "%s", ini_path);
  return true;
}

void SaveSystem_ResyncShadow(void) {
  if (!s_runtime.live || s_runtime.size != kActRaiserSramSize) return;
  memcpy(s_runtime.shadow, s_runtime.live, kActRaiserSramSize);
  s_runtime.shadow_valid = true;
}

bool SaveSystem_LoadActive(SaveError *error) {
  ClearError(error);
  if (!s_runtime.live) return Fail(error, "save system is not attached");
  const char *path = ActivePath();
  FILE *probe = fopen(path, "rb");
  if (!probe) {
    if (errno == ENOENT) {
      SaveSystem_ResyncShadow();
      fprintf(stderr, "[saves] %s backend: no existing %s; using fresh SRAM\n",
              s_runtime.backend == kSaveBackend_Ini ? "ini" : "native-srm",
              path);
      return true;
    }
    return Fail(error, "cannot inspect %s: %s", path, strerror(errno));
  }
  fclose(probe);
  if (!Save_LoadFile(ActiveFormat(), path, s_runtime.live, error)) return false;
  SaveSystem_ResyncShadow();
  fprintf(stderr, "[saves] loaded %s backend from %s\n",
          s_runtime.backend == kSaveBackend_Ini ? "ini" : "native-srm", path);
  return true;
}

bool SaveSystem_WriteActive(SaveError *error) {
  ClearError(error);
  if (!s_runtime.live) return Fail(error, "save system is not attached");
  if (!Save_WriteFile(ActiveFormat(), ActivePath(), s_runtime.live, error))
    return false;
  SaveSystem_ResyncShadow();
  return true;
}

bool SaveSystem_AutoPersistIfChanged(SaveError *error) {
  ClearError(error);
  if (!s_runtime.live) return Fail(error, "save system is not attached");
  if (!s_runtime.shadow_valid) {
    SaveSystem_ResyncShadow();
    return true;
  }
  if (!memcmp(s_runtime.shadow, s_runtime.live, kActRaiserSramSize)) return true;
  if (!SaveSystem_WriteActive(error)) return false;
  fprintf(stderr, "[saves] battery SRAM changed -> wrote %s\n", ActivePath());
  return true;
}

const char *SaveSystem_ActivePath(void) {
  return s_runtime.live ? ActivePath() : "";
}

SaveBackend SaveSystem_ActiveBackend(void) {
  return s_runtime.backend;
}

static bool CopyFile(const char *source, const char *destination,
                     SaveError *error) {
  FILE *in = fopen(source, "rb");
  if (!in) return Fail(error, "cannot read %s: %s", source, strerror(errno));
  FILE *out = fopen(destination, "wb");
  if (!out) {
    fclose(in);
    return Fail(error, "cannot write %s: %s", destination, strerror(errno));
  }
  bool success = true;
  uint8_t buffer[4096];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), in)) != 0) {
    if (fwrite(buffer, 1, count, out) != count) {
      success = Fail(error, "error writing backup %s", destination);
      break;
    }
  }
  if (ferror(in)) success = Fail(error, "error reading %s", source);
  if (fclose(in) != 0) success = Fail(error, "error closing %s", source);
  if (fflush(out) != 0)
    success = Fail(error, "error flushing backup %s", destination);
  if (fclose(out) != 0)
    success = Fail(error, "error closing backup %s", destination);
  if (!success) remove(destination);
  return success;
}

static bool BackupActiveOnce(bool enabled, SaveError *error) {
  if (!enabled || s_runtime.backup_taken) return true;
  const char *path = ActivePath();
  FILE *probe = fopen(path, "rb");
  if (!probe) {
    if (errno == ENOENT) {
      s_runtime.backup_taken = true;
      return true;
    }
    return Fail(error, "cannot inspect %s for backup: %s", path,
                strerror(errno));
  }
  fclose(probe);
  time_t now = time(NULL);
  struct tm local_time;
#ifdef _WIN32
  localtime_s(&local_time, &now);
#else
  localtime_r(&now, &local_time);
#endif
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time);
  char backup[640];
  snprintf(backup, sizeof(backup), "%s.bak-%s", path, timestamp);
  if (!CopyFile(path, backup, error)) return false;
  s_runtime.backup_taken = true;
  fprintf(stderr, "[saves] backup -> %s\n", backup);
  return true;
}

static bool SetStagedU8(uint8_t *scratch, int offset, int value,
                        int minimum, int maximum, const char *label,
                        SaveError *error) {
  if (value < 0) return true;
  if (value < minimum || value > maximum)
    return Fail(error, "%s must be %d..%d", label, minimum, maximum);
  scratch[offset] = (uint8_t)value;
  return true;
}

static bool SetStagedU16(uint8_t *scratch, int offset, int value,
                         int minimum, int maximum, const char *label,
                         SaveError *error) {
  if (value < 0) return true;
  if (value < minimum || value > maximum)
    return Fail(error, "%s must be %d..%d", label, minimum, maximum);
  WriteLe16(scratch + offset, (uint16_t)value);
  return true;
}

static bool SaveItemValueValid(int value) {
  static const uint8_t values[] = {
    0x00, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0d, 0x0e, 0x0f, 0x12, 0x13, 0x14,
  };
  for (size_t i = 0; i < sizeof(values); i++)
    if (value == values[i]) return true;
  return false;
}

static bool ScoreToBcd(int score, uint16_t *encoded) {
  if (!encoded || score < 0 || score > 99990 || score % 10) return false;
  int value = score / 10;
  uint16_t bcd = 0;
  for (int shift = 0; shift < 16; shift += 4) {
    bcd |= (uint16_t)((value % 10) << shift);
    value /= 10;
  }
  if (value) return false;
  *encoded = bcd;
  return true;
}

bool SaveSystem_ApplyEdits(const SaveEditRequest *edits,
                           bool armed, bool persist,
                           bool auto_backup, SaveError *error) {
  ClearError(error);
  if (!s_runtime.live || !edits)
    return Fail(error, "save system is not attached");
  if (!armed) return Fail(error, "save editing is not armed");
  if (!Save_ChecksumValid(s_runtime.live))
    return Fail(error, "current SRAM has no valid save checksum");
  uint8_t scratch[kActRaiserSramSize];
  memcpy(scratch, s_runtime.live, sizeof(scratch));
  for (int i = 0; i < kActRaiserSaveRegionCount; i++) {
    if (edits->region_state[i] < 0) continue;
    if (!Save_SetRegionState(scratch, i, edits->region_state[i]))
      return Fail(error, "invalid %s state value %d",
                  g_save_region_fields[i].label, edits->region_state[i]);
  }
  if (edits->player_name_set) {
    size_t length = strlen(edits->player_name);
    if (length == 0 || length > 8)
      return Fail(error, "Player name must contain 1..8 characters");
    for (size_t i = 0; i < length; i++) {
      unsigned char ch = (unsigned char)edits->player_name[i];
      if (ch < 0x20 || ch > 0x7e)
        return Fail(error, "Player name must use printable ASCII");
    }
    /* The game reserves nine bytes at $1439-$1441 for an eight-character
     * name plus terminator/padding; $1442 begins the level word. */
    memset(scratch + kSavePlayerName, 0, 9);
    memcpy(scratch + kSavePlayerName, edits->player_name, length);
  }
  if (edits->professional_mode >= 0) {
    if (edits->professional_mode > 1)
      return Fail(error, "Professional mode must be locked or unlocked");
    if (edits->professional_mode) {
      scratch[kSaveProfessionalMode + 0] = 'A';
      scratch[kSaveProfessionalMode + 1] = 'C';
      scratch[kSaveProfessionalMode + 2] = 'T';
    } else {
      memset(scratch + kSaveProfessionalMode, 0xff, 3);
    }
  }
  if (edits->death_heim_state >= 0) {
    if (edits->death_heim_state != 0 && edits->death_heim_state != 1 &&
        edits->death_heim_state != 4)
      return Fail(error, "Death Heim state must be locked, unlocked, or cleared");
    scratch[kSaveDeathHeimState] =
        (uint8_t)(edits->death_heim_state == 4 ? 3 : 0);
    scratch[kSaveDeathHeimUnlocked] =
        (uint8_t)((scratch[kSaveDeathHeimUnlocked] & ~1u) |
                  (edits->death_heim_state != 0));
  }
  if (!SetStagedU16(scratch, kSaveMasterLevel, edits->master_level,
                    1, 17, "Master level", error) ||
      !SetStagedU16(scratch, kSaveMasterHp, edits->master_hp,
                    1, 24, "Master HP", error) ||
      !SetStagedU16(scratch, kSaveMasterMp, edits->master_mp,
                    0, 10, "Master MP", error) ||
      !SetStagedU16(scratch, kSaveAngelSpCurrent, edits->angel_sp_current,
                    0, 999, "Angel current SP", error) ||
      !SetStagedU16(scratch, kSaveAngelSpMax, edits->angel_sp_max,
                    0, 999, "Angel maximum SP", error) ||
      !SetStagedU8(scratch, kSaveAngelHpCurrent, edits->angel_hp_current,
                   0, 24, "Angel current HP", error) ||
      !SetStagedU8(scratch, kSaveAngelHpMax, edits->angel_hp_max,
                   1, 24, "Angel maximum HP", error) ||
      !SetStagedU8(scratch, kSaveMessageSpeed, edits->message_speed,
                   0, 9, "Message speed", error))
    return false;
  if (edits->lives >= 0) {
    if (edits->lives < 1 || edits->lives > 9)
      return Fail(error, "Lives must be 1..9");
    scratch[kSaveLives] = (uint8_t)(edits->lives - 1);
  }
  for (int i = 0; i < 4; i++) {
    if (edits->magic_slots[i] < 0) continue;
    if (edits->magic_slots[i] > 4)
      return Fail(error, "Magic slot %d must be empty or magic 1..4", i + 1);
    scratch[kSaveMagicSlots + i] =
        (uint8_t)((scratch[kSaveMagicSlots + i] & 0x80) |
                  edits->magic_slots[i]);
  }
  if (edits->equipped_magic >= 0) {
    if (edits->equipped_magic > 4)
      return Fail(error, "Equipped magic must be none or spell 1..4");
    for (int i = 0; i < 4; i++) scratch[kSaveMagicSlots + i] &= 0x7f;
    if (edits->equipped_magic) {
      int selected_slot = -1;
      for (int i = 0; i < 4; i++) {
        if ((scratch[kSaveMagicSlots + i] & 0x7f) ==
            edits->equipped_magic) {
          selected_slot = i;
          break;
        }
      }
      if (selected_slot < 0)
        return Fail(error, "Equipped spell %d is not present in a magic slot",
                    edits->equipped_magic);
      scratch[kSaveMagicSlots + selected_slot] |= 0x80;
    }
    scratch[kSaveEquippedMagic] = (uint8_t)edits->equipped_magic;
  }
  for (int i = 0; i < 8; i++) {
    if (edits->item_slots[i] < 0) continue;
    if (!SaveItemValueValid(edits->item_slots[i]))
      return Fail(error, "Item slot %d has invalid item $%02X", i + 1,
                  edits->item_slots[i]);
    scratch[kSaveItemSlots + i] = (uint8_t)edits->item_slots[i];
  }
  for (int region = 0; region < kActRaiserSaveRegionCount; region++) {
    for (int act = 0; act < 2; act++) {
      int score = edits->scores[region][act];
      if (score < 0) continue;
      uint16_t bcd = 0;
      if (!ScoreToBcd(score, &bcd))
        return Fail(error, "%s Act %d score must be 0..99990 by 10",
                    g_save_region_fields[region].label, act + 1);
      WriteLe16(scratch + kSaveScores + region * 4 + act * 2, bcd);
    }
  }
  if (!memcmp(scratch, s_runtime.live, sizeof(scratch)))
    return Fail(error, "no staged save edits change the image");
  Save_RecomputeChecksum(scratch);
  if (persist) {
    if (!BackupActiveOnce(auto_backup, error)) return false;
    if (!Save_WriteFile(ActiveFormat(), ActivePath(), scratch, error))
      return false;
  }
  memcpy(s_runtime.live, scratch, sizeof(scratch));
  SaveSystem_ResyncShadow();
  return true;
}

bool SaveSystem_ApplyRegionEdits(const int edits[kActRaiserSaveRegionCount],
                                 bool armed, bool persist,
                                 bool auto_backup, SaveError *error) {
  if (!edits) return Fail(error, "invalid region edit request");
  SaveEditRequest request;
  SaveEditRequest_Clear(&request);
  memcpy(request.region_state, edits, sizeof(request.region_state));
  return SaveSystem_ApplyEdits(&request, armed, persist,
                               auto_backup, error);
}

static SaveFileFormat FormatFromPath(const char *path) {
  size_t length = path ? strlen(path) : 0;
  const char *extension = length >= 4 ? path + length - 4 : "";
  return length >= 4 && extension[0] == '.' &&
      tolower((unsigned char)extension[1]) == 'i' &&
      tolower((unsigned char)extension[2]) == 'n' &&
      tolower((unsigned char)extension[3]) == 'i'
      ? kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm;
}

bool SaveSystem_Import(const char *path, bool auto_backup, SaveError *error) {
  ClearError(error);
  if (!s_runtime.live || !path || !path[0])
    return Fail(error, "invalid save import request");
  uint8_t scratch[kActRaiserSramSize];
  if (!Save_LoadFile(FormatFromPath(path), path, scratch, error)) return false;
  if (!BackupActiveOnce(auto_backup, error)) return false;
  if (!Save_WriteFile(ActiveFormat(), ActivePath(), scratch, error)) return false;
  memcpy(s_runtime.live, scratch, sizeof(scratch));
  SaveSystem_ResyncShadow();
  return true;
}

bool SaveSystem_Export(SaveFileFormat format, const char *path,
                       SaveError *error) {
  ClearError(error);
  if (!s_runtime.live) return Fail(error, "save system is not attached");
  if (!Save_ChecksumValid(s_runtime.live))
    return Fail(error, "current SRAM has no valid save checksum");
  return Save_WriteFile(format, path, s_runtime.live, error);
}
