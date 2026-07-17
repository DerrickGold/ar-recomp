#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  kActRaiserSramSize = 0x2000,
  kActRaiserSramChecksumOffset = 0x1fec,
  kActRaiserSaveRegionCount = 6,
};

typedef enum SaveRegionState {
  kSaveRegionState_Act1 = 0,
  kSaveRegionState_Act1Cleared = 2,
  kSaveRegionState_Act2 = 3,
  kSaveRegionState_Act2Cleared = 4,
} SaveRegionState;

typedef enum SaveFileFormat {
  kSaveFileFormat_NativeSrm = 0,
  kSaveFileFormat_Ini,
} SaveFileFormat;

typedef enum SaveBackend {
  kSaveBackend_NativeSrm = 0,
  kSaveBackend_Ini,
  kSaveBackend_Count,
} SaveBackend;

typedef struct SaveError {
  char message[256];
} SaveError;

typedef struct SaveFieldDesc {
  const char *key;
  const char *label;
  uint16_t offset;
} SaveFieldDesc;

extern const SaveFieldDesc g_save_region_fields[kActRaiserSaveRegionCount];

/* All values use -1 for "leave as-is". Lives is the displayed 1..9 value;
 * the codec handles the game's zero-based stored representation. */
typedef struct SaveEditRequest {
  int region_state[kActRaiserSaveRegionCount];
  bool player_name_set;
  char player_name[9];
  int professional_mode;
  int death_heim_state;
  int master_level;
  int master_hp;
  int master_mp;
  int lives;
  int angel_sp_current;
  int angel_sp_max;
  int angel_hp_current;
  int angel_hp_max;
  int message_speed;
  int equipped_magic;
  int magic_slots[4];
  int item_slots[8];
  int scores[kActRaiserSaveRegionCount][2];
} SaveEditRequest;

void SaveEditRequest_Clear(SaveEditRequest *edits);

uint32_t Save_ComputeChecksum(const uint8_t sram[kActRaiserSramSize]);
uint32_t Save_StoredChecksum(const uint8_t sram[kActRaiserSramSize]);
bool Save_ChecksumValid(const uint8_t sram[kActRaiserSramSize]);
uint32_t Save_RecomputeChecksum(uint8_t sram[kActRaiserSramSize]);

bool Save_GetRegionState(const uint8_t sram[kActRaiserSramSize],
                         int region, int *value);
bool Save_SetRegionState(uint8_t sram[kActRaiserSramSize],
                         int region, int value);
const char *Save_RegionStateName(int value);
bool Save_ParseRegionState(const char *text, int *value);

/* Transactional file codecs. Load never changes `out` on any error. Native
 * files must be exactly 8192 bytes and checksum-valid. INI version 1 requires
 * every lossless raw chunk and validates it before applying named overrides. */
bool Save_LoadFile(SaveFileFormat format, const char *path,
                   uint8_t out[kActRaiserSramSize], SaveError *error);
bool Save_WriteFile(SaveFileFormat format, const char *path,
                    const uint8_t sram[kActRaiserSramSize], SaveError *error);

/* Runtime owner for the one canonical g_sram image. The active backend is
 * snapshotted at boot; changing the corresponding setting takes effect after
 * restart and never redirects writes halfway through a session. */
bool SaveSystem_Attach(uint8_t *live_sram, size_t size,
                       SaveBackend backend,
                       const char *native_path, const char *ini_path,
                       SaveError *error);
bool SaveSystem_LoadActive(SaveError *error);
bool SaveSystem_WriteActive(SaveError *error);
bool SaveSystem_AutoPersistIfChanged(SaveError *error);
void SaveSystem_ResyncShadow(void);
const char *SaveSystem_ActivePath(void);
SaveBackend SaveSystem_ActiveBackend(void);

/* Every request field contains -1 for unchanged or its documented value. A
 * persistent edit backs up and atomically writes the active format before the
 * scratch image replaces live SRAM. Session-only edits only replace live SRAM.
 * Both paths repair the checksum and re-sync the auto-persist shadow. */
bool SaveSystem_ApplyEdits(const SaveEditRequest *edits,
                           bool armed, bool persist,
                           bool auto_backup, SaveError *error);
/* Compatibility helper retained for focused region-state tests/callers. */
bool SaveSystem_ApplyRegionEdits(const int edits[kActRaiserSaveRegionCount],
                                 bool armed, bool persist,
                                 bool auto_backup, SaveError *error);
bool SaveSystem_Import(const char *path, bool auto_backup, SaveError *error);
bool SaveSystem_Export(SaveFileFormat format, const char *path,
                       SaveError *error);
