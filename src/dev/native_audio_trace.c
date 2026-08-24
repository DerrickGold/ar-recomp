#include "native_audio_trace.h"

#include <string.h>

enum {
  kRequestCapacity = 32768,
  kSongEventCapacity = 2048,
  kDspProvenanceCapacity = 1024,
  kSuppressionProvenanceCapacity = 128,
  kScheduledPortCapacity = 256,
  kEffectLaneCount = 2,
  kEffectPortFirst = 2,
  kEffectPortLast = 3,
  kSpcSequenceStart = 0x0E14,
  kSpcSequenceEnd = 0x0E51,
  kSpcOrdinaryBusyGate = 0x0DF6,
};

typedef struct ScheduledPortValue {
  uint64_t serial;
  uint8_t value;
} ScheduledPortValue;

typedef struct ScheduledPortQueue {
  ScheduledPortValue values[kScheduledPortCapacity];
  uint32_t head;
  uint32_t tail;
} ScheduledPortQueue;

static NativeAudioRequestRecord s_requests[kRequestCapacity];
static NativeAudioSongEvent s_song_events[kSongEventCapacity];
static NativeAudioDspProvenance s_provenance[kDspProvenanceCapacity];
static NativeAudioMusicSuppression
    s_suppressions[kSuppressionProvenanceCapacity];
static NativeAudioTraceStats s_stats;
static uint64_t s_next_serial;
static uint64_t s_mailbox_serial[2];
static uint64_t s_applied_serial[2];
static uint64_t s_last_read_serial[2];
static uint64_t s_lane_owner[kEffectLaneCount];
static uint64_t s_dual_serial;
static uint32_t s_song_event_count;
static uint32_t s_provenance_count;
static uint32_t s_suppression_count;
static ScheduledPortQueue s_scheduled[2];

static int EffectIndexFromKind(NativeAudioRequestKind kind) {
  return kind == kNativeAudioRequest_Event ? 0 :
         kind == kNativeAudioRequest_Sfx ? 1 : -1;
}

static int EffectIndexFromPort(uint8_t port) {
  return port >= kEffectPortFirst && port <= kEffectPortLast
             ? (int)(port - kEffectPortFirst)
             : -1;
}

static NativeAudioRequestRecord *FindMutable(uint64_t serial) {
  if (!serial) return NULL;
  NativeAudioRequestRecord *r =
      &s_requests[(serial - 1u) % kRequestCapacity];
  return r->serial == serial ? r : NULL;
}

static void SetOutcome(NativeAudioRequestRecord *r,
                       NativeAudioRequestOutcome outcome,
                       uint64_t cycle) {
  if (!r || r->outcome != kNativeAudioOutcome_Pending) return;
  r->outcome = (uint8_t)outcome;
  r->sequence_end_cycle = cycle;
  if (outcome < kNativeAudioOutcome_Count)
    s_stats.outcome[outcome]++;
  if (s_stats.outcome[kNativeAudioOutcome_Pending])
    s_stats.outcome[kNativeAudioOutcome_Pending]--;
}

static uint8_t EffectiveId(uint8_t id) {
  uint8_t effective = (uint8_t)(id & 0x7f);
  return effective > 0x26 ? 0x07 : effective;
}

static void FinishDisplacedPortSerial(uint64_t serial, uint64_t replacement,
                                      uint64_t cycle) {
  NativeAudioRequestRecord *r = FindMutable(serial);
  if (!r || (r->flags & kNativeAudioFlag_SequenceStarted)) return;
  NativeAudioRequestRecord *next = FindMutable(replacement);
  if (next && next->id == r->id) {
    r->replaced_by_serial = replacement;
    SetOutcome(r, kNativeAudioOutcome_CoalescedPortDuplicate, cycle);
    return;
  }
  SetOutcome(r,
             (r->flags & kNativeAudioFlag_DualBusySeen)
                 ? kNativeAudioOutcome_RejectedDualBusy
                 : kNativeAudioOutcome_OverwrittenPort,
             cycle);
}

