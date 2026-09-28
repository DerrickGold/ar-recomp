#include "save/save_checkpoint.h"
#include "byte_order.h"
#include "deterministic_hash.h"
#include "snesrecomp/support/utf8_fs.h"
#include "support/test_assert.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define REMOVE_DIR(path) _rmdir(path)
#else
#include <unistd.h>
#define REMOVE_DIR(path) rmdir(path)
#endif

typedef struct Fixture {
  const SaveSnapshot *expected;
  const char *path;
  SaveFileFormat format;
  unsigned published, notified;
  bool reject;
} Fixture;

static bool Validate(void *context, const void *payload, size_t size, SaveError *error) {
  (void)context;
  (void)error;
  return size == 1 && *(const uint8_t *)payload < 128;
}
static SaveCheckpointStatus ValidateRecord(const uint8_t *payload, size_t size, void *context) {
  return Validate(context, payload, size, NULL) ? kSaveCheckpoint_Ready : kSaveCheckpoint_Invalid;
}
static bool Prepare(void *context, SaveError *error) {
  (void)context;
  (void)error;
  return true;
}
static void Reloaded(void *context) { (void)context; }
static bool Build(void *context, const char *path, const uint8_t *expected,
                  const uint8_t *image, SaveCommitKind kind, const SaveImportSource *source,
                  void *payload, size_t capacity, size_t *size, SaveError *error) {
  (void)context;
  assert(capacity >= 1);
  if (kind == kSaveCommit_Story || kind == kSaveCommit_StorySnapshot) {
    *(uint8_t *)payload = 20;
  } else {
    SaveSnapshot saved;
    if (!SaveCheckpoint_ReadSnapshot(source ? source->path : path,
                                      source ? image : expected, &saved, error)) return false;
    assert(saved.payload_size == 1);
    *(uint8_t *)payload = saved.payload[0];
  }
  *size = 1;
  return true;
}
static void Committed(void *context, SaveCommitKind kind) {
  (void)kind;
  ++((Fixture *)context)->notified;
}
static bool BeforeCommit(void *context, SaveError *error) {
  Fixture *fixture = context;
  if (fixture->reject && error)
    snprintf(error->message, sizeof(error->message), "injected routing failure");
  return !fixture->reject;
}
static void AssertSnapshot(const SaveSnapshot *actual, const SaveSnapshot *expected) {
  assert(!memcmp(actual->image, expected->image, kActRaiserSramSize));
  assert(actual->payload_size == expected->payload_size);
  assert(!memcmp(actual->payload, expected->payload, actual->payload_size));
  assert(!strcmp(actual->name, expected->name));
}
static void Published(void *context, const uint8_t *image) {
  Fixture *fixture = context;
  uint8_t disk[kActRaiserSramSize];
  SaveSnapshot actual;
  SaveError error = {{0}};
  assert(Save_LoadFile(fixture->format, fixture->path, disk, &error));
  assert(!memcmp(disk, image, sizeof(disk)));
  assert(SaveCheckpoint_ReadSnapshot(fixture->path, disk, &actual, &error));
  AssertSnapshot(&actual, fixture->expected); /* Notification cannot precede the name. */
  ++fixture->published;
}
static SaveSnapshot Snapshot(const char *native, const char *name, unsigned payload) {
  SaveSnapshot result = {.payload_size = 1};
  result.payload[0] = (uint8_t)payload;
  memcpy(result.image + 0x1439, native, strlen(native));
  result.image[0x1442] = 10;
  Save_RecomputeChecksum(result.image);
  snprintf(result.name, sizeof(result.name), "%s", name);
  return result;
}
static void Remove(const char *path) {
  char extra[256];
  remove(path);
  snprintf(extra, sizeof(extra), "%s.archeckpoint", path);
  remove(extra);
  snprintf(extra, sizeof(extra), "%s.arname", path);
  remove(extra);
}

