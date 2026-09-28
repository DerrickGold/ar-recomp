#ifndef AR_REGIONAL_SAVE_FIXTURE_H
#define AR_REGIONAL_SAVE_FIXTURE_H
/* Offline fixture creation bypasses the live owner, but uses the same complete
 * snapshot transaction and validators as production. */
#include "regional/session/regional_session.h"

static inline SaveCheckpointStatus TestRegional_ValidateSave(
    const uint8_t *payload, size_t size, void *context) {
  ArRegionalSession decoded;
  SaveCheckpointStatus status = ArRegionalSession_Decode(payload, size, &decoded);
  if (status == kSaveCheckpoint_Ready && decoded.slot != *(const uint32_t *)context)
    return kSaveCheckpoint_Mismatch;
  return status;
}

static inline bool TestRegional_Save(const ArRegionalSession *session, SaveFileFormat format,
    const char *path, const uint8_t *expected, const uint8_t *image, SaveError *error) {
  uint8_t payload[kSaveCheckpointPayloadMax];
  size_t size;
  if (!ArRegionalSession_Encode(session, payload, sizeof(payload), &size)) return false;
  uint32_t slot = session->slot;
  return SaveCheckpoint_Commit(format, path, expected, image, payload, size,
                                TestRegional_ValidateSave, &slot, error);
}
#endif