static void QueueScheduled(int index, uint64_t serial, uint8_t value) {
  ScheduledPortQueue *q = &s_scheduled[index];
  if (q->tail - q->head >= kScheduledPortCapacity) {
    ScheduledPortValue *lost =
        &q->values[q->head % kScheduledPortCapacity];
    FinishDisplacedPortSerial(lost->serial, 0, 0);
    q->head++;
    s_stats.scheduled_port_overflow++;
  }
  ScheduledPortValue *slot =
      &q->values[q->tail % kScheduledPortCapacity];
  slot->serial = serial;
  slot->value = value;
  q->tail++;
}

static uint64_t PopScheduled(int index, uint8_t value) {
  ScheduledPortQueue *q = &s_scheduled[index];
  if (q->head == q->tail) return 0;
  ScheduledPortValue *slot =
      &q->values[q->head % kScheduledPortCapacity];
  uint64_t serial = slot->serial;
  /* Scheduling and apply are ordered. A mismatch means tracing began in the
   * middle of a pre-existing queue; discard until the observed value aligns. */
  while (slot->value != value && q->head + 1u < q->tail) {
    q->head++;
    slot = &q->values[q->head % kScheduledPortCapacity];
    serial = slot->serial;
  }
  q->head++;
  return slot->value == value ? serial : 0;
}

static void AddSongEvent(NativeAudioSongEventKind kind, uint8_t value,
                         uint32_t image_src, const char *caller,
                         uint32_t game_frame, uint64_t cycle) {
  if (s_song_event_count >= kSongEventCapacity) {
    s_stats.song_event_overflow++;
    return;
  }
  NativeAudioSongEvent *e = &s_song_events[s_song_event_count++];
  e->cycle = cycle;
  e->caller = caller;
  e->game_frame = game_frame;
  e->image_src = image_src;
  e->kind = (uint8_t)kind;
  e->value = value;
  s_stats.song_events++;
}

void NativeAudioTraceModel_Reset(void) {
  memset(s_requests, 0, sizeof(s_requests));
  memset(s_song_events, 0, sizeof(s_song_events));
  memset(s_provenance, 0, sizeof(s_provenance));
  memset(s_suppressions, 0, sizeof(s_suppressions));
  memset(&s_stats, 0, sizeof(s_stats));
  memset(s_mailbox_serial, 0, sizeof(s_mailbox_serial));
  memset(s_applied_serial, 0, sizeof(s_applied_serial));
  memset(s_last_read_serial, 0, sizeof(s_last_read_serial));
  memset(s_lane_owner, 0, sizeof(s_lane_owner));
  memset(s_scheduled, 0, sizeof(s_scheduled));
  s_next_serial = 1;
  s_dual_serial = 0;
  s_song_event_count = 0;
  s_provenance_count = 0;
  s_suppression_count = 0;
}

static NativeAudioRequestRecord *CreateRequest(
    NativeAudioRequestKind kind, uint8_t id, const char *caller,
    uint32_t site, uint32_t game_frame, uint16_t cpu_x,
    uint16_t cpu_y, uint64_t cycle, uint64_t *serial_out) {
  int index = EffectIndexFromKind(kind);
  if (index < 0 || id == 0) return NULL;

  uint64_t serial = s_next_serial++;
  NativeAudioRequestRecord *r =
      &s_requests[(serial - 1u) % kRequestCapacity];
  if (r->serial) s_stats.request_records_evicted++;
  memset(r, 0, sizeof(*r));
  r->serial = serial;
  r->posted_cycle = cycle;
  r->caller = caller;
  r->site = site;
  r->game_frame = game_frame;
  r->cpu_x = cpu_x;
  r->cpu_y = cpu_y;
  r->kind = (uint8_t)kind;
  r->id = id;
  r->effective_id = EffectiveId(id);
  r->outcome = kNativeAudioOutcome_Pending;
  s_stats.requests++;
  s_stats.retained_requests = s_stats.requests < kRequestCapacity
                                  ? s_stats.requests
                                  : kRequestCapacity;
  s_stats.outcome[kNativeAudioOutcome_Pending]++;
  if (serial_out) *serial_out = serial;
  return r;
}

