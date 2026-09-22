#include "save_checkpoint.h"

#include "byte_order.h"
#include "deterministic_hash.h"
#include "snesrecomp/support/utf8_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kHeaderBytes = 12,
  kRecordHeaderBytes = 4,
  kHashBytes = 8,
  kJournalMax = kHeaderBytes + 2 * (kRecordHeaderBytes + kActRaiserSramSize +
                                  kSaveCheckpointPayloadMax) + kHashBytes,
};
static const uint8_t kMagic[8] = {'A', 'R', 'C', 'H', 'E', 'C', 'K', 0};

typedef struct CheckpointRecord {
  const uint8_t *image, *payload;
  size_t size;
} CheckpointRecord;

typedef struct CheckpointJournal {
  uint8_t bytes[kJournalMax];
  CheckpointRecord records[2];
  unsigned count;
} CheckpointJournal;

static SaveCheckpointStatus Fail(SaveError *error, SaveCheckpointStatus status,
                                 const char *message) {
  if (error) snprintf(error->message, sizeof(error->message), "%s", message);
  return status;
}

static bool ValidImage(const uint8_t *image) {
  return image && Save_ChecksumValid(image);
}

static bool CompanionPath(const char *native_path, char *path, size_t capacity) {
  if (!native_path || !native_path[0]) return false;
  int written = snprintf(path, capacity, "%s.archeckpoint", native_path);
  return written > 0 && (size_t)written < capacity;
}

static SaveCheckpointStatus ReadJournal(const char *path, CheckpointJournal *journal,
                                        SaveError *error) {
  journal->count = 0;
  FILE *file = sr_fopen(path, "rb");
  if (!file) {
    if (errno == ENOENT) return kSaveCheckpoint_Missing;
    return Fail(error, kSaveCheckpoint_IoError, "cannot read save checkpoint companion");
  }
  size_t size = fread(journal->bytes, 1, sizeof(journal->bytes), file);
  bool failed = ferror(file) != 0;
  const int extra = fgetc(file);
  failed |= ferror(file) != 0;
  if (fclose(file) != 0) failed = true;
  if (failed) return Fail(error, kSaveCheckpoint_IoError, "error reading save checkpoint companion");
  const uint8_t *bytes = journal->bytes;
  if (extra != EOF || size < kHeaderBytes + kHashBytes || memcmp(bytes, kMagic, sizeof(kMagic)))
    return Fail(error, kSaveCheckpoint_Invalid, "invalid save checkpoint header or length");
  if (ByteOrder_ReadLe16(bytes + 8) != 1)
    return Fail(error, kSaveCheckpoint_Unsupported, "unsupported save checkpoint version; companion preserved");
  unsigned count = ByteOrder_ReadLe16(bytes + 10);
  uint64_t hash = DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET,
                                          bytes, size - kHashBytes);
  uint64_t stored = ByteOrder_ReadLe32(bytes + size - 8) |
      ((uint64_t)ByteOrder_ReadLe32(bytes + size - 4) << 32);
  if (!count || count > 2 || hash != stored)
    return Fail(error, kSaveCheckpoint_Invalid, "damaged save checkpoint companion");
  size_t offset = kHeaderBytes, end = size - kHashBytes;
  for (unsigned i = 0; i < count; ++i) {
    if (end - offset < kRecordHeaderBytes + kActRaiserSramSize)
      return Fail(error, kSaveCheckpoint_Invalid, "truncated save checkpoint record");
    size_t payload_size = ByteOrder_ReadLe32(bytes + offset);
    offset += kRecordHeaderBytes;
    const uint8_t *image = bytes + offset;
    offset += kActRaiserSramSize;
    if (payload_size > kSaveCheckpointPayloadMax ||
        payload_size > end - offset || !ValidImage(image))
      return Fail(error, kSaveCheckpoint_Invalid, "invalid save checkpoint record");
    journal->records[i] = (CheckpointRecord){image, bytes + offset, payload_size};
    offset += payload_size;
  }
  if (offset != end || (count == 2 &&
      !memcmp(journal->records[0].image, journal->records[1].image, kActRaiserSramSize)))
    return Fail(error, kSaveCheckpoint_Invalid, "ambiguous or trailing save checkpoint data");
  journal->count = count;
  return kSaveCheckpoint_Ready;
}

static const CheckpointRecord *Find(const CheckpointJournal *journal, const uint8_t *image) {
  for (unsigned i = 0; image && i < journal->count; ++i)
    if (!memcmp(image, journal->records[i].image, kActRaiserSramSize)) return &journal->records[i];
  return NULL;
}

SaveCheckpointStatus SaveCheckpoint_Read(const char *native_path, const uint8_t *image,
    void *payload, size_t capacity, size_t *size, SaveError *error) {
  if (error) error->message[0] = 0;
  char path[kHostPathCapacity];
  if (!CompanionPath(native_path, path, sizeof(path)) || !ValidImage(image) || !payload || !size)
    return Fail(error, kSaveCheckpoint_Invalid, "invalid save checkpoint read request");
  CheckpointJournal *journal = malloc(sizeof(*journal));
  if (!journal) return Fail(error, kSaveCheckpoint_IoError, "out of memory reading checkpoint");
  SaveCheckpointStatus status = ReadJournal(path, journal, error);
  if (status == kSaveCheckpoint_Ready) {
    const CheckpointRecord *record = Find(journal, image);
    if (!record) status = Fail(error, kSaveCheckpoint_Mismatch, "no checkpoint matches this save; recovery required");
    else if (!record->size) status = kSaveCheckpoint_Missing; /* Retained legacy save, no metadata yet. */
    else if (record->size > capacity)
      status = Fail(error, kSaveCheckpoint_Invalid, "save checkpoint payload buffer is too small");
    else {
      memcpy(payload, record->payload, record->size);
      *size = record->size;
    }
  }
  free(journal);
  return status;
}

