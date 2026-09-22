#include "regional_session.h"

#include "byte_order.h"

#include <stdio.h>
#include <string.h>

enum { kHeaderBytes = 36, kPayloadCapacity = 1024 };
static const uint8_t kMagic[8] = {'A', 'R', 'P', 'R', 'I', 'C', 'E', 0};
static const char *const kSourceKeys[] = {"us", "jp", "eu"};
_Static_assert(sizeof(kSourceKeys) / sizeof(kSourceKeys[0]) == kArRegionalCostSource_Count,
               "each persisted source needs a stable key");

static bool Valid(const ArRegionalSession *session) {
  if (!session || !session->revision) return false;
  bool has_id = false;
  for (unsigned i = 0; i < sizeof(session->campaign); ++i) has_id |= session->campaign[i] != 0;
  ArRegionalCostSnapshot unused;
  return has_id && ArRegionalCosts_Resolve(&session->requested, &unused) &&
      ArRegionalCosts_Resolve(&session->effective, &unused);
}

bool ArRegionalSession_NewGame(ArRegionalSession *session, uint32_t slot,
    const uint8_t campaign[16], const ArRegionalCostPolicy *defaults) {
  if (!session || !campaign || !defaults) return false;
  ArRegionalSession next = {.slot = slot, .revision = 1, .requested = *defaults, .effective = *defaults};
  memcpy(next.campaign, campaign, sizeof(next.campaign));
  if (!Valid(&next)) return false;
  *session = next;
  return true;
}