uint64_t NativeAudioTraceModel_PostRequest(
    NativeAudioRequestKind kind, uint8_t id, int emitted,
    const char *caller, uint32_t site, uint32_t game_frame,
    uint16_t cpu_x, uint16_t cpu_y, uint64_t cycle) {
  uint64_t serial = 0;
  NativeAudioRequestRecord *r = CreateRequest(
      kind, id, caller, site, game_frame, cpu_x, cpu_y, cycle, &serial);
  if (!r) return 0;
  const int index = EffectIndexFromKind(kind);

  if (!emitted) {
    SetOutcome(r, kNativeAudioOutcome_SuppressedSetting, cycle);
    return serial;
  }

  r->flags |= kNativeAudioFlag_MailboxPosted;
  NativeAudioRequestRecord *old = FindMutable(s_mailbox_serial[index]);
  if (old) {
    old->replaced_by_serial = serial;
    SetOutcome(old,
               old->id == id
                   ? kNativeAudioOutcome_CoalescedMailboxDuplicate
                   : kNativeAudioOutcome_OverwrittenMailbox,
               cycle);
  }
  s_mailbox_serial[index] = serial;
  return serial;
}

uint64_t NativeAudioTraceModel_PostExtendedRequest(
    NativeAudioRequestKind kind, uint8_t id, const char *caller,
    uint32_t site, uint32_t game_frame, uint16_t cpu_x,
    uint16_t cpu_y, uint64_t cycle) {
  uint64_t serial = 0;
  NativeAudioRequestRecord *r = CreateRequest(
      kind, id, caller, site, game_frame, cpu_x, cpu_y, cycle, &serial);
  if (!r) return 0;
  r->flags |= kNativeAudioFlag_ExtendedTransport;
  return serial;
}

void NativeAudioTraceModel_ExtendedDisposition(
    uint64_t serial, uint64_t existing_serial,
    int coalesced, int overflow, uint64_t cycle) {
  NativeAudioRequestRecord *r = FindMutable(serial);
  if (!r || !(r->flags & kNativeAudioFlag_ExtendedTransport)) return;
  if (coalesced) {
    r->replaced_by_serial = existing_serial;
    SetOutcome(r, kNativeAudioOutcome_CoalescedExtendedDuplicate, cycle);
  } else if (overflow) {
    SetOutcome(r, kNativeAudioOutcome_ExtendedFifoOverflow, cycle);
  }
}

void NativeAudioTraceModel_ExtendedSequenceStart(
    uint64_t serial, uint8_t lane, uint8_t virtual_voice, uint64_t cycle) {
  NativeAudioRequestRecord *r = FindMutable(serial);
  if (!r || lane >= 2 || r->outcome != kNativeAudioOutcome_Pending) return;
  r->flags |= kNativeAudioFlag_SequenceStarted;
  r->lanes_started |= (uint8_t)(1u << lane);
  r->active_lanes |= (uint8_t)(1u << lane);
  if (virtual_voice >= 8 && virtual_voice < 24)
    r->virtual_voices_started |= (uint16_t)(1u << (virtual_voice - 8));
  if (!r->sequence_start_cycle) r->sequence_start_cycle = cycle;
}

void NativeAudioTraceModel_ExtendedSequenceEnd(
    uint64_t serial, uint8_t lane, uint64_t cycle) {
  NativeAudioRequestRecord *r = FindMutable(serial);
  if (!r || lane >= 2) return;
  r->active_lanes &= (uint8_t)~(1u << lane);
  if (!r->active_lanes && r->outcome == kNativeAudioOutcome_Pending)
    SetOutcome(r, kNativeAudioOutcome_Completed, cycle);
}

void NativeAudioTraceModel_ExtendedCancel(
    uint64_t serial, uint64_t cycle) {
  NativeAudioRequestRecord *r = FindMutable(serial);
  if (!r) return;
  r->active_lanes = 0;
  SetOutcome(r, kNativeAudioOutcome_CanceledSongTransition, cycle);
}

