#include "regional_session.h"

#include "byte_order.h"

#include <stdio.h>
#include <string.h>

enum { kHeaderBytes = 36, kPayloadCapacity = 1024,
       kV1RecordCount = kArRegionalCostRule_Count + kArRegionalTimerRule_Count,
       kV2RecordCount = kV1RecordCount + 1,
       kV3RecordCount = kV2RecordCount + 1,
       kV4RecordCount = kV3RecordCount + 1,
       kV5RecordCount = kV4RecordCount + kArRegionalDevelopmentRule_Count,
       kV6RecordCount = kV5RecordCount + kArRegionalRecovery_Count,
       kV7RecordCount = kV6RecordCount + kArRegionalQuake_Count,
       kV8RecordCount = kV7RecordCount + 1,
       kV9RecordCount = kV8RecordCount + 1,
       kV10RecordCount = kV9RecordCount + 1,
       kRecordCount = kV10RecordCount + 1 };
_Static_assert(kArRegionalCostRule_Count == 9 && kArRegionalTimerRule_Count == 6,
               "extend the legacy record mapping explicitly when adding family leaves");
_Static_assert(kArRegionalDevelopmentRule_Count == 3,"version5 has three development leaves");
_Static_assert(kArRegionalRecovery_Count == 2, "version6 has two recovery leaves");
_Static_assert(kArRegionalQuake_Count == 5, "version7 has five quake selectors");
static const uint8_t kMagic[8] = {'A', 'R', 'R', 'E', 'G', 'I', 'O', 'N'};
static const uint8_t kPriceMagic[8] = {'A', 'R', 'P', 'R', 'I', 'C', 'E', 0};
static const char *const kSourceKeys[] = {"us", "jp", "eu"};
_Static_assert(sizeof(kSourceKeys) / sizeof(kSourceKeys[0]) == kArRegionalSource_Count,
               "each persisted source needs a stable key");

static bool Valid(const ArRegionalSession *session) {
  if (!session || !session->revision) return false;
  bool has_id = false;
  for (unsigned i = 0; i < sizeof(session->campaign); ++i) has_id |= session->campaign[i] != 0;
  ArRegionalCostSnapshot unused;
  ArRegionalDevelopmentSnapshot unused_development;
  ArRegionalRecoverySnapshot unused_recovery;
  ArRegionalQuakeSnapshot unused_quake;
  return has_id && ArRegionalCosts_Resolve(&session->requested.costs, &unused) &&
      ArRegionalCosts_Resolve(&session->effective.costs, &unused) &&
      ArRegionalTimers_Valid(&session->requested.timers) &&
      ArRegionalTimers_Valid(&session->effective.timers) &&
      (unsigned)session->requested.retry_score < kArRegionalSource_Count &&
      (unsigned)session->effective.retry_score < kArRegionalSource_Count &&
      (unsigned)session->requested.town_wait < kArRegionalSource_Count &&
      (unsigned)session->effective.town_wait < kArRegionalSource_Count &&
      (unsigned)session->requested.fishing < kArRegionalSource_Count &&
      (unsigned)session->effective.fishing < kArRegionalSource_Count &&
      ArRegionalDevelopment_Resolve(&session->requested.development,&unused_development) &&
      ArRegionalDevelopment_Resolve(&session->effective.development,&unused_development) &&
      ArRegionalRecovery_Resolve(&session->requested.recovery, &unused_recovery) &&
      ArRegionalRecovery_Resolve(&session->effective.recovery, &unused_recovery) &&
      ArRegionalQuake_Resolve(&session->requested.quake, &unused_quake) &&
      ArRegionalQuake_Resolve(&session->effective.quake, &unused_quake) &&
      (unsigned)session->requested.score_page < kArRegionalSource_Count &&
      (unsigned)session->effective.score_page < kArRegionalSource_Count &&
      (unsigned)session->requested.menu_return < kArRegionalSource_Count &&
      (unsigned)session->effective.menu_return < kArRegionalSource_Count &&
      (unsigned)session->requested.speed_range < kArRegionalSource_Count &&
      (unsigned)session->effective.speed_range < kArRegionalSource_Count &&
      (unsigned)session->requested.magic_gesture < kArRegionalSource_Count &&
      (unsigned)session->effective.magic_gesture < kArRegionalSource_Count;
}