bool ArRegionalSession_RequestCosts(ArRegionalSession *session, uint32_t revision,
    ArRegionalCostGroup group, ArRegionalCostSource source) {
  if (!Valid(session) || session->revision != revision) return false;
  ArRegionalCostPolicy next = session->requested;
  if (!ArRegionalCosts_SetGroup(&next, group, source)) return false;
  if (!memcmp(&next, &session->requested, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested = next;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginCosts(ArRegionalSession *session,
    ArRegionalCostGroup group, ArRegionalCostSnapshot *quote) {
  if (!Valid(session) || !quote || (unsigned)group >= kArRegionalCostGroup_Count) return false;
  ArRegionalCostPolicy next = session->effective;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i)
    if (ArRegionalCosts_Descriptor((ArRegionalCostRule)i)->group == group)
      next.source[i] = session->requested.source[i];
  bool changed = memcmp(&next, &session->effective, sizeof(next)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  ArRegionalCostSnapshot prices;
  if (!ArRegionalCosts_Resolve(&next, &prices)) return false;
  session->effective = next;
  if (changed) ++session->revision;
  *quote = prices;
  return true;
}

/* Compact explicit codec, never fwrite a C struct or persist enum ordinals.
 * Each named leaf carries requested/effective source keys AND resolved prices.
 * A table/schema change that cannot reproduce those prices fails visibly. */
static bool Encode(const ArRegionalSession *session, uint8_t *out, size_t *size) {
  if (!Valid(session)) return false;
  memset(out, 0, kHeaderBytes);
  memcpy(out, kMagic, sizeof(kMagic));
  ByteOrder_WriteLe16(out + 8, 1);
  ByteOrder_WriteLe16(out + 10, kArRegionalCostRule_Count);
  ByteOrder_WriteLe32(out + 12, session->slot);
  memcpy(out + 16, session->campaign, 16);
  ByteOrder_WriteLe32(out + 32, session->revision);
  size_t offset = kHeaderBytes;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    const ArRegionalCostDescriptor *desc = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    size_t length = strlen(desc->key);
    if (!length || length > UINT8_MAX || length + 9 > kPayloadCapacity - offset) return false;
    out[offset++] = (uint8_t)length;
    memcpy(out + offset, desc->key, length);
    offset += length;
    memcpy(out + offset, kSourceKeys[session->requested.source[i]], 2);
    memcpy(out + offset + 2, kSourceKeys[session->effective.source[i]], 2);
    ByteOrder_WriteLe16(out + offset + 4, desc->price[session->requested.source[i]]);
    ByteOrder_WriteLe16(out + offset + 6, desc->price[session->effective.source[i]]);
    offset += 8;
  }
  *size = offset;
  return true;
}

static ArRegionalCostSource DecodeSource(const uint8_t *key) {
  for (unsigned i = 0; i < kArRegionalCostSource_Count; ++i)
    if (!memcmp(key, kSourceKeys[i], 2)) return (ArRegionalCostSource)i;
  return kArRegionalCostSource_Count;
}

static SaveCheckpointStatus Decode(const uint8_t *bytes, size_t size, ArRegionalSession *session) {
  if (size < kHeaderBytes || memcmp(bytes, kMagic, sizeof(kMagic))) return kSaveCheckpoint_Invalid;
  if (ByteOrder_ReadLe16(bytes + 8) != 1) return kSaveCheckpoint_Unsupported;
  if (ByteOrder_ReadLe16(bytes + 10) != kArRegionalCostRule_Count) return kSaveCheckpoint_Unsupported;
  ArRegionalSession next = {.slot = ByteOrder_ReadLe32(bytes + 12), .revision = ByteOrder_ReadLe32(bytes + 32)};
  memcpy(next.campaign, bytes + 16, sizeof(next.campaign));
  bool seen[kArRegionalCostRule_Count] = {0};
  size_t offset = kHeaderBytes;
  for (unsigned n = 0; n < kArRegionalCostRule_Count; ++n) {
    if (offset == size) return kSaveCheckpoint_Invalid;
    size_t length = bytes[offset++];
    if (!length || length + 8 > size - offset) return kSaveCheckpoint_Invalid;
    unsigned rule;
    for (rule = 0; rule < kArRegionalCostRule_Count; ++rule) {
      const char *key = ArRegionalCosts_Descriptor((ArRegionalCostRule)rule)->key;
      if (strlen(key) == length && !memcmp(bytes + offset, key, length)) break;
    }
    if (rule == kArRegionalCostRule_Count) return kSaveCheckpoint_Unsupported;
    if (seen[rule]) return kSaveCheckpoint_Invalid;
    seen[rule] = true;
    offset += length;
    ArRegionalCostSource requested = DecodeSource(bytes + offset), effective = DecodeSource(bytes + offset + 2);
    if (requested == kArRegionalCostSource_Count || effective == kArRegionalCostSource_Count)
      return kSaveCheckpoint_Unsupported;
    const ArRegionalCostDescriptor *desc = ArRegionalCosts_Descriptor((ArRegionalCostRule)rule);
    if (desc->price[requested] != ByteOrder_ReadLe16(bytes + offset + 4) ||
        desc->price[effective] != ByteOrder_ReadLe16(bytes + offset + 6)) return kSaveCheckpoint_Unsupported;
    next.requested.source[rule] = requested;
    next.effective.source[rule] = effective;
    offset += 8;
  }
  if (offset != size || !Valid(&next)) return kSaveCheckpoint_Invalid;
  *session = next;
  return kSaveCheckpoint_Ready;
}

SaveCheckpointStatus ArRegionalSession_Load(ArRegionalSession *session,
    uint32_t slot, const char *path, const uint8_t *image, SaveError *error) {
  if (error) error->message[0] = 0;
  if (!session) return kSaveCheckpoint_Invalid;
  uint8_t payload[kSaveCheckpointPayloadMax];
  size_t size;
  SaveCheckpointStatus status = SaveCheckpoint_Read(path, image, payload, sizeof(payload), &size, error);
  if (status != kSaveCheckpoint_Ready) return status;
  ArRegionalSession next;
  status = Decode(payload, size, &next);
  if (status == kSaveCheckpoint_Ready && next.slot != slot) status = kSaveCheckpoint_Mismatch;
  if (status != kSaveCheckpoint_Ready) {
    if (error) snprintf(error->message, sizeof(error->message),
                        "regional checkpoint is incompatible or belongs to another slot; preserved");
    return status;
  }
  *session = next;
  return kSaveCheckpoint_Ready;
}

static SaveCheckpointStatus ValidatePayload(const uint8_t *payload, size_t size, void *context) {
  ArRegionalSession decoded;
  SaveCheckpointStatus status = Decode(payload, size, &decoded);
  if (status == kSaveCheckpoint_Ready && decoded.slot != *(const uint32_t *)context)
    return kSaveCheckpoint_Mismatch;
  return status;
}

bool ArRegionalSession_Save(const ArRegionalSession *session, SaveFileFormat format,
    const char *path, const uint8_t *expected, const uint8_t *image, SaveError *error) {
  uint8_t payload[kPayloadCapacity];
  size_t size;
  if (error) error->message[0] = 0;
  if (!Encode(session, payload, &size)) {
    if (error) snprintf(error->message, sizeof(error->message), "invalid regional pricing session");
    return false;
  }
  uint32_t slot = session->slot;
  return SaveCheckpoint_Commit(format, path, expected, image, payload, size,
                               ValidatePayload, &slot, error);
}