void NativeAudioTraceModel_CpuPortWrite(
    uint8_t port, uint8_t value, const char *caller,
    uint32_t game_frame, uint64_t cycle) {
  if (port == 0) {
    NativeAudioSongEventKind kind = kNativeAudioSong_Play;
    if (value == 0x00) kind = kNativeAudioSong_Idle;
    else if (value == 0xF0) kind = kNativeAudioSong_Halt;
    else if (value == 0xF1) kind = kNativeAudioSong_Prepare;
    else if (value == 0xF2) kind = kNativeAudioSong_Pause;
    else if (value == 0xFF) kind = kNativeAudioSong_Uploader;
    AddSongEvent(kind, value, 0, caller, game_frame, cycle);
    return;
  }

  int index = EffectIndexFromPort(port);
  if (index < 0) return;
  uint64_t serial = 0;
  NativeAudioRequestRecord *r = FindMutable(s_mailbox_serial[index]);
  if (r && r->id == value && value != 0) {
    serial = r->serial;
    r->flags |= kNativeAudioFlag_PortWritten;
    r->port_write_cycle = cycle;
    s_mailbox_serial[index] = 0;
  }
  QueueScheduled(index, serial, value);
}

void NativeAudioTraceModel_PortApply(
    uint8_t port, uint8_t value, uint64_t cycle) {
  int index = EffectIndexFromPort(port);
  if (index < 0) return;

  uint64_t serial = PopScheduled(index, value);
  FinishDisplacedPortSerial(s_applied_serial[index], serial, cycle);
  s_applied_serial[index] = 0;

  NativeAudioRequestRecord *r = FindMutable(serial);
  if (!r || value == 0) return;
  s_applied_serial[index] = serial;
  r->flags |= kNativeAudioFlag_PortApplied;
  r->port_apply_cycle = cycle;
}

void NativeAudioTraceModel_SpcPortRead(
    uint8_t port, uint8_t value, uint8_t dual_busy, uint64_t cycle) {
  int index = EffectIndexFromPort(port);
  if (index < 0) return;
  uint64_t serial = s_applied_serial[index];
  NativeAudioRequestRecord *r = FindMutable(serial);
  if (!r || r->id != value || value == 0) return;

  r->flags |= kNativeAudioFlag_SpcRead;
  r->spc_read_cycle = cycle;
  if (dual_busy && port == 2 && !(value & 0x80)) {
    r->flags |= kNativeAudioFlag_DualBusySeen;
    s_applied_serial[index] = 0;
    SetOutcome(r, kNativeAudioOutcome_RejectedDualBusy, cycle);
    return;
  }
  s_last_read_serial[index] = serial;
  s_applied_serial[index] = 0;
}

static void ReplaceLaneOwner(int lane, uint64_t new_serial, uint64_t cycle) {
  uint64_t old_serial = s_lane_owner[lane];
  if (old_serial && old_serial != new_serial) {
    NativeAudioRequestRecord *old = FindMutable(old_serial);
    if (old) {
      old->flags |= kNativeAudioFlag_LaneReplaced;
      old->active_lanes &= (uint8_t)~(1u << lane);
      old->replaced_by_serial = new_serial;
      SetOutcome(old, kNativeAudioOutcome_ReplacedLane, cycle);
    }
  }
  s_lane_owner[lane] = new_serial;
}

static void SequenceStart(uint8_t a, uint8_t x, uint64_t cycle) {
  int lane = x == 0x10 ? 0 : x == 0x12 ? 1 : -1;
  if (lane < 0) return;

  uint64_t serial = 0;
  if (lane == 0) {
    serial = s_last_read_serial[0];
    s_last_read_serial[0] = 0;
    if (serial && (a & 0x80)) s_dual_serial = serial;
  } else if (s_dual_serial && (a & 0x80)) {
    serial = s_dual_serial;
    s_dual_serial = 0;
  } else {
    serial = s_last_read_serial[1];
    s_last_read_serial[1] = 0;
  }

  NativeAudioRequestRecord *r = FindMutable(serial);
  if (!r) {
    /* A sequence start with no matching traced request still replaces the
     * physical lane. Clear any prior attribution rather than letting a later
     * end opcode falsely complete the old request. */
    ReplaceLaneOwner(lane, 0, cycle);
    return;
  }
  ReplaceLaneOwner(lane, serial, cycle);
  r->flags |= kNativeAudioFlag_SequenceStarted;
  r->lanes_started |= (uint8_t)(1u << lane);
  r->active_lanes |= (uint8_t)(1u << lane);
  if (!r->sequence_start_cycle) r->sequence_start_cycle = cycle;
}