enum { kAutomatic, kStory, kEditor, kImport, kSnapshot, kMetadata, kName, kOperations };
static void Transaction(SaveFileFormat format, unsigned operation, unsigned fault) {
  const char *path = format == kSaveFileFormat_Ini ? "snapshot-matrix.ini" : "snapshot-matrix.srm";
  const char *donor = "snapshot-matrix-donor.srm";
  Remove(path);
  Remove(donor);
  SaveSnapshot before = Snapshot("ELISE", "Élise", 10);
  SaveSnapshot next = Snapshot("RENEE", "Renée", 20);
  next.image[0x1442] = 11;
  Save_RecomputeChecksum(next.image);
  SaveError error = {{0}};
  assert(SaveCheckpoint_CommitSnapshot(format, path, NULL, &before, ValidateRecord, NULL, &error));
  assert(SaveCheckpoint_CommitSnapshot(kSaveFileFormat_NativeSrm, donor, NULL, &next,
                                       ValidateRecord, NULL, &error));
  if (operation == kAutomatic || operation == kEditor) next.payload[0] = 10;
  if (operation == kEditor) next.name[0] = 0;
  if (operation == kMetadata || operation == kName) {
    next = before;
    if (operation == kMetadata) next.payload[0] = 20;
    else snprintf(next.name, sizeof(next.name), "Éli");
  }
  uint8_t live[kActRaiserSramSize];
  assert(SaveSystem_Attach(live, sizeof(live), (SaveBackend)format, path, path, &error));
  assert(SaveSystem_LoadActive(&error));
  Fixture fixture = {.path = path, .format = format, .expected = &next, .reject = fault == 0};
  SaveCommitHost codec = {.context = &fixture, .prepare_story = Prepare, .build = Build,
      .validate = Validate, .committed = Committed, .reloaded = Reloaded};
  assert(SaveSystem_SetCommitHost(&codec));
  SaveStorageHooks hooks = {.context = &fixture, .before_commit = BeforeCommit,
                            .committed = Published};
  SaveSystem_SetStorageHooks(&hooks);
  char blocked[256];
  snprintf(blocked, sizeof(blocked), "%s%s.tmp", path, fault == 1 ? ".archeckpoint" : "");
  if (fault == 1 || fault == 2) assert(sr_mkdir(blocked) == 0);
  bool result = false;
  if (operation == kEditor) {
    SaveEditRequest edits;
    SaveEditRequest_Clear(&edits);
    edits.player_name_set = true;
    snprintf(edits.player_name, sizeof(edits.player_name), "RENEE");
    edits.master_level = 11;
    result = SaveSystem_ApplyEdits(&edits, true, true, false, &error);
  } else if (operation == kImport) {
    result = SaveSystem_Import(donor, false, &error);
  } else if (operation == kMetadata) {
    /* Metadata-only persistence cannot capture these unsaved live bytes/name. */
    live[100] = 99;
    Save_RecomputeChecksum(live);
    SaveSystem_ResyncShadow();
    assert(SaveSystem_SetLocalizedPlayerName("Renée", "RENEE"));
    result = SaveSystem_UpdateMetadata(before.image, next.payload, next.payload_size, &error);
  } else {
    assert(SaveSystem_SetLocalizedPlayerName(next.name, operation == kName ? "ELISE" : "RENEE"));
    if (operation == kSnapshot) {
      result = SaveSystem_CommitStorySnapshot(next.image, &error) == kSaveStorySnapshot_Committed;
    } else {
      if (operation == kStory) assert(SaveSystem_BeginNativeWrite(&error));
      memcpy(live, next.image, sizeof(live));
      if (operation == kStory) {
        assert(SaveSystem_EndNativeWrite(true, &error));
        live[100] = 77;
        Save_RecomputeChecksum(live); /* Later live changes cannot alter the completed save. */
      }
      result = SaveSystem_AutoPersistIfChanged(&error);
    }
  }
  const bool same_image = operation == kMetadata || operation == kName;
  const bool success = fault == 3 || (fault == 2 && same_image);
  if (result != success)
    fprintf(stderr, "format=%d operation=%u fault=%u: %s\n",
            format, operation, fault, error.message);
  assert(result == success);
  assert(fixture.published == (unsigned)success && fixture.notified == (unsigned)success);
  if (fault == 1 || fault == 2) assert(REMOVE_DIR(blocked) == 0);
  /* Discard every in-memory retry/pending state, exactly as a cold process would. */
  memset(live, 0xcc, sizeof(live));
  assert(SaveSystem_Attach(live, sizeof(live), (SaveBackend)format, path, path, &error));
  assert(SaveSystem_LoadActive(&error));
  SaveSnapshot actual;
  assert(SaveCheckpoint_ReadSnapshot(path, live, &actual, &error));
  AssertSnapshot(&actual, success ? &next : &before);
  Remove(path);
  Remove(donor);
}