bool ArRegionalSession_NewGame(ArRegionalSession *session, uint32_t slot,
    const uint8_t campaign[16], const ArRegionalCostPolicy *defaults) {
  if (!session || !campaign || !defaults) return false;
  ArRegionalSession next = {.slot = slot, .revision = 1, .requested.costs = *defaults, .effective.costs = *defaults};
  ArRegionalTimers_Init(&next.requested.timers, kArRegionalSource_US);
  next.effective.timers = next.requested.timers;
  memcpy(next.campaign, campaign, sizeof(next.campaign));
  if (!Valid(&next)) return false;
  *session = next;
  return true;
}

bool ArRegionalSession_RequestTimers(ArRegionalSession *session, uint32_t revision,
                                     ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision) return false;
  ArRegionalTimerPolicy next;
  if (!ArRegionalTimers_Init(&next, source)) return false;
  if (!memcmp(&next, &session->requested.timers, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.timers = next;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginTimers(ArRegionalSession *session,
                                   ArRegionalTimerPolicy *snapshot) {
  if (!snapshot || !Valid(session)) return false;
  const bool changed = memcmp(&session->requested.timers, &session->effective.timers,
                               sizeof(session->requested.timers)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  session->effective.timers = session->requested.timers;
  if (changed) ++session->revision;
  *snapshot = session->effective.timers;
  return true;
}

bool ArRegionalSession_RequestRetryScore(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.retry_score == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.retry_score = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginRetryScore(ArRegionalSession *session, bool *clear_score) {
  if (!clear_score || !Valid(session)) return false;
  const bool changed = session->requested.retry_score != session->effective.retry_score;
  if (changed && session->revision == UINT32_MAX) return false;
  bool resolved;
  if (!ArRegionalRetry_Resolve(session->requested.retry_score, &resolved)) return false;
  session->effective.retry_score = session->requested.retry_score;
  if (changed) ++session->revision;
  *clear_score = resolved;
  return true;
}

bool ArRegionalSession_RequestTownWait(ArRegionalSession *session, uint32_t revision,
                                      ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.town_wait == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.town_wait = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginTownWait(ArRegionalSession *session, uint16_t *updates) {
  if (!updates || !Valid(session)) return false;
  const bool changed = session->requested.town_wait != session->effective.town_wait;
  if (changed && session->revision == UINT32_MAX) return false;
  uint16_t resolved;
  if (!ArRegionalTownWait_Resolve(session->requested.town_wait, &resolved)) return false;
  session->effective.town_wait = session->requested.town_wait;
  if (changed) ++session->revision;
  *updates = resolved;
  return true;
}

bool ArRegionalSession_RequestFishing(ArRegionalSession *session, uint32_t revision,
                                     ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.fishing == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.fishing = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginFishing(ArRegionalSession *session, uint16_t *updates,
                                   bool *reconcile) {
  if (!updates || !reconcile || !Valid(session)) return false;
  const bool changed = session->requested.fishing != session->effective.fishing;
  if (changed && session->revision == UINT32_MAX) return false;
  uint16_t pending, active;
  if (!ArRegionalFishing_Resolve(session->requested.fishing, &pending) ||
      !ArRegionalFishing_Resolve(session->effective.fishing, &active)) return false;
  session->effective.fishing = session->requested.fishing;
  if (changed) ++session->revision;
  *updates = pending;
  *reconcile = pending != active;
  return true;
}

bool ArRegionalSession_RequestDevelopment(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source) {
  if(!Valid(session) || session->revision!=revision)return false;
  ArRegionalDevelopmentPolicy next;
  if(!ArRegionalDevelopment_Init(&next,source))return false;
  if(!memcmp(&next,&session->requested.development,sizeof(next)))return true;
  if(session->revision==UINT32_MAX)return false;
  session->requested.development=next;++session->revision;return true;
}
bool ArRegionalSession_BeginDevelopment(ArRegionalSession *session,
                                       ArRegionalDevelopmentSnapshot *snapshot) {
  if(!snapshot || !Valid(session))return false;
  const bool changed=memcmp(&session->requested.development,&session->effective.development,
                            sizeof(session->requested.development))!=0;
  if(changed && session->revision==UINT32_MAX)return false;
  ArRegionalDevelopmentSnapshot next;
  if(!ArRegionalDevelopment_Resolve(&session->requested.development,&next))return false;
  session->effective.development=session->requested.development;
  if(changed)++session->revision;
  *snapshot=next;return true;
}

bool ArRegionalSession_RequestRecovery(ArRegionalSession *session, uint32_t revision,
                                      ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision) return false;
  ArRegionalRecoveryPolicy next;
  if (!ArRegionalRecovery_Init(&next, source)) return false;
  if (!memcmp(&next, &session->requested.recovery, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.recovery = next;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginRecovery(ArRegionalSession *session,
    ArRegionalRecoverySnapshot *snapshot, unsigned *changed) {
  if (!snapshot || !changed || !Valid(session)) return false;
  const bool edit = memcmp(&session->requested.recovery, &session->effective.recovery,
                           sizeof(session->requested.recovery)) != 0;
  if (edit && session->revision == UINT32_MAX) return false;
  ArRegionalRecoverySnapshot next;
  if (!ArRegionalRecovery_Resolve(&session->requested.recovery, &next)) return false;
  unsigned mask = 0;
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i) {
    const ArRegionalRecoveryDescriptor *rule = ArRegionalRecovery_Descriptor((ArRegionalRecoveryRule)i);
    if (rule->value[session->requested.recovery.source[i]] !=
        rule->value[session->effective.recovery.source[i]]) mask |= 1u << i;
  }
  session->effective.recovery = session->requested.recovery;
  if (edit) ++session->revision;
  *snapshot = next;
  *changed = mask;
  return true;
}

bool ArRegionalSession_RequestQuake(ArRegionalSession *session, uint32_t revision,
                                    ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision) return false;
  ArRegionalQuakePolicy next;
  if (!ArRegionalQuake_Init(&next, source)) return false;
  if (!memcmp(&next, &session->requested.quake, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.quake = next;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginQuake(ArRegionalSession *session, ArRegionalQuakeSnapshot *snapshot) {
  if (!snapshot || !Valid(session)) return false;
  const bool changed = memcmp(&session->requested.quake, &session->effective.quake,
                              sizeof(session->requested.quake)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  ArRegionalQuakeSnapshot next;
  if (!ArRegionalQuake_Resolve(&session->requested.quake, &next)) return false;
  session->effective.quake = session->requested.quake;
  if (changed) ++session->revision;
  *snapshot = next;
  return true;
}

bool ArRegionalSession_RequestScorePage(ArRegionalSession *session, uint32_t revision,
                                       ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision || (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.score_page == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.score_page = source;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginScorePage(ArRegionalSession *session, bool *enabled) {
  if (!enabled || !Valid(session)) return false;
  const bool changed = session->requested.score_page != session->effective.score_page;
  if (changed && session->revision == UINT32_MAX) return false;
  bool next;
  if (!ArRegionalScorePage_Resolve(session->requested.score_page, &next)) return false;
  session->effective.score_page = session->requested.score_page;
  if (changed) ++session->revision;
  *enabled = next;
  return true;
}

bool ArRegionalSession_RequestMenuReturn(ArRegionalSession *session, uint32_t revision,
                                        ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision || (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.menu_return == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.menu_return = source;
  ++session->revision;
  return true;
}
bool ArRegionalSession_BeginMenuReturn(ArRegionalSession *session, bool *keep_open) {
  if (!keep_open || !Valid(session)) return false;
  const bool changed = session->requested.menu_return != session->effective.menu_return;
  if (changed && session->revision == UINT32_MAX) return false;
  bool next;
  if (!ArRegionalMenuReturn_Resolve(session->requested.menu_return, &next)) return false;
  session->effective.menu_return = session->requested.menu_return;
  if (changed) ++session->revision;
  *keep_open = next;
  return true;
}

bool ArRegionalSession_RequestSpeedRange(ArRegionalSession *session, uint32_t revision,
                                        ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.speed_range == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.speed_range = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginSpeedRange(ArRegionalSession *session, uint16_t *maximum) {
  if (!maximum || !Valid(session)) return false;
  const bool changed = session->requested.speed_range != session->effective.speed_range;
  if (changed && session->revision == UINT32_MAX) return false;
  uint16_t next;
  if (!ArRegionalSpeedRange_Resolve(session->requested.speed_range, &next)) return false;
  session->effective.speed_range = session->requested.speed_range;
  if (changed) ++session->revision;
  *maximum = next;
  return true;
}

bool ArRegionalSession_RequestMagicGesture(ArRegionalSession *session, uint32_t revision,
                                         ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision ||
      (unsigned)source >= kArRegionalSource_Count) return false;
  if (session->requested.magic_gesture == source) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.magic_gesture = source;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginMagicGesture(ArRegionalSession *session,
                                        bool controls_released, bool *up_attack) {
  if (!up_attack || !Valid(session)) return false;
  const ArRegionalSource next = controls_released ? session->requested.magic_gesture
                                                : session->effective.magic_gesture;
  const bool changed = next != session->effective.magic_gesture;
  if (changed && session->revision == UINT32_MAX) return false;
  bool resolved;
  if (!ArRegionalMagicGesture_Resolve(next, &resolved)) return false;
  session->effective.magic_gesture = next;
  if (changed) ++session->revision;
  *up_attack = resolved;
  return true;
}

/* The wire shape is shared, not the units: stable keys select the descriptor
 * for resource counts, initial BCD times, booleans or town service counts. */
static const char *Record(unsigned i, const uint16_t **values) {
  if (i == kV10RecordCount) {
    const ArRegionalMagicGestureDescriptor *desc = ArRegionalMagicGesture_Descriptor();
    *values = desc->up_attack;
    return desc->key;
  }
  if (i == kV9RecordCount) {
    const ArRegionalSpeedRangeDescriptor *desc = ArRegionalSpeedRange_Descriptor();
    *values = desc->maximum;
    return desc->key;
  }
  if (i == kV8RecordCount) {
    const ArRegionalMenuReturnDescriptor *desc = ArRegionalMenuReturn_Descriptor();
    *values = desc->keep_open;
    return desc->key;
  }
  if (i == kV7RecordCount) {
    const ArRegionalScorePageDescriptor *desc = ArRegionalScorePage_Descriptor();
    *values = desc->enabled;
    return desc->key;
  }
  if (i >= kV6RecordCount) {
    const ArRegionalQuakeDescriptor *desc = ArRegionalQuake_Descriptor((ArRegionalQuakeRule)(i - kV6RecordCount));
    *values = desc->random;
    return desc->key;
  }
  if (i >= kV5RecordCount) {
    const ArRegionalRecoveryDescriptor *desc = ArRegionalRecovery_Descriptor(
        (ArRegionalRecoveryRule)(i - kV5RecordCount));
    *values = desc->value;
    return desc->key;
  }
  if(i>=kV4RecordCount) {
    const ArRegionalDevelopmentDescriptor *desc=ArRegionalDevelopment_Descriptor(
        (ArRegionalDevelopmentRule)(i-kV4RecordCount));
    *values=desc->updates;return desc->key;
  }
  if (i == kV3RecordCount) {
    const ArRegionalFishingDescriptor *desc = ArRegionalFishing_Descriptor();
    *values = desc->updates;
    return desc->key;
  }
  if (i == kV2RecordCount) {
    const ArRegionalTownWaitDescriptor *desc = ArRegionalTownWait_Descriptor();
    *values = desc->updates;
    return desc->key;
  }
  if (i == kV1RecordCount) {
    const ArRegionalRetryDescriptor *desc = ArRegionalRetry_Descriptor();
    *values = desc->clear_score;
    return desc->key;
  }
  if (i < kArRegionalCostRule_Count) {
    const ArRegionalCostDescriptor *desc = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    *values = desc->price;
    return desc->key;
  }
  const ArRegionalTimerDescriptor *desc = ArRegionalTimers_Descriptor(
      (ArRegionalTimerRule)(i - kArRegionalCostRule_Count));
  *values = desc->bcd;
  return desc->key;
}

static ArRegionalSource RecordSource(const ArRegionalSession *session, unsigned i, bool requested) {
  if (i == kV10RecordCount) return requested ? session->requested.magic_gesture : session->effective.magic_gesture;
  if (i == kV9RecordCount) return requested ? session->requested.speed_range : session->effective.speed_range;
  if (i == kV8RecordCount) return requested ? session->requested.menu_return : session->effective.menu_return;
  if (i == kV7RecordCount) return requested ? session->requested.score_page : session->effective.score_page;
  if (i >= kV6RecordCount)
    return requested ? session->requested.quake.source[i - kV6RecordCount] :
                       session->effective.quake.source[i - kV6RecordCount];
  if (i >= kV5RecordCount)
    return requested ? session->requested.recovery.source[i - kV5RecordCount] :
                       session->effective.recovery.source[i - kV5RecordCount];
  if(i>=kV4RecordCount)
    return requested?session->requested.development.source[i-kV4RecordCount]:session->effective.development.source[i-kV4RecordCount];
  if (i == kV3RecordCount)
    return requested ? session->requested.fishing : session->effective.fishing;
  if (i == kV2RecordCount)
    return requested ? session->requested.town_wait : session->effective.town_wait;
  if (i == kV1RecordCount)
    return requested ? session->requested.retry_score : session->effective.retry_score;
  if (i < kArRegionalCostRule_Count)
    return requested ? session->requested.costs.source[i] : session->effective.costs.source[i];
  i -= kArRegionalCostRule_Count;
  return requested ? session->requested.timers.source[i] : session->effective.timers.source[i];
}

bool ArRegionalSession_RequestCosts(ArRegionalSession *session, uint32_t revision,
    ArRegionalCostGroup group, ArRegionalSource source) {
  if (!Valid(session) || session->revision != revision) return false;
  ArRegionalCostPolicy next = session->requested.costs;
  if (!ArRegionalCosts_SetGroup(&next, group, source)) return false;
  if (!memcmp(&next, &session->requested.costs, sizeof(next))) return true;
  if (session->revision == UINT32_MAX) return false;
  session->requested.costs = next;
  ++session->revision;
  return true;
}

bool ArRegionalSession_BeginCosts(ArRegionalSession *session,
    ArRegionalCostGroup group, ArRegionalCostSnapshot *quote) {
  if (!Valid(session) || !quote || (unsigned)group >= kArRegionalCostGroup_Count) return false;
  ArRegionalCostPolicy next = session->effective.costs;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i)
    if (ArRegionalCosts_Descriptor((ArRegionalCostRule)i)->group == group)
      next.source[i] = session->requested.costs.source[i];
  bool changed = memcmp(&next, &session->effective.costs, sizeof(next)) != 0;
  if (changed && session->revision == UINT32_MAX) return false;
  ArRegionalCostSnapshot prices;
  if (!ArRegionalCosts_Resolve(&next, &prices)) return false;
  session->effective.costs = next;
  if (changed) ++session->revision;
  *quote = prices;
  return true;
}

/* Compact explicit codec, never fwrite a C struct or persist enum ordinals.
 * Each named leaf carries requested/effective source keys AND resolved values.
 * A table/schema change that cannot reproduce those values fails visibly. */
static bool Encode(const ArRegionalSession *session, uint8_t *out, size_t *size) {
  if (!Valid(session)) return false;
  memset(out, 0, kHeaderBytes);
  memcpy(out, kMagic, sizeof(kMagic));
  ByteOrder_WriteLe16(out + 8, 11);
  ByteOrder_WriteLe16(out + 10, kRecordCount);
  ByteOrder_WriteLe32(out + 12, session->slot);
  memcpy(out + 16, session->campaign, 16);
  ByteOrder_WriteLe32(out + 32, session->revision);
  size_t offset = kHeaderBytes;
  for (unsigned i = 0; i < kRecordCount; ++i) {
    const uint16_t *values;
    const char *key = Record(i, &values);
    const ArRegionalSource requested = RecordSource(session, i, true);
    const ArRegionalSource effective = RecordSource(session, i, false);
    size_t length = strlen(key);
    if (!length || length > UINT8_MAX || length + 9 > kPayloadCapacity - offset) return false;
    out[offset++] = (uint8_t)length;
    memcpy(out + offset, key, length);
    offset += length;
    memcpy(out + offset, kSourceKeys[requested], 2);
    memcpy(out + offset + 2, kSourceKeys[effective], 2);
    ByteOrder_WriteLe16(out + offset + 4, values[requested]);
    ByteOrder_WriteLe16(out + offset + 6, values[effective]);
    offset += 8;
  }
  *size = offset;
  return true;
}

static ArRegionalSource DecodeSource(const uint8_t *key) {
  for (unsigned i = 0; i < kArRegionalSource_Count; ++i)
    if (!memcmp(key, kSourceKeys[i], 2)) return (ArRegionalSource)i;
  return kArRegionalSource_Count;
}

static SaveCheckpointStatus Decode(const uint8_t *bytes, size_t size, ArRegionalSession *session) {
  if (size < kHeaderBytes) return kSaveCheckpoint_Invalid;
  const bool pricing_only = !memcmp(bytes, kPriceMagic, sizeof(kPriceMagic));
  if (!pricing_only && memcmp(bytes, kMagic, sizeof(kMagic))) return kSaveCheckpoint_Invalid;
  const unsigned version = ByteOrder_ReadLe16(bytes + 8);
  if (version < 1 || version > (pricing_only ? 1u : 11u)) return kSaveCheckpoint_Unsupported;
  const unsigned count = pricing_only ? kArRegionalCostRule_Count :
      version == 1 ? kV1RecordCount : version == 2 ? kV2RecordCount :
      version == 3 ? kV3RecordCount : version == 4 ? kV4RecordCount :
      version == 5 ? kV5RecordCount : version == 6 ? kV6RecordCount :
      version == 7 ? kV7RecordCount : version == 8 ? kV8RecordCount :
      version == 9 ? kV9RecordCount : version == 10 ? kV10RecordCount : kRecordCount;
  if (ByteOrder_ReadLe16(bytes + 10) != count) return kSaveCheckpoint_Unsupported;
  ArRegionalSession next = {.slot = ByteOrder_ReadLe32(bytes + 12), .revision = ByteOrder_ReadLe32(bytes + 32)};
  memcpy(next.campaign, bytes + 16, sizeof(next.campaign));
  ArRegionalTimers_Init(&next.requested.timers, kArRegionalSource_US);
  next.effective.timers = next.requested.timers;
  bool seen[kRecordCount] = {0};
  size_t offset = kHeaderBytes;
  for (unsigned n = 0; n < count; ++n) {
    if (offset == size) return kSaveCheckpoint_Invalid;
    size_t length = bytes[offset++];
    if (!length || length + 8 > size - offset) return kSaveCheckpoint_Invalid;
    unsigned rule;
    const uint16_t *values = NULL;
    for (rule = 0; rule < count; ++rule) {
      const char *key = Record(rule, &values);
      if (strlen(key) == length && !memcmp(bytes + offset, key, length)) break;
    }
    if (rule == count) return kSaveCheckpoint_Unsupported;
    if (seen[rule]) return kSaveCheckpoint_Invalid;
    seen[rule] = true;
    offset += length;
    ArRegionalSource requested = DecodeSource(bytes + offset), effective = DecodeSource(bytes + offset + 2);
    if (requested == kArRegionalSource_Count || effective == kArRegionalSource_Count)
      return kSaveCheckpoint_Unsupported;
    if (values[requested] != ByteOrder_ReadLe16(bytes + offset + 4) ||
        values[effective] != ByteOrder_ReadLe16(bytes + offset + 6)) return kSaveCheckpoint_Unsupported;
    if (rule == kV10RecordCount) {
      next.requested.magic_gesture = requested; next.effective.magic_gesture = effective;
    } else if (rule == kV9RecordCount) {
      next.requested.speed_range = requested; next.effective.speed_range = effective;
    } else if (rule == kV8RecordCount) {
      next.requested.menu_return = requested; next.effective.menu_return = effective;
    } else if (rule == kV7RecordCount) {
      next.requested.score_page = requested; next.effective.score_page = effective;
    } else if (rule >= kV6RecordCount) {
      next.requested.quake.source[rule - kV6RecordCount] = requested;
      next.effective.quake.source[rule - kV6RecordCount] = effective;
    } else if (rule >= kV5RecordCount) {
      next.requested.recovery.source[rule - kV5RecordCount] = requested;
      next.effective.recovery.source[rule - kV5RecordCount] = effective;
    } else if(rule>=kV4RecordCount) {
      next.requested.development.source[rule-kV4RecordCount]=requested;
      next.effective.development.source[rule-kV4RecordCount]=effective;
    } else if (rule == kV3RecordCount) {
      next.requested.fishing = requested;
      next.effective.fishing = effective;
    } else if (rule == kV2RecordCount) {
      next.requested.town_wait = requested;
      next.effective.town_wait = effective;
    } else if (rule == kV1RecordCount) {
      next.requested.retry_score = requested;
      next.effective.retry_score = effective;
    } else if (rule < kArRegionalCostRule_Count) {
      next.requested.costs.source[rule] = requested;
      next.effective.costs.source[rule] = effective;
    } else {
      next.requested.timers.source[rule - kArRegionalCostRule_Count] = requested;
      next.effective.timers.source[rule - kArRegionalCostRule_Count] = effective;
    }
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
    if (error) snprintf(error->message, sizeof(error->message), "invalid regional rules session");
    return false;
  }
  uint32_t slot = session->slot;
  return SaveCheckpoint_Commit(format, path, expected, image, payload, size,
                               ValidatePayload, &slot, error);
}
