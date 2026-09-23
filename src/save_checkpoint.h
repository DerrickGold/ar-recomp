#ifndef AR_SAVE_CHECKPOINT_H
#define AR_SAVE_CHECKPOINT_H

#include "save_system.h"

enum { kSaveCheckpointPayloadMax = 8192 };

typedef enum SaveCheckpointStatus {
  kSaveCheckpoint_Ready,
  kSaveCheckpoint_Missing,
  kSaveCheckpoint_Mismatch,
  kSaveCheckpoint_Invalid,
  kSaveCheckpoint_Unsupported,
  kSaveCheckpoint_IoError,
} SaveCheckpointStatus;

typedef SaveCheckpointStatus (*SaveCheckpointValidatePayload)(
    const uint8_t *payload, size_t size, void *context);

/* Two bounded checkpoints: current candidate and the last matching durable
 * save. Match all 8192 bytes, including bytes outside the retail checksum.
 * Companion path is native_path + ".archeckpoint". Opaque payloads belong to
 * the feature codec, not this storage owner. All
 * non-Ready reads leave payload/size untouched; Missing is not corrupt data. */
SaveCheckpointStatus SaveCheckpoint_Read(const char *native_path,
    const uint8_t image[kActRaiserSramSize], void *payload, size_t capacity,
    size_t *size, SaveError *error);

/* Called only at a confirmed completed-save boundary, never on a partial SRAM
 * write or merely entering New Game. expected is the caller's durable image,
 * or NULL when no native file exists. Reject external replacement/stale state.
 *
 * Journal first, native file second. A failure/crash between replacements
 * leaves BOTH metadata versions recoverable: cold load chooses the exact
 * native image that actually reached disk. Identical-image metadata updates
 * need only one atomic journal write. A future successful save retains one
 * previous image; older rollbacks report Mismatch, not guessed provenance.
 * No locks: one writer owns a slot. Output live SRAM is
 * never mutated. Native and INI formats use the same canonical image.
 *
 * The feature validator is required and checks the candidate AND all retained
 * records before mutation (zero-sized retained records explicitly mark legacy
 * images without metadata). Unknown feature schemas cannot be discarded while
 * rotating the journal. It must be pure: no file writes or live-state edits. */
bool SaveCheckpoint_Commit(SaveFileFormat format, const char *native_path,
    const uint8_t *expected,
    const uint8_t image[kActRaiserSramSize], const void *payload, size_t size,
    SaveCheckpointValidatePayload validate, void *context,
    SaveError *error);

#endif