static void SequenceEnd(uint8_t x, uint64_t cycle) {
  int lane = x == 0x10 ? 0 : x == 0x12 ? 1 : -1;
  if (lane < 0) return;
  uint64_t serial = s_lane_owner[lane];
  NativeAudioRequestRecord *r = FindMutable(serial);
  s_lane_owner[lane] = 0;
  if (!r) return;
  r->active_lanes &= (uint8_t)~(1u << lane);
  if (!r->active_lanes && r->outcome == kNativeAudioOutcome_Pending)
    SetOutcome(r, kNativeAudioOutcome_Completed, cycle);
}

void NativeAudioTraceModel_SpcOpcode(
    uint16_t pc, uint8_t a, uint8_t x, uint8_t dual_busy,
    uint8_t port3_value, uint64_t cycle) {
  if (pc == kSpcSequenceStart) {
    SequenceStart(a, x, cycle);
  } else if (pc == kSpcSequenceEnd) {
    SequenceEnd(x, cycle);
  } else if (pc == kSpcOrdinaryBusyGate && dual_busy && port3_value != 0) {
    NativeAudioRequestRecord *r = FindMutable(s_applied_serial[1]);
    if (r && r->id == port3_value)
      r->flags |= kNativeAudioFlag_DualBusySeen;
  }
}

void NativeAudioTraceModel_SpcUpload(
    uint32_t image_src, const char *caller,
    uint32_t game_frame, uint64_t cycle) {
  /* The HLE upload clears the runtime port queue and both input-port banks,
   * and replaces the resident sequence data. Anything already beyond the
   * game mailbox is intentionally canceled by that song-image transition. */
  for (int index = 0; index < 2; index++) {
    ScheduledPortQueue *queue = &s_scheduled[index];
    while (queue->head != queue->tail) {
      ScheduledPortValue *value =
          &queue->values[queue->head % kScheduledPortCapacity];
      SetOutcome(FindMutable(value->serial),
                 kNativeAudioOutcome_CanceledSongTransition, cycle);
      queue->head++;
    }
    SetOutcome(FindMutable(s_applied_serial[index]),
               kNativeAudioOutcome_CanceledSongTransition, cycle);
    SetOutcome(FindMutable(s_last_read_serial[index]),
               kNativeAudioOutcome_CanceledSongTransition, cycle);
    NativeAudioRequestRecord *owner = FindMutable(s_lane_owner[index]);
    if (owner) owner->active_lanes = 0;
    SetOutcome(owner, kNativeAudioOutcome_CanceledSongTransition, cycle);
    s_applied_serial[index] = 0;
    s_last_read_serial[index] = 0;
    s_lane_owner[index] = 0;
  }
  memset(s_scheduled, 0, sizeof(s_scheduled));
  s_dual_serial = 0;
  AddSongEvent(kNativeAudioSong_ImageUploaded, 0, image_src,
               caller, game_frame, cycle);
}

void NativeAudioTraceModel_DspWrite(
    uint16_t spc_pc, uint8_t spc_x, uint8_t dsp_addr, uint8_t value,
    uint8_t track_mask, uint8_t ownership_mask, uint64_t cycle) {
  s_stats.dsp_writes++;
  for (uint32_t i = 0; i < s_provenance_count; i++) {
    NativeAudioDspProvenance *p = &s_provenance[i];
    if (p->spc_pc == spc_pc && p->spc_x == spc_x &&
        p->dsp_addr == dsp_addr && p->track_mask == track_mask &&
        p->ownership_mask == ownership_mask) {
      p->writes++;
      p->last_cycle = cycle;
      p->last_value = value;
      return;
    }
  }
  if (s_provenance_count >= kDspProvenanceCapacity) {
    s_stats.provenance_overflow++;
    return;
  }
  NativeAudioDspProvenance *p = &s_provenance[s_provenance_count++];
  p->first_cycle = p->last_cycle = cycle;
  p->writes = 1;
  p->spc_pc = spc_pc;
  p->spc_x = spc_x;
  p->dsp_addr = dsp_addr;
  p->track_mask = track_mask;
  p->ownership_mask = ownership_mask;
  p->last_value = value;
}

