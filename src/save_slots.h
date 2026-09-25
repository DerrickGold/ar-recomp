#ifndef AR_SAVE_SLOTS_H
#define AR_SAVE_SLOTS_H

#include "save_system.h"

enum { kSaveSlotCount = 10, kSaveSlotPathCapacity = 512, kSaveSlotDraftCapacity = 32768 };
typedef enum SaveSlotState { kSaveSlot_Empty, kSaveSlot_Ready, kSaveSlot_Unavailable } SaveSlotState;
typedef struct SaveSlotRecord {
  SaveBackend backend;
  bool ever_saved, prepared, checkpoint_required;
  SaveBackend prepared_backend;
  uint64_t saved_at, saved_image;
} SaveSlotRecord;
typedef struct SaveSlotInspection {
  SaveSlotState state;
  uint64_t fingerprint, modified_at;
  bool approximate_time;
  uint8_t image[kActRaiserSramSize];
  char path[kSaveSlotPathCapacity];
  SaveError error;
} SaveSlotInspection;
/* One collection owner per process. The lock survives until Close and is not
 * inherited by exec. Paths and backend are pinned, never chosen by file age. */
typedef struct SaveSlots {
  char root[kSaveSlotPathCapacity];
  SaveSlotRecord records[kSaveSlotCount];
  unsigned active, previous, destination;
  unsigned layout; /* 2 while migration is pending, 3 after publication. */
  bool legacy_native; /* Pending adoption of the historical actraiser.srm. */
  bool pending, new_game, adopted, dirty, first_write_in_progress;
  uint64_t expected;
  void *lock;
} SaveSlots;

bool SaveSlots_Open(SaveSlots *slots, const char *root, SaveBackend legacy_backend, SaveError *error);
/* Persist evidence that a companion exists, including metadata-only upgrades. */
bool SaveSlots_ObserveCheckpoints(SaveSlots *slots,SaveError *error);
void SaveSlots_Close(SaveSlots *slots);
bool SaveSlots_Paths(const SaveSlots *slots, unsigned slot, char *native, char *ini, size_t capacity);
bool SaveSlots_Inspect(const SaveSlots *slots, unsigned slot, SaveSlotInspection *out);
bool SaveSlots_ReadDraft(const SaveSlots *slots, unsigned slot, void *out, size_t capacity, size_t *size, SaveError *error);
/* Update the active empty slot's setup without scheduling a restart. */
bool SaveSlots_UpdateDraft(SaveSlots *slots, const void *draft, size_t size, SaveError *error);
/* A prepared format does not redirect the live source writer. It becomes the
 * committed format only when the destination acknowledges a successful boot. */
SaveBackend SaveSlots_DestinationBackend(const SaveSlots *slots);
/* Inspections are immutable review tokens. Revalidation includes both save
 * companions and any prepared draft. Source remains active until next boot. */
bool SaveSlots_Request(SaveSlots *slots, unsigned slot, uint64_t inspected,
    const void *draft, size_t draft_size, SaveBackend new_backend, SaveError *error);
bool SaveSlots_ValidateDestination(const SaveSlots *slots, SaveError *error);
bool SaveSlots_Acknowledge(SaveSlots *slots, SaveError *error);
bool SaveSlots_ReturnToPrevious(SaveSlots *slots, SaveError *error);
/* The first-write intent reaches disk BEFORE any native image can be written.
 * Timestamp publication follows a successful commit and is retryable. */
bool SaveSlots_BeforeCommit(SaveSlots *slots, SaveError *error);
void SaveSlots_DidCommit(SaveSlots *slots, const uint8_t *image);
bool SaveSlots_Flush(SaveSlots *slots, SaveError *error);

#endif
