#ifndef AR_SAVE_SYSTEM_H
#define AR_SAVE_SYSTEM_H
/* SaveSystem: the cartridge save (SRAM): checksum, region states and the save
 * editor's fields; loading and writing .srm, .ini and .arsave files; and the
 * active slot's storage (attach, validate, flush, legacy migration).
 * Phase: host I/O.
 * Tests: tests/save_system_test.c */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"

enum {
  kActRaiserSramSize = 0x2000,
  kActRaiserSramChecksumOffset = 0x1fec,
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
  char player_name[kActRaiserPlayerNameStorageBytes];
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
  int magic_slots[kActRaiserSaveMagicSlotCount];
  int item_slots[kActRaiserSaveItemSlotCount];
  int scores[kActRaiserSaveRegionCount][kActRaiserSaveActCount];
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
/* Save companions share the native writer's flush/atomic-replace semantics.
 * This writes only the supplied auxiliary path, never live SRAM. The owning
 * codec validates its version/size and coordinates its checkpoint first. */
bool Save_WriteCompanionFile(const char *path, const void *data, size_t size,
                             SaveError *error);

typedef struct SaveSummary {
  char name[kActRaiserPlayerNameStorageBytes * 32];
  int level, acts_cleared, death_heim; /* -1 means unknown, Death Heim: 0/1/4 */
  int towns[kActRaiserSaveRegionCount];
} SaveSummary;
/* Pure file/image inspection; never borrows the live SRAM or name state. */
bool Save_ReadSummary(const char *path, const uint8_t *image, SaveSummary *out);

typedef struct SaveStorageHooks {
  void *context;
  bool (*before_commit)(void *context, SaveError *error);
  void (*committed)(void *context, const uint8_t *image);
  bool (*validate)(void *context, SaveError *error);
} SaveStorageHooks;
void SaveSystem_SetStorageHooks(const SaveStorageHooks *hooks);
bool SaveSystem_ValidateActive(SaveError *error);
/* Rejects incomplete native writes before flushing completed save work. */
bool SaveSystem_FlushForSwitch(SaveError *error);

/* Runtime owner for the one canonical g_sram image. The active backend is
 * snapshotted at boot; changing the corresponding setting takes effect after
 * restart and never redirects writes halfway through a session. */
bool SaveSystem_Attach(uint8_t *live_sram, size_t size,
                       SaveBackend backend,
                       const char *native_path, const char *ini_path,
                       SaveError *error);
/* If the active path does not exist, import one legacy native .srm through the
 * selected backend's validated, atomic codec. The legacy source is retained. */
bool SaveSystem_MigrateLegacyNative(const char *legacy_path,
                                    SaveError *error);
bool SaveSystem_LoadActive(SaveError *error);
/* The host supplies the saves subtree selected by its launcher, and -1 for
 * external diagnostic saves. Attach clears this routing along with live state. */
bool SaveSystem_SetStorageRoot(const char *root,int slot,SaveError *error);
bool SaveSystem_DefaultImportPath(char *out,size_t capacity,SaveError *error);
bool SaveSystem_ExportToLibrary(SaveFileFormat format,bool campaign,SaveError *error);
bool SaveSystem_RecoveryPath(const uint8_t id[16],char *out,size_t capacity,SaveError *error);
bool SaveSystem_WriteActive(SaveError *error);
bool SaveSystem_AutoPersistIfChanged(SaveError *error);
/* Native story saves are multi-write transactions. The game adapter brackets
 * the complete writer, including checksum stores; frame/shutdown polls must
 * not persist intermediate images. An aborted writer stays blocked until a
 * successful reload/reattach, rather than flushing partial SRAM on shutdown. */
bool SaveSystem_BeginNativeWrite(SaveError *error);
bool SaveSystem_EndNativeWrite(bool completed, SaveError *error);
/* Last loaded/successfully persisted canonical image, independent of the
 * session-only auto-persist shadow. False leaves out unchanged (empty slot). */
bool SaveSystem_CopyDurableImage(uint8_t out[kActRaiserSramSize]);

/* Optional world-camera bookmark, separate from battery SRAM. The accepted
 * title boundary calls Begin once: Continue reads a bookmark bound to the
 * exact durable image; New Game keeps visits private until its first story
 * save. Zero means no usable bookmark. Callers must exclude record/replay.
 * Record is O(1), main-thread only; it performs no I/O. Flush is host-owned,
 * retryable and nonfatal: losing a camera bookmark never invalidates a save. */
uint8_t SaveSystem_BeginTownVisits(bool continuing);
void SaveSystem_RecordTownVisit(uint8_t town);
bool SaveSystem_FlushTownVisit(SaveError *error);
typedef enum SaveCommitKind {
  kSaveCommit_Automatic,
  kSaveCommit_Story,
  kSaveCommit_Editor,
  kSaveCommit_Import,
  kSaveCommit_StorySnapshot,
  kSaveCommit_Metadata,
} SaveCommitKind;

typedef enum SaveStorySnapshotResult {
  kSaveStorySnapshot_NotCommitted,
  kSaveStorySnapshot_Committed,
} SaveStorySnapshotResult;
/* Persist a complete, game-owned projection captured at a quiescent story
 * boundary. Unlike an editor/automatic write, this binds the CURRENT campaign
 * metadata. No native writer or previously completed save may be pending.
 * On NotCommitted, live SRAM/durable image/shadow remain unchanged. The host
 * journal may retain a retry candidate, always bound to its exact image.
 * On Committed, the complete snapshot (including the name) is durable before
 * live SRAM/shadow are replaced. Failure leaves the previous snapshot readable.
 * No frame service or callbacks that advance gameplay may run during this call.
 * The image must not alias live SRAM. CPU/cache coherency belongs to the game. */
SaveStorySnapshotResult SaveSystem_CommitStorySnapshot(
    const uint8_t image[kActRaiserSramSize], SaveError *error);

/* Feature codecs supply data; only the save owner writes campaign files.
 * prepare_story freezes feature state at native completion without I/O.
 * build returns the payload for a proposed snapshot without publishing it.
 * validate checks candidate and retained payloads without side effects.
 * committed runs only after the complete snapshot is durable. */
enum { kSaveCampaignPayloadCapacity = 32768 };
typedef struct SaveImportSource {
  const char *path; /* Raw import, with optional checkpoint companion. */
  const uint8_t *payload; /* Complete campaign archive; NULL for raw imports. */
  size_t payload_size;
  bool archive;
} SaveImportSource;
typedef struct SaveCommitHost {
  void *context;
  bool (*prepare_story)(void *context, SaveError *error);
  bool (*build)(void *context, const char *path, const uint8_t *expected,
                const uint8_t *image, SaveCommitKind kind, const SaveImportSource *source,
                void *payload, size_t capacity, size_t *size, SaveError *error);
  bool (*validate)(void *context, const void *payload, size_t size, SaveError *error);
  void (*committed)(void *context, SaveCommitKind kind);
  void (*reloaded)(void *context);
} SaveCommitHost;
/* Attach clears the host. Install after initial load. Caller owns context. */
bool SaveSystem_SetCommitHost(const SaveCommitHost *host);
/* Metadata-only edits preserve durable SRAM and name, and share the normal
 * transaction, validation and slot publication path. Never copy live SRAM. */
bool SaveSystem_UpdateMetadata(const uint8_t expected[kActRaiserSramSize],
                                const void *payload, size_t size, SaveError *error);
void SaveSystem_ResyncShadow(void);
/* Mark a live-SRAM subrange as session-only without concealing unrelated
 * game writes. A later native save still persists the complete live image. */
void SaveSystem_ResyncShadowRange(size_t offset, size_t size);
const char *SaveSystem_ActivePath(void);
SaveBackend SaveSystem_ActiveBackend(void);
/* Snapshot the current native player name as printable UTF-8/ASCII. This is a
 * read-only semantic accessor for localization placeholders; callers never
 * receive the live SRAM pointer or its USA-specific offset. */
bool SaveSystem_CopyPlayerName(char *destination, size_t capacity);
/* Optional host extension associated with the active save checksum and native
 * compatibility name. The caller supplies the current live name, which can
 * precede battery SRAM during a new game. No UTF-8 is written into SRAM. */
bool SaveSystem_CopyLocalizedPlayerName(const char *compatibility_name,
                                        char *destination, size_t capacity);
/* Stage up to eight Unicode grapheme clusters. Persistence waits until native
 * SRAM contains the compatibility name; loading another save replaces this
 * session metadata. The caller must observe native name-entry acceptance. */
bool SaveSystem_SetLocalizedPlayerName(const char *utf8_name,
                                       const char *compatibility_name);

/* Every request field contains -1 for unchanged or its documented value. A
 * persistent edit backs up and atomically writes the active format before the
 * scratch image replaces live SRAM. Session-only edits only replace live SRAM.
 * Both paths repair the checksum and re-sync the auto-persist shadow.
 * Success means the complete snapshot, including its saved name, is durable.
 * Failure leaves the previous complete snapshot readable on a cold load. */
bool SaveSystem_ApplyEdits(const SaveEditRequest *edits,
                           bool armed, bool persist,
                           bool auto_backup, SaveError *error);
/* Compatibility helper retained for focused region-state tests/callers. */
bool SaveSystem_ApplyRegionEdits(const int edits[kActRaiserSaveRegionCount],
                                 bool armed, bool persist,
                                 bool auto_backup, SaveError *error);
/* True means SRAM, feature metadata and name committed together. */
bool SaveSystem_Import(const char *path, bool auto_backup, SaveError *error);
/* Atomic .arsave archive of the last durable campaign: canonical SRAM,
 * validated feature payload and enhanced name. Imports rebind its slot only.
 * Unsaved/session-only gameplay is not included. */
bool SaveSystem_ExportCampaign(const char *path, SaveError *error);
/* Emulator-compatible raw export. Intentionally excludes feature metadata. */
bool SaveSystem_Export(SaveFileFormat format, const char *path,
                       SaveError *error);
/* Permanent recovery copy of an already completed/persisted native save.
 * The game must first persist current WRAM (including active actor caches)
 * as a coherent story image; this API does not save unsaved gameplay.
 * Reject pending/dirty/session-only images and externally changed disk files.
 * Exclusively create directory; never reuse/overwrite an existing directory.
 * Writes save.srm and its complete snapshot journal, native image last.
 * Failure may leave a partial directory; retain it and choose a new name on
 * retry. No live state, active files or auto-persist shadow are modified.
 * A completed copy is restored by replacing the active save and companions
 * together while the game is closed. Native SRAM remains emulator compatible. */
bool SaveSystem_CreateRecoveryCopy(const char *directory, SaveError *error);

#endif  /* AR_SAVE_SYSTEM_H */
