#include "save/save_checkpoint.h"

#include "byte_order.h"
#include "deterministic_hash.h"
#include "snesrecomp/support/utf8_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  kHeaderBytes = 12,
  kRecordHeaderBytes = 8,
  kHashBytes = 8,
  kJournalMax = kHeaderBytes + 2 * (kRecordHeaderBytes + kActRaiserSramSize +
                                  kSaveCheckpointPayloadMax + kSaveNameCapacity - 1) + kHashBytes,
};
static const uint8_t kMagic[8] = {'A', 'R', 'C', 'H', 'E', 'C', 'K', 0};

typedef struct CheckpointRecord {
  const uint8_t *image, *payload;
  size_t size;
  bool name_known;
  char name[kSaveNameCapacity];
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
  if (failed)
    return Fail(error, kSaveCheckpoint_IoError, "error reading save checkpoint companion");
  const uint8_t *bytes = journal->bytes;
  if (extra != EOF || size < kHeaderBytes + kHashBytes || memcmp(bytes, kMagic, sizeof(kMagic)))
    return Fail(error, kSaveCheckpoint_Invalid, "invalid save checkpoint header or length");
  const unsigned version = ByteOrder_ReadLe16(bytes + 8);
  if (version != 1 && version != 2)
    return Fail(error, kSaveCheckpoint_Unsupported,
                "unsupported save checkpoint version; companion preserved");
  unsigned count = ByteOrder_ReadLe16(bytes + 10);
  uint64_t hash = DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET,
                                          bytes, size - kHashBytes);
  uint64_t stored = ByteOrder_ReadLe32(bytes + size - 8) |
      ((uint64_t)ByteOrder_ReadLe32(bytes + size - 4) << 32);
  if (!count || count > 2 || hash != stored)
    return Fail(error, kSaveCheckpoint_Invalid, "damaged save checkpoint companion");
  size_t offset = kHeaderBytes, end = size - kHashBytes;
  const size_t record_header = version == 1 ? 4 : kRecordHeaderBytes;
  for (unsigned i = 0; i < count; ++i) {
    if (end - offset < record_header + kActRaiserSramSize)
      return Fail(error, kSaveCheckpoint_Invalid, "truncated save checkpoint record");
    size_t payload_size = ByteOrder_ReadLe32(bytes + offset);
    size_t name_size = version == 2 ? ByteOrder_ReadLe16(bytes + offset + 4) : 0;
    unsigned flags = version == 2 ? ByteOrder_ReadLe16(bytes + offset + 6) : 0;
    if (flags > 1 || name_size >= kSaveNameCapacity || (name_size && !flags))
      return Fail(error, kSaveCheckpoint_Invalid, "invalid checkpoint name header");
    offset += record_header;
    const uint8_t *image = bytes + offset;
    offset += kActRaiserSramSize;
    if (payload_size > kSaveCheckpointPayloadMax || payload_size > end - offset ||
        name_size > end - offset - payload_size || !ValidImage(image))
      return Fail(error, kSaveCheckpoint_Invalid, "invalid save checkpoint record");
    CheckpointRecord *record = &journal->records[i];
    *record = (CheckpointRecord){.image = image, .payload = bytes + offset,
                                 .size = payload_size, .name_known = flags == 1};
    offset += payload_size;
    memcpy(record->name, bytes + offset, name_size);
    offset += name_size;
    char native[kActRaiserPlayerNameStorageBytes];
    if (name_size && (strlen(record->name) != name_size || !SaveName_Valid(record->name) ||
                     !SaveName_CopyNative(image, native, sizeof(native))))
      return Fail(error, kSaveCheckpoint_Invalid, "invalid checkpoint player name");
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
    if (!record)
      status = Fail(error, kSaveCheckpoint_Mismatch,
                    "no checkpoint matches this save; recovery required");
    else if (!record->size)
      status = kSaveCheckpoint_Missing; /* Retained legacy save, no metadata yet. */
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

bool SaveCheckpoint_IsLegacySnapshot(const char *native_path, const uint8_t *image) {
  char path[kHostPathCapacity];
  if (!ValidImage(image) || !CompanionPath(native_path, path, sizeof(path))) return false;
  CheckpointJournal *journal = malloc(sizeof(*journal));
  if (!journal) return false;
  const CheckpointRecord *record = ReadJournal(path, journal, NULL) == kSaveCheckpoint_Ready
      ? Find(journal, image) : NULL;
  bool legacy = record && record->name_known && !record->size;
  free(journal);
  return legacy;
}

bool SaveCheckpoint_ReadSnapshot(const char *native_path, const uint8_t *image,
                                 SaveSnapshot *out, SaveError *error) {
  char path[kHostPathCapacity];
  if (!out || !ValidImage(image) || !CompanionPath(native_path, path, sizeof(path))) {
    Fail(error, kSaveCheckpoint_Invalid, "invalid campaign snapshot read");
    return false;
  }
  CheckpointJournal *journal = malloc(sizeof(*journal));
  if (!journal) {
    Fail(error, kSaveCheckpoint_IoError, "out of memory reading campaign snapshot");
    return false;
  }
  SaveCheckpointStatus status = ReadJournal(path, journal, error);
  const CheckpointRecord *record = status == kSaveCheckpoint_Ready ? Find(journal, image) : NULL;
  bool ok = status == kSaveCheckpoint_Missing || (status == kSaveCheckpoint_Ready && record);
  if (status == kSaveCheckpoint_Ready && !record)
    Fail(error, kSaveCheckpoint_Mismatch, "no checkpoint matches this save; recovery required");
  char name[kSaveNameCapacity] = {0};
  if (ok) {
    if (record && record->name_known)
      memcpy(name, record->name, sizeof(name));
    else
      ok = SaveName_ReadLegacy(native_path, image, name, error);
  }
  if (ok) {
    memmove(out->image, image, kActRaiserSramSize);
    memset(out->payload, 0, sizeof(out->payload));
    if (record && record->size) memcpy(out->payload, record->payload, record->size);
    out->payload_size = record ? record->size : 0;
    memcpy(out->name, name, sizeof(name));
  }
  free(journal);
  return ok;
}

static size_t WriteRecord(uint8_t *out, const CheckpointRecord *record) {
  const size_t name_size = strlen(record->name);
  ByteOrder_WriteLe32(out, (uint32_t)record->size);
  ByteOrder_WriteLe16(out + 4, (uint16_t)name_size);
  ByteOrder_WriteLe16(out + 6, record->name_known ? 1 : 0);
  memcpy(out + kRecordHeaderBytes, record->image, kActRaiserSramSize);
  if (record->size)
    memcpy(out + kRecordHeaderBytes + kActRaiserSramSize, record->payload, record->size);
  memcpy(out + kRecordHeaderBytes + kActRaiserSramSize + record->size, record->name, name_size);
  return kRecordHeaderBytes + kActRaiserSramSize + record->size + name_size;
}

bool SaveCheckpoint_CommitSnapshot(SaveFileFormat format, const char *native_path,
    const uint8_t *expected, const SaveSnapshot *next,
    SaveCheckpointValidatePayload validate, void *context, SaveError *error) {
  const uint8_t *image = next ? next->image : NULL;
  const void *payload = next ? next->payload : NULL;
  const size_t size = next ? next->payload_size : 0;
  if (error) error->message[0] = 0;
  char path[kHostPathCapacity];
  if ((format != kSaveFileFormat_NativeSrm && format != kSaveFileFormat_Ini) ||
      !CompanionPath(native_path, path, sizeof(path)) ||
      !ValidImage(image) || (expected && !ValidImage(expected)) ||
      size > kSaveCheckpointPayloadMax || (size && !validate) ||
      !memchr(next->name, 0, sizeof(next->name))) {
    Fail(error, kSaveCheckpoint_Invalid, "invalid save checkpoint commit request");
    return false;
  }
  char native[kActRaiserPlayerNameStorageBytes];
  if (next->name[0] && (!SaveName_Valid(next->name) ||
                       !SaveName_CopyNative(image, native, sizeof(native)))) {
    Fail(error, kSaveCheckpoint_Invalid, "invalid candidate campaign name");
    return false;
  }
  SaveCheckpointStatus validation = size ? validate(payload, size, context) : kSaveCheckpoint_Ready;
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
      Fail(error, kSaveCheckpoint_Mismatch,
           "save changed since checkpoint load; reload before saving");
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
    validation = validate ? validate(journal->records[i].payload, journal->records[i].size, context)
                          : kSaveCheckpoint_Unsupported;
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
        !memcmp(prepared->payload, payload, size) && prepared->name_known &&
        !strcmp(prepared->name, next->name))) {
    Fail(error, kSaveCheckpoint_Mismatch,
         "unmatched companion preserved; explicit recovery required");
    goto done;
  }
  bool same_image = expected && !memcmp(expected, image, kActRaiserSramSize);
  /* Preserve a pre-feature save too: if native replacement fails, it remains
   * legacy Missing rather than becoming an unexplained mismatched companion. */
  CheckpointRecord legacy = {.image = expected};
  if (!previous && expected && status == kSaveCheckpoint_Missing) previous = &legacy;
  CheckpointRecord adopted;
  if (previous && !previous->name_known) {
    adopted = *previous;
    if (!SaveName_ReadLegacy(native_path, expected, adopted.name, error)) goto done;
    adopted.name_known = true;
    previous = &adopted;
  }
  const CheckpointRecord *retained = same_image ? NULL : previous;
  if (same_image) {
    for (unsigned i = 0; i < journal->count; ++i)
      if (memcmp(image, journal->records[i].image, kActRaiserSramSize))
        retained = &journal->records[i];
  }
  CheckpointRecord candidate = {.image = image, .payload = payload, .size = size,
                                 .name_known = true};
  memcpy(candidate.name, next->name, sizeof(candidate.name));
  memcpy(encoded, kMagic, sizeof(kMagic));
  ByteOrder_WriteLe16(encoded + 8, 2);
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

/* Payload-only fixture/migration entry point. It still uses the complete
 * snapshot writer and preserves a name only when its native mirror matches. */
bool SaveCheckpoint_Commit(SaveFileFormat format, const char *path,
    const uint8_t *expected, const uint8_t *image, const void *payload, size_t size,
    SaveCheckpointValidatePayload validate, void *context, SaveError *error) {
  if (!image || !payload || !size || size > kSaveCheckpointPayloadMax) return false;
  SaveSnapshot *next = calloc(1, sizeof(*next));
  if (!next) return false;
  bool ok = !expected || SaveCheckpoint_ReadSnapshot(path, expected, next, error);
  char before[kActRaiserPlayerNameStorageBytes], after[kActRaiserPlayerNameStorageBytes];
  if (!expected || !SaveName_CopyNative(expected, before, sizeof(before)) ||
      !SaveName_CopyNative(image, after, sizeof(after)) || strcmp(before, after)) next->name[0] = 0;
  memcpy(next->image, image, kActRaiserSramSize);
  memcpy(next->payload, payload, size);
  next->payload_size = size;
  if (ok)
    ok = SaveCheckpoint_CommitSnapshot(format, path, expected, next, validate, context, error);
  free(next);
  return ok;
}