void NativeAudioTraceModel_MusicUpdateSuppressed(
    uint16_t spc_pc, uint8_t spc_x, uint8_t track_mask,
    uint8_t ownership_mask, uint64_t cycle) {
  uint8_t overlap = (uint8_t)(track_mask & ownership_mask);
  if (!overlap) return;

  s_stats.music_updates_suppressed++;
  uint64_t owner_serial = 0;
  if (overlap & 0x40) owner_serial = s_lane_owner[0];
  if (!owner_serial && (overlap & 0x80)) owner_serial = s_lane_owner[1];
  NativeAudioRequestRecord *owner = FindMutable(owner_serial);
  if (owner) {
    owner->music_updates_suppressed++;
    owner->music_suppressed_voice_mask |= overlap;
  } else {
    s_stats.music_suppressions_unattributed++;
  }

  for (uint32_t i = 0; i < s_suppression_count; i++) {
    NativeAudioMusicSuppression *entry = &s_suppressions[i];
    if (entry->spc_pc == spc_pc && entry->spc_x == spc_x &&
        entry->track_mask == track_mask &&
        entry->ownership_mask == ownership_mask) {
      entry->occurrences++;
      entry->unattributed += owner ? 0u : 1u;
      entry->last_cycle = cycle;
      return;
    }
  }
  if (s_suppression_count >= kSuppressionProvenanceCapacity) {
    s_stats.suppression_provenance_overflow++;
    return;
  }
  NativeAudioMusicSuppression *entry =
      &s_suppressions[s_suppression_count++];
  entry->first_cycle = entry->last_cycle = cycle;
  entry->occurrences = 1;
  entry->unattributed = owner ? 0 : 1;
  entry->spc_pc = spc_pc;
  entry->spc_x = spc_x;
  entry->track_mask = track_mask;
  entry->ownership_mask = ownership_mask;
}

const NativeAudioRequestRecord *NativeAudioTraceModel_FindRequest(
    uint64_t serial) {
  return FindMutable(serial);
}

size_t NativeAudioTraceModel_CopyRequests(
    NativeAudioRequestRecord *out, size_t capacity) {
  if (!out || !capacity) return 0;
  uint64_t first = s_next_serial > kRequestCapacity
                       ? s_next_serial - kRequestCapacity
                       : 1;
  size_t count = 0;
  for (uint64_t serial = first; serial < s_next_serial && count < capacity;
       serial++) {
    const NativeAudioRequestRecord *r =
        NativeAudioTraceModel_FindRequest(serial);
    if (r) out[count++] = *r;
  }
  return count;
}

size_t NativeAudioTraceModel_CopySongEvents(
    NativeAudioSongEvent *out, size_t capacity) {
  size_t count = s_song_event_count < capacity ? s_song_event_count : capacity;
  if (out && count) memcpy(out, s_song_events, count * sizeof(*out));
  return count;
}

size_t NativeAudioTraceModel_CopyDspProvenance(
    NativeAudioDspProvenance *out, size_t capacity) {
  size_t count = s_provenance_count < capacity ? s_provenance_count : capacity;
  if (out && count) memcpy(out, s_provenance, count * sizeof(*out));
  return count;
}

size_t NativeAudioTraceModel_CopyMusicSuppressions(
    NativeAudioMusicSuppression *out, size_t capacity) {
  size_t count =
      s_suppression_count < capacity ? s_suppression_count : capacity;
  if (out && count) memcpy(out, s_suppressions, count * sizeof(*out));
  return count;
}

void NativeAudioTraceModel_GetStats(NativeAudioTraceStats *out) {
  if (out) *out = s_stats;
}

const char *NativeAudioTrace_OutcomeName(NativeAudioRequestOutcome outcome) {
  static const char *const names[kNativeAudioOutcome_Count] = {
    "pending", "suppressed_setting", "coalesced_mailbox_duplicate",
    "overwritten_mailbox", "coalesced_port_duplicate", "overwritten_port",
    "rejected_dual_busy", "replaced_lane", "canceled_song_transition",
    "coalesced_extended_duplicate", "extended_fifo_overflow", "completed"
  };
  return outcome < kNativeAudioOutcome_Count ? names[outcome] : "unknown";
}

const char *NativeAudioTrace_SongEventName(NativeAudioSongEventKind kind) {
  static const char *const names[] = {
    "idle", "halt", "prepare", "pause", "uploader", "play", "image_uploaded"
  };
  return kind <= kNativeAudioSong_ImageUploaded ? names[kind] : "unknown";
}