static size_t WriteRecord(uint8_t *out, const CheckpointRecord *record) {
  ByteOrder_WriteLe32(out, (uint32_t)record->size);
  memcpy(out + kRecordHeaderBytes, record->image, kActRaiserSramSize);
  if (record->size) memcpy(out + kRecordHeaderBytes + kActRaiserSramSize, record->payload, record->size);
  return kRecordHeaderBytes + kActRaiserSramSize + record->size;
}

bool SaveCheckpoint_Commit(SaveFileFormat format, const char *native_path,
    const uint8_t *expected, const uint8_t *image,
    const void *payload, size_t size, SaveCheckpointValidatePayload validate,
    void *context, SaveError *error) {
  if (error) error->message[0] = 0;
  char path[kHostPathCapacity];
  if ((format != kSaveFileFormat_NativeSrm && format != kSaveFileFormat_Ini) ||
      !CompanionPath(native_path, path, sizeof(path)) ||
      !ValidImage(image) || (expected && !ValidImage(expected)) || !payload ||
      !size || size > kSaveCheckpointPayloadMax || !validate) {
    Fail(error, kSaveCheckpoint_Invalid, "invalid save checkpoint commit request");
    return false;
  }
  SaveCheckpointStatus validation = validate(payload, size, context);
  if (validation != kSaveCheckpoint_Ready) {
    Fail(error, validation, "invalid or unsupported candidate checkpoint payload");
    return false;
  }
  /* Check the actual disk image, not the save system's session-only resync
   * shadow. Do not overwrite a file changed outside this writer's session. */
  uint8_t disk[kActRaiserSramSize];
  if (expected) {
    if (!Save_LoadFile(format, native_path, disk, error)) return false;
    if (memcmp(expected, disk, sizeof(disk))) {
      Fail(error, kSaveCheckpoint_Mismatch, "save changed since checkpoint load; reload before saving");
      return false;
    }
  } else {
    FILE *probe = sr_fopen(native_path, "rb");
    const bool exists = probe != NULL;
    if (probe) fclose(probe);
    if (exists || errno != ENOENT) {
      Fail(error, kSaveCheckpoint_Mismatch, "expected an empty save slot; existing file preserved");
      return false;
    }
  }
  CheckpointJournal *journal = malloc(sizeof(*journal));
  uint8_t *encoded = malloc(kJournalMax);
  bool success = false;
  if (!journal || !encoded) {
    Fail(error, kSaveCheckpoint_IoError, "out of memory preparing checkpoint");
    goto done;
  }
  SaveCheckpointStatus status = ReadJournal(path, journal, error);
  if (status != kSaveCheckpoint_Ready && status != kSaveCheckpoint_Missing) goto done;
  for (unsigned i = 0; i < journal->count; ++i) {
    if (!journal->records[i].size) continue; /* Explicit legacy-image marker. */
    validation = validate(journal->records[i].payload, journal->records[i].size, context);
    if (validation != kSaveCheckpoint_Ready) {
      Fail(error, validation, "incompatible retained checkpoint payload; companion preserved");
      goto done;
    }
  }
  const CheckpointRecord *previous = Find(journal, expected);
  /* A first-save retry may have left a prepared candidate but no native file.
   * Only the exact same candidate is idempotent; never discard another slot. */
  const CheckpointRecord *prepared = expected ? NULL : Find(journal, image);
  if (status == kSaveCheckpoint_Ready && !previous &&
      !(prepared && journal->count == 1 && prepared->size == size &&
        !memcmp(prepared->payload, payload, size))) {
    Fail(error, kSaveCheckpoint_Mismatch, "unmatched companion preserved; explicit recovery required");
    goto done;
  }
  bool same_image = expected && !memcmp(expected, image, kActRaiserSramSize);
  /* Preserve a pre-feature save too: if native replacement fails, it remains
   * legacy Missing rather than becoming an unexplained mismatched companion. */
  const CheckpointRecord legacy = {expected, NULL, 0};
  if (!previous && expected && status == kSaveCheckpoint_Missing) previous = &legacy;
  const CheckpointRecord *retained = same_image ? NULL : previous;
  if (same_image) {
    for (unsigned i = 0; i < journal->count; ++i)
      if (memcmp(image, journal->records[i].image, kActRaiserSramSize)) retained = &journal->records[i];
  }
  const CheckpointRecord candidate = {image, payload, size};
  memcpy(encoded, kMagic, sizeof(kMagic));
  ByteOrder_WriteLe16(encoded + 8, 1);
  ByteOrder_WriteLe16(encoded + 10, retained ? 2 : 1);
  size_t bytes = kHeaderBytes + WriteRecord(encoded + kHeaderBytes, &candidate);
  if (retained) bytes += WriteRecord(encoded + bytes, retained);
  uint64_t hash = DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET, encoded, bytes);
  ByteOrder_WriteLe32(encoded + bytes, (uint32_t)hash);
  ByteOrder_WriteLe32(encoded + bytes + 4, (uint32_t)(hash >> 32));
  if (!Save_WriteCompanionFile(path, encoded, bytes + kHashBytes, error)) goto done;
  success = same_image || Save_WriteFile(format, native_path, image, error);
done:
  free(encoded);
  free(journal);
  return success;
}