static void LegacyAndFirstSave(SaveFileFormat format) {
  const char *path = format == kSaveFileFormat_Ini ? "snapshot-legacy.ini" : "snapshot-legacy.srm";
  char journal[256], name_path[256], blocked[256];
  snprintf(journal, sizeof(journal), "%s.archeckpoint", path);
  snprintf(name_path, sizeof(name_path), "%s.arname", path);
  snprintf(blocked, sizeof(blocked), "%s.tmp", path);
  Remove(path);
  SaveSnapshot before = Snapshot("ELISE", "Élise", 10);
  SaveSnapshot next = Snapshot("RENEE", "Renée", 20), actual;
  SaveError error = {{0}};
  /* Independent version-1 journal plus its old checksum-bound name companion. */
  uint8_t bytes[12 + 4 + kActRaiserSramSize + 1 + 8] = "ARCHECK";
  ByteOrder_WriteLe16(bytes + 8, 1);
  ByteOrder_WriteLe16(bytes + 10, 1);
  ByteOrder_WriteLe32(bytes + 12, 1);
  memcpy(bytes + 16, before.image, kActRaiserSramSize);
  bytes[16 + kActRaiserSramSize] = 10;
  uint64_t hash = DeterministicHash_Fnv1a64(DETERMINISTIC_HASH_FNV1A64_OFFSET,
                                           bytes, sizeof(bytes) - 8);
  ByteOrder_WriteLe32(bytes + sizeof(bytes) - 8, (uint32_t)hash);
  ByteOrder_WriteLe32(bytes + sizeof(bytes) - 4, (uint32_t)(hash >> 32));
  uint8_t name[29] = "ARNAME1";
  ByteOrder_WriteLe32(name + 8, Save_ComputeChecksum(before.image));
  memcpy(name + 12, "ELISE", 5);
  ByteOrder_WriteLe16(name + 21, 6);
  memcpy(name + 23, "Élise", 6);
  assert(Save_WriteFile(format, path, before.image, &error));
  assert(Save_WriteCompanionFile(journal, bytes, sizeof(bytes), &error));
  assert(Save_WriteCompanionFile(name_path, name, sizeof(name), &error));
  assert(SaveCheckpoint_ReadSnapshot(path, before.image, &actual, &error));
  AssertSnapshot(&actual, &before);
  assert(sr_mkdir(blocked) == 0);
  assert(!SaveCheckpoint_CommitSnapshot(format, path, before.image, &next,
                                        ValidateRecord, NULL, &error));
  assert(SaveCheckpoint_ReadSnapshot(path, before.image, &actual, &error));
  AssertSnapshot(&actual, &before); /* Old name survives interruption during migration. */
  assert(REMOVE_DIR(blocked) == 0);
  assert(SaveCheckpoint_CommitSnapshot(format, path, before.image, &next,
                                       ValidateRecord, NULL, &error));
  assert(SaveCheckpoint_ReadSnapshot(path, next.image, &actual, &error));
  AssertSnapshot(&actual, &next);
  assert(SaveCheckpoint_ReadSnapshot(path, before.image, &actual, &error));
  AssertSnapshot(&actual, &before);
  Remove(path);

  /* An interrupted very first save may be retried only with the same complete
   * snapshot, including the name. A mismatched retry preserves its journal. */
  assert(sr_mkdir(blocked) == 0);
  assert(!SaveCheckpoint_CommitSnapshot(format, path, NULL, &next, ValidateRecord, NULL, &error));
  assert(REMOVE_DIR(blocked) == 0);
  actual = next;
  snprintf(actual.name, sizeof(actual.name), "Wrong");
  assert(!SaveCheckpoint_CommitSnapshot(format, path, NULL, &actual, ValidateRecord, NULL, &error));
  assert(SaveCheckpoint_CommitSnapshot(format, path, NULL, &next, ValidateRecord, NULL, &error));
  assert(SaveCheckpoint_ReadSnapshot(path, next.image, &actual, &error));
  AssertSnapshot(&actual, &next);
  Remove(path);
}

int main(void) {
  for (unsigned format = 0; format < 2; ++format) {
    for (unsigned operation = 0; operation < kOperations; ++operation)
      for (unsigned fault = 0; fault < 4; ++fault) Transaction(format, operation, fault);
    LegacyAndFirstSave(format);
  }
  puts("complete snapshots: all write paths, interrupted writes, cold reload and migration passed");
  return 0;
}
