#include "native_audio_extension.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "settings.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/snes.h"
#include "snes/spc.h"

enum {
  kEventTrack = 0x10,
  kOrdinarySfxTrack = 0x12,
  kEventHardwareVoice = 6,
  kOrdinarySfxHardwareVoice = 7,
  kEventVirtualVoice = 8,
  kOrdinarySfxVirtualVoice = 9,
  kEventMask = 0x40,
  kOrdinarySfxMask = 0x80,
  kEffectMask = kEventMask | kOrdinarySfxMask,
  kCurrentTrackMaskAddress = 0x47,
  kEffectOwnershipMaskAddress = 0x1a,
  kEffectUpdateEntry = 0x0e7f,
  kEffectUpdateReturn = 0x0f0b,
  kEffectCleanupReturn = 0x0e7e,
  kEffectEmptyReturn = 0x0e83,
  kEffectDriverEntry = 0x0da0,
  kEffectDriverReturn = 0x0e13,
  kVirtualVoiceFirst = kDspHardwareVoiceCount,
  kVirtualVoicePoolSize = kDspExtendedVoiceCount,
  kRequestQueueCapacity = 128,
  kTrackStatePairCount = 29,
  kTrackStateBytes = kTrackStatePairCount * 2,
};

typedef struct NativeAudioQueuedRequest {
  uint64_t serial;
  uint64_t trace_serial;
  uint32_t caller_pc;
  uint32_t game_frame;
  uint16_t actor_x;
  uint16_t actor_y;
  uint8_t id;
  uint8_t event_request;
} NativeAudioQueuedRequest;

typedef struct NativeAudioEffectInstance {
  uint64_t serial;
  uint64_t trace_serial;
  uint32_t caller_pc;
  uint32_t game_frame;
  uint16_t actor_x;
  uint16_t actor_y;
  uint8_t state[kTrackStateBytes];
  uint8_t id;
  uint8_t voice;
  uint8_t lane_track;
  uint8_t lane_mask;
  uint8_t active;
  uint8_t event_request;
  uint8_t paired;
  uint8_t pair_phase;
  uint8_t noise_enabled;
  uint8_t start_kof_stage;
  uint8_t pending_kon;
  uint8_t ending;
  uint8_t end_kof_stage;
  uint8_t release_after_kon;
} NativeAudioEffectInstance;

typedef struct NativeAudioExtensionState {
  uint64_t next_serial;
  NativeAudioQueuedRequest queue[kRequestQueueCapacity];
  NativeAudioEffectInstance instance[kVirtualVoicePoolSize];
  uint32_t queue_head;
  uint32_t queue_count;
  uint32_t coalesced_count;
  uint32_t overflow_count;
  uint16_t schedule_mask;
  uint8_t scheduler_active;
  uint8_t current_slot;
  uint8_t active_virtual_voice;
  uint8_t charged_lane_mask;
  uint8_t current_update_free;
} NativeAudioExtensionState;

/* Every per-track array is packed for ten native logical tracks: X is the
 * even byte offset $00,$02,...,$12. Saving these 29 pairs is the complete
 * mutable context consumed by $0E7F-$0F0B. The first six arrays are direct
 * page; the remaining 23 repeat every $14 bytes through $03CB. */
static const uint16_t kTrackStateBase[kTrackStatePairCount] = {
  0x0070, 0x0084, 0x0098, 0x00ac, 0x00c0, 0x00d4,
  0x0200, 0x0214, 0x0228, 0x023c, 0x0250, 0x0264,
  0x0278, 0x028c, 0x02a0, 0x02b4, 0x02c8, 0x02dc,
  0x02f0, 0x0304, 0x0318, 0x032c, 0x0340, 0x0354,
  0x0368, 0x037c, 0x0390, 0x03a4, 0x03b8,
};

static bool s_enabled;
static bool s_log;
static bool (*s_previous_filter)(Apu *, uint8_t, uint8_t *);
static void (*s_previous_opcode_patch)(Spc *, uint16_t);
static int (*s_previous_cycle_hook)(Spc *, uint16_t, int);
static void (*s_previous_extra_saveload)(Apu *, SaveLoadInfo *);
static void (*s_previous_upload_hook)(uint32_t);
static NativeAudioExtensionState s_state;
static uint8_t s_sequence_track = 0xff;
static uint8_t s_sequence_mask;
static uint8_t s_song_kon_pending_mask;
static uint8_t s_logged_routed_mask[0x80];
static uint8_t s_logged_routed_value[0x80];
static bool s_logged_routed_seen[0x80];
static bool s_current_opcode_free;

void (*g_native_audio_extension_trace_disposition_hook)(
    uint64_t, uint64_t, bool, bool) = NULL;
void (*g_native_audio_extension_trace_start_hook)(
    uint64_t, uint8_t, uint8_t) = NULL;
void (*g_native_audio_extension_trace_end_hook)(uint64_t, uint8_t) = NULL;
void (*g_native_audio_extension_trace_cancel_hook)(uint64_t) = NULL;

extern void (*g_rtl_spc_upload_hook)(uint32_t src);
extern Snes *g_snes;

static bool IsPerVoiceWritable(uint8_t addr) {
  return addr < 0x80 && (addr & 0x0f) < 8;
}

static bool IsVoiceMaskRegister(uint8_t addr) {
  return addr == 0x2d || addr == 0x3d || addr == 0x4d ||
      addr == 0x4c || addr == 0x5c;
}

static void LoadTrackState(Apu *apu, const NativeAudioEffectInstance *instance) {
  if (!apu || !instance) return;
  for (int i = 0; i < kTrackStatePairCount; i++) {
    const uint16_t address =
        (uint16_t)(kTrackStateBase[i] + instance->lane_track);
    apu->ram[address] = instance->state[i * 2];
    apu->ram[(uint16_t)(address + 1)] = instance->state[i * 2 + 1];
  }
}

static void SaveTrackState(Apu *apu, NativeAudioEffectInstance *instance) {
  if (!apu || !instance) return;
  for (int i = 0; i < kTrackStatePairCount; i++) {
    const uint16_t address =
        (uint16_t)(kTrackStateBase[i] + instance->lane_track);
    instance->state[i * 2] = apu->ram[address];
    instance->state[i * 2 + 1] =
        apu->ram[(uint16_t)(address + 1)];
  }
}

static bool SameProducer(const NativeAudioQueuedRequest *request,
                         bool event_request, uint8_t id,
                         uint32_t caller_pc, uint32_t game_frame,
                         uint16_t actor_x, uint16_t actor_y) {
  const bool same_site =
      request && request->event_request == (uint8_t)event_request &&
      request->id == id && request->caller_pc == caller_pc &&
      request->game_frame == game_frame;
  if (!same_site) return false;
  /* The message compositor advances X/Y for every glyph even though all of
   * its $07 posts are one depth-one pacing blip. Preserve that native
   * coalescing explicitly; gameplay producer loops retain actor identity. */
  if (event_request && id == 0x07 && caller_pc == 0x01902d)
    return true;
  return request->actor_x == actor_x && request->actor_y == actor_y;
}

static bool SameActiveProducer(const NativeAudioEffectInstance *instance,
                               bool event_request, uint8_t id,
                               uint32_t caller_pc, uint32_t game_frame,
                               uint16_t actor_x, uint16_t actor_y) {
  const bool same_site = instance && instance->active && !instance->ending &&
      instance->event_request == (uint8_t)event_request &&
      instance->id == id && instance->caller_pc == caller_pc &&
      instance->game_frame == game_frame;
  if (!same_site) return false;
  if (event_request && id == 0x07 && caller_pc == 0x01902d)
    return true;
  return instance->actor_x == actor_x && instance->actor_y == actor_y;
}

bool NativeAudioExtension_QueueRequest(
    bool event_request, uint8_t id, uint32_t caller_pc,
    uint32_t game_frame, uint16_t actor_x, uint16_t actor_y,
    uint64_t trace_serial) {
  if (!s_enabled) return false;
  /* Zero is the native mailbox's idle/clear value, never a sequence. */
  if (id == 0) return true;

  RtlApuLock();
  for (uint32_t i = 0; i < s_state.queue_count; i++) {
    const uint32_t index =
        (s_state.queue_head + i) % kRequestQueueCapacity;
    if (SameProducer(&s_state.queue[index], event_request, id, caller_pc,
                     game_frame, actor_x, actor_y)) {
      s_state.coalesced_count++;
      if (g_native_audio_extension_trace_disposition_hook)
        g_native_audio_extension_trace_disposition_hook(
            trace_serial, s_state.queue[index].trace_serial, true, false);
      RtlApuUnlock();
      return true;
    }
  }
  for (int i = 0; i < kVirtualVoicePoolSize; i++) {
    if (SameActiveProducer(&s_state.instance[i], event_request, id,
                           caller_pc, game_frame, actor_x, actor_y)) {
      s_state.coalesced_count++;
      if (g_native_audio_extension_trace_disposition_hook)
        g_native_audio_extension_trace_disposition_hook(
            trace_serial, s_state.instance[i].trace_serial, true, false);
      RtlApuUnlock();
      return true;
    }
  }

  if (s_state.queue_count == kRequestQueueCapacity) {
    s_state.overflow_count++;
    if (s_log)
      fprintf(stderr,
              "[audio-ext] request FIFO full; dropped %s id=%02x "
              "site=%06x frame=%u\n",
              event_request ? "event" : "sfx", id, caller_pc, game_frame);
    if (g_native_audio_extension_trace_disposition_hook)
      g_native_audio_extension_trace_disposition_hook(
          trace_serial, 0, false, true);
    RtlApuUnlock();
    return true;
  }

  const uint32_t tail =
      (s_state.queue_head + s_state.queue_count) % kRequestQueueCapacity;
  NativeAudioQueuedRequest *request = &s_state.queue[tail];
  memset(request, 0, sizeof(*request));
  request->serial = ++s_state.next_serial;
  request->trace_serial = trace_serial;
  request->caller_pc = caller_pc;
  request->game_frame = game_frame;
  request->actor_x = actor_x;
  request->actor_y = actor_y;
  request->id = id;
  request->event_request = (uint8_t)event_request;
  s_state.queue_count++;
  if (g_native_audio_extension_trace_disposition_hook)
    g_native_audio_extension_trace_disposition_hook(
        trace_serial, 0, false, false);
  if (s_log)
    fprintf(stderr,
            "[audio-ext] queued serial=%llu %s id=%02x site=%06x "
            "frame=%u depth=%u\n",
            (unsigned long long)request->serial,
            event_request ? "event" : "sfx", id, caller_pc, game_frame,
            s_state.queue_count);
  RtlApuUnlock();
  return true;
}

int NativeAudioExtension_QueuedRequestCount(void) {
  return (int)s_state.queue_count;
}

int NativeAudioExtension_ActiveInstanceCount(void) {
  int count = 0;
  for (int i = 0; i < kVirtualVoicePoolSize; i++)
    count += s_state.instance[i].active && !s_state.instance[i].ending;
  return count;
}

bool NativeAudioExtension_RouteVoiceWrite(
    uint8_t dsp_addr, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask, int *hardware_voice, int *virtual_voice) {
  if (!IsPerVoiceWritable(dsp_addr)) return false;
  const int physical = dsp_addr >> 4;
  const uint8_t expected_mask = (uint8_t)(1u << physical);
  if (track_mask != expected_mask) return false;

  int mapped = -1;
  if (logical_track == kEventTrack &&
      physical == kEventHardwareVoice) {
    mapped = kEventVirtualVoice;
  } else if (logical_track == kOrdinarySfxTrack &&
             physical == kOrdinarySfxHardwareVoice) {
    mapped = kOrdinarySfxVirtualVoice;
  } else if (logical_track <= 0x0e && !(logical_track & 1) &&
             (logical_track >> 1) == physical) {
    return false; /* proven song write, even while the lane is owned */
  } else if (physical >= kEventHardwareVoice &&
             (ownership_mask & expected_mask)) {
    /* Shared driver helpers repurpose X for a DSP register address. The
     * ownership bit remains the proof that their current destination is the
     * effect lane, matching the mixer classifier's conservative fallback. */
    mapped = physical == kEventHardwareVoice
        ? kEventVirtualVoice : kOrdinarySfxVirtualVoice;
  }
  if (mapped < 0) return false;
  if (hardware_voice) *hardware_voice = physical;
  if (virtual_voice) *virtual_voice = mapped;
  return true;
}

uint8_t NativeAudioExtension_RoutedGlobalMask(
    uint8_t logical_track, uint8_t track_mask, uint8_t ownership_mask) {
  if (logical_track == kEventTrack && track_mask == kEventMask)
    return kEventMask;
  if (logical_track == kOrdinarySfxTrack &&
      track_mask == kOrdinarySfxMask)
    return kOrdinarySfxMask;
  if (logical_track <= 0x0e && !(logical_track & 1) &&
      track_mask == (uint8_t)(1u << (logical_track >> 1)))
    return 0; /* proven song mask write */

  /* The central DSP-mask flush at $0458 has already repurposed X and clears
   * $47. At that point $1A is the only authoritative split. A nonzero $47
   * fallback is used only when it agrees with ownership. */
  if (track_mask == 0)
    return ownership_mask & kEffectMask;
  return track_mask & ownership_mask & kEffectMask;
}

bool NativeAudioExtension_ShouldBypassMusicSuppression(
    uint16_t spc_pc, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask) {
  if (spc_pc != 0x04d4 && spc_pc != 0x05b6 && spc_pc != 0x080e)
    return false;
  if (logical_track > 0x0e || (logical_track & 1)) return false;
  const uint8_t song_mask = (uint8_t)(1u << (logical_track >> 1));
  return track_mask == song_mask && (ownership_mask & song_mask) != 0;
}

static void ClearEffectScratchMasks(Apu *apu) {
  if (!apu) return;
  const uint8_t keep = (uint8_t)~kEffectMask;
  apu->ram[0x1a] &= keep;
  apu->ram[0x36] &= keep;
  apu->ram[0x37] &= keep;
  apu->ram[0x45] &= keep; /* pending KON */
  apu->ram[0x46] &= keep; /* pending KOF */
  apu->ram[0x49] &= keep; /* noise mask */
  apu->ram[0x5e] &= keep; /* voice-parameter dirty mask */
}

static bool PairIsActive(uint64_t serial) {
  int lanes = 0;
  for (int i = 0; i < kVirtualVoicePoolSize; i++) {
    const NativeAudioEffectInstance *instance = &s_state.instance[i];
    if (instance->active && !instance->ending && instance->paired &&
        instance->serial == serial)
      lanes++;
  }
  return lanes == 2;
}

static void SynchronizePairPhase(uint64_t serial, uint8_t phase) {
  for (int i = 0; i < kVirtualVoicePoolSize; i++) {
    NativeAudioEffectInstance *instance = &s_state.instance[i];
    if (instance->active && !instance->ending && instance->paired &&
        instance->serial == serial)
      instance->pair_phase = phase;
  }
}

static void InitializeTrackState(Apu *apu,
                                 NativeAudioEffectInstance *instance) {
  memset(instance->state, 0, sizeof(instance->state));
  LoadTrackState(apu, instance);

  uint8_t sequence = instance->id & 0x7f;
  if (sequence >= 0x27) sequence = 0x07;
  const uint16_t table = (uint16_t)(0x2400 + sequence * 2);
  const uint8_t x = instance->lane_track;
  apu->ram[(uint16_t)(0x00d4 + x)] = apu->ram[table];
  apu->ram[(uint16_t)(0x00d5 + x)] =
      apu->ram[(uint16_t)(table + 1)];
  apu->ram[(uint16_t)(0x0305 + x)] = 0xdc;
  apu->ram[(uint16_t)(0x0369 + x)] = 0x0a;
  apu->ram[(uint16_t)(0x0341 + x)] = 0x0a;
  apu->ram[(uint16_t)(0x0340 + x)] = 0;
  apu->ram[(uint16_t)(0x0215 + x)] = 0;
  apu->ram[(uint16_t)(0x03a5 + x)] = 0;
  apu->ram[(uint16_t)(0x02f0 + x)] = 0;
  apu->ram[(uint16_t)(0x0264 + x)] = 0;
  apu->ram[(uint16_t)(0x00ad + x)] = 0;
  apu->ram[(uint16_t)(0x00c1 + x)] = 0;
  apu->ram[(uint16_t)(0x03b8 + x)] = 0;
  apu->ram[(uint16_t)(0x0084 + x)] = 0;
  apu->ram[(uint16_t)(0x0085 + x)] = 0;
  apu->ram[(uint16_t)(0x0070 + x)] =
      instance->paired && x == kOrdinarySfxTrack ? 3 : 2;
  SaveTrackState(apu, instance);
}

static int FindFreeInstanceSlot(int after) {
  for (int i = after + 1; i < kVirtualVoicePoolSize; i++) {
    if (!s_state.instance[i].active)
      return i;
  }
  return -1;
}

static void StartNewInstance(Apu *apu, int slot,
                             const NativeAudioQueuedRequest *request,
                             uint8_t lane_track, bool paired) {
  NativeAudioEffectInstance *instance = &s_state.instance[slot];
  memset(instance, 0, sizeof(*instance));
  instance->serial = request->serial;
  instance->trace_serial = request->trace_serial;
  instance->caller_pc = request->caller_pc;
  instance->game_frame = request->game_frame;
  instance->actor_x = request->actor_x;
  instance->actor_y = request->actor_y;
  instance->id = request->id;
  instance->voice = (uint8_t)(kVirtualVoiceFirst + slot);
  instance->lane_track = lane_track;
  instance->lane_mask = lane_track == kEventTrack
      ? kEventMask : kOrdinarySfxMask;
  instance->active = 1;
  instance->event_request = request->event_request;
  instance->paired = paired;
  instance->start_kof_stage = 2;
  InitializeTrackState(apu, instance);
  dsp_setVoiceBus(apu->dsp, instance->voice, kDspVoiceBus_Sfx);
  dsp_writeVirtualVoiceControl(apu->dsp, instance->voice, 0x2d, false);
  dsp_writeVirtualVoiceControl(apu->dsp, instance->voice, 0x3d, false);
  dsp_writeVirtualVoiceControl(apu->dsp, instance->voice, 0x4d, false);
  if (g_native_audio_extension_trace_start_hook)
    g_native_audio_extension_trace_start_hook(
        instance->trace_serial,
        instance->lane_track == kEventTrack ? 0 : 1,
        instance->voice);
  if (s_log)
    fprintf(stderr,
            "[audio-ext] start serial=%llu id=%02x lane=%02x voice=%u%s\n",
            (unsigned long long)instance->serial, instance->id,
            instance->lane_track, instance->voice,
            paired ? " paired" : "");
}

static bool AllocateFrontRequest(Apu *apu) {
  if (!apu || !apu->dsp || s_state.queue_count == 0) return false;
  NativeAudioQueuedRequest *request =
      &s_state.queue[s_state.queue_head % kRequestQueueCapacity];
  uint8_t sequence = request->id & 0x7f;
  if (sequence >= 0x27) sequence = 0x07;
  const uint16_t table = (uint16_t)(0x2400 + sequence * 2);
  if ((apu->ram[table] | apu->ram[(uint16_t)(table + 1)]) == 0)
    return false; /* common effect image is not installed yet */

  const bool paired = request->event_request && (request->id & 0x80);
  const int first = FindFreeInstanceSlot(-1);
  if (first < 0) return false;
  const int second = paired ? FindFreeInstanceSlot(first) : -1;
  if (paired && second < 0) return false;

  StartNewInstance(apu, first, request,
                   request->event_request ? kEventTrack : kOrdinarySfxTrack,
                   paired);
  if (paired)
    StartNewInstance(apu, second, request, kOrdinarySfxTrack, true);
  s_state.queue_head =
      (s_state.queue_head + 1) % kRequestQueueCapacity;
  s_state.queue_count--;
  return true;
}

static void AllocateQueuedRequests(Apu *apu) {
  while (AllocateFrontRequest(apu)) {}
}

static int FirstScheduledSlot(void) {
  for (int i = 0; i < kVirtualVoicePoolSize; i++) {
    if ((s_state.schedule_mask & (uint16_t)(1u << i)) &&
        !s_state.instance[i].ending)
      return i;
  }
  return -1;
}

static void LoadScheduledInstance(Spc *spc, int slot) {
  Apu *apu = spc->apu;
  NativeAudioEffectInstance *instance = &s_state.instance[slot];
  LoadTrackState(apu, instance);
  ClearEffectScratchMasks(apu);
  apu->ram[kCurrentTrackMaskAddress] = instance->lane_mask;
  apu->ram[0x34] = 0xff;
  apu->ram[0x35] = PairIsActive(instance->serial) ? 0xff : 0;
  apu->ram[0x3f] = instance->pair_phase;
  if (instance->noise_enabled)
    apu->ram[0x49] |= instance->lane_mask;
  spc->x = instance->lane_track;
  spc->pc = kEffectUpdateEntry;
  s_state.scheduler_active = 1;
  s_state.current_slot = (uint8_t)slot;
  s_state.active_virtual_voice = instance->voice;
  const uint8_t lane_bit = instance->lane_track == kEventTrack ? 1 : 2;
  s_state.current_update_free =
      (s_state.charged_lane_mask & lane_bit) != 0;
  s_state.charged_lane_mask |= lane_bit;
}

static void FinishScheduledInstance(Spc *spc, bool ended) {
  Apu *apu = spc->apu;
  const int slot = s_state.current_slot;
  if (slot < 0 || slot >= kVirtualVoicePoolSize) return;
  NativeAudioEffectInstance *instance = &s_state.instance[slot];
  SaveTrackState(apu, instance);
  instance->noise_enabled =
      (apu->ram[0x49] & instance->lane_mask) != 0;
  dsp_writeVirtualVoiceControl(apu->dsp, instance->voice, 0x3d,
                               instance->noise_enabled != 0);

  if (apu->ram[0x45] & instance->lane_mask)
    instance->pending_kon = 1;

  if (instance->paired)
    SynchronizePairPhase(instance->serial, apu->ram[0x3f]);
  ClearEffectScratchMasks(apu);
  s_state.schedule_mask &= (uint16_t)~(1u << slot);
  if (ended) {
    if (s_log)
      fprintf(stderr,
              "[audio-ext] end serial=%llu id=%02x lane=%02x voice=%u\n",
              (unsigned long long)instance->serial, instance->id,
              instance->lane_track, instance->voice);
    if (g_native_audio_extension_trace_end_hook)
      g_native_audio_extension_trace_end_hook(
          instance->trace_serial,
          instance->lane_track == kEventTrack ? 0 : 1);
    /* Cleanup's KOF is applied by the same central $0458 flush that services
     * native voices. Hold this pool slot through the following clear flush so
     * KOF cannot overlap a newly allocated KON on the same virtual voice. */
    instance->ending = 1;
    instance->end_kof_stage = 2;
  }

  const int next = FirstScheduledSlot();
  if (next >= 0) {
    LoadScheduledInstance(spc, next);
    return;
  }
  s_state.scheduler_active = 0;
  s_state.current_slot = 0xff;
  s_state.active_virtual_voice = 0xff;
  apu->ram[0x34] = 0;
  apu->ram[0x35] = 0;
  apu->ram[kCurrentTrackMaskAddress] = 0;
  ClearEffectScratchMasks(apu);
}

static void StartEffectScheduler(Spc *spc) {
  Apu *apu = spc->apu;
  AllocateQueuedRequests(apu);
  /* Preserve one native SPC update cost for X=$10 and one for X=$12.
   * Additional instances of either lane are extension work and execute in
   * host time, so overlap cannot slow the song timer merely because the pool
   * is busy. */
  s_state.charged_lane_mask = 0;
  s_state.schedule_mask = 0;
  for (int i = 0; i < kVirtualVoicePoolSize; i++) {
    if (s_state.instance[i].active && !s_state.instance[i].ending)
      s_state.schedule_mask |= (uint16_t)(1u << i);
  }
  const int first = FirstScheduledSlot();
  if (first >= 0) {
    LoadScheduledInstance(spc, first);
  } else {
    ClearEffectScratchMasks(apu);
    apu->ram[0x34] = 0;
    apu->ram[0x35] = 0;
    apu->ram[kCurrentTrackMaskAddress] = 0;
    spc->pc = kEffectDriverReturn;
  }
}

static void FlushVirtualLifecycleControls(Apu *apu, uint8_t addr) {
  if (!apu || !apu->dsp || (addr != 0x4c && addr != 0x5c)) return;
  for (int i = 0; i < kVirtualVoicePoolSize; i++) {
    NativeAudioEffectInstance *instance = &s_state.instance[i];
    if (!instance->active) continue;
    if (addr == 0x5c) {
      if (instance->ending) {
        if (instance->end_kof_stage == 2) {
          dsp_writeVirtualVoiceControl(
              apu->dsp, instance->voice, 0x5c, true);
          instance->end_kof_stage = 1;
        } else if (instance->end_kof_stage == 1) {
          dsp_writeVirtualVoiceControl(
              apu->dsp, instance->voice, 0x5c, false);
          instance->end_kof_stage = 0;
          instance->release_after_kon = 1;
        }
      } else if (instance->start_kof_stage == 2) {
        dsp_writeVirtualVoiceControl(
            apu->dsp, instance->voice, 0x5c, true);
        instance->start_kof_stage = 1;
      } else if (instance->start_kof_stage == 1) {
        dsp_writeVirtualVoiceControl(
            apu->dsp, instance->voice, 0x5c, false);
        instance->start_kof_stage = 0;
      }
    } else {
      dsp_writeVirtualVoiceControl(
          apu->dsp, instance->voice, 0x4c,
          instance->pending_kon != 0);
      instance->pending_kon = 0;
      if (instance->ending && instance->release_after_kon) {
        instance->active = 0;
        instance->release_after_kon = 0;
      }
    }
  }
}

static int RoutedVirtualVoice(uint8_t effect_mask, int phase1_voice) {
  if (s_state.scheduler_active &&
      s_state.current_slot < kVirtualVoicePoolSize) {
    const NativeAudioEffectInstance *instance =
        &s_state.instance[s_state.current_slot];
    if (instance->active && (instance->lane_mask & effect_mask))
      return s_state.active_virtual_voice;
  }
  return phase1_voice;
}

static bool RouteDspWrite(Apu *apu, uint8_t addr, uint8_t *value) {
  if (s_previous_filter && !s_previous_filter(apu, addr, value))
    return false;
  if (!s_enabled || !apu || !apu->dsp || !apu->spc || !value)
    return true;

  const uint8_t logical_track = apu->spc->x;
  const uint8_t track_mask = apu->ram[kCurrentTrackMaskAddress];
  const uint8_t ownership = apu->ram[kEffectOwnershipMaskAddress];
  if (track_mask == 0)
    FlushVirtualLifecycleControls(apu, addr);
  s_song_kon_pending_mask &= ownership;
  /* $080A still has the sequencer track in X. Its shared writer at $0834
   * replaces X with $64-$67/$74-$77 and can temporarily see $1A clear. Carry
   * the proven track across that helper so SRCN/ADSR/GAIN follow the same
   * route as pitch/volume instead of leaking back onto song voice 6/7. */
  uint8_t routed_track = logical_track;
  if (IsPerVoiceWritable(addr) && logical_track == addr &&
      track_mask == s_sequence_mask)
    routed_track = s_sequence_track;
  int hardware_voice = -1;
  int virtual_voice = -1;
  if (NativeAudioExtension_RouteVoiceWrite(
          addr, routed_track, track_mask, ownership,
          &hardware_voice, &virtual_voice)) {
    virtual_voice = RoutedVirtualVoice(track_mask, virtual_voice);
    dsp_setVoiceBus(apu->dsp, hardware_voice, kDspVoiceBus_Music);
    dsp_setVoiceBus(apu->dsp, virtual_voice, kDspVoiceBus_Sfx);
    dsp_writeVirtualVoiceRegister(apu->dsp, virtual_voice, addr, *value);
    if (s_log) {
      fprintf(stderr,
              "[audio-ext] voice-write dsp=%02x x=%02x mask=%02x "
              "owner=%02x physical=%d virtual=%d value=%02x\n",
              addr, routed_track, track_mask, ownership,
              hardware_voice, virtual_voice, *value);
    }
    return false;
  }

  if (IsVoiceMaskRegister(addr)) {
    const uint8_t route_mask = NativeAudioExtension_RoutedGlobalMask(
        logical_track, track_mask, ownership);
    if (route_mask) {
      if (route_mask & kEventMask) {
        const int voice = RoutedVirtualVoice(
            kEventMask, kEventVirtualVoice);
        dsp_setVoiceBus(apu->dsp, voice, kDspVoiceBus_Sfx);
        dsp_writeVirtualVoiceControl(
            apu->dsp, voice, addr,
            (*value & kEventMask) != 0);
      }
      if (route_mask & kOrdinarySfxMask) {
        const int voice = RoutedVirtualVoice(
            kOrdinarySfxMask, kOrdinarySfxVirtualVoice);
        dsp_setVoiceBus(
            apu->dsp, voice, kDspVoiceBus_Sfx);
        dsp_writeVirtualVoiceControl(
            apu->dsp, voice, addr,
            (*value & kOrdinarySfxMask) != 0);
      }
      uint8_t hardware_value = *value;
      uint8_t hardware_update_mask = (uint8_t)~route_mask;
      if (track_mask == 0 && addr == 0x5c) {
        /* The central mask flush's owned bit describes the effect KOF, not
         * the song. Clear the physical KOF latch normally so a direct song
         * KOF allowed through $05B6 cannot remain held until effect end. */
        hardware_value &= (uint8_t)~route_mask;
        hardware_update_mask = 0xff;
      } else if (track_mask == 0 && addr == 0x4c) {
        /* A song note admitted at $04D4 sets the same driver KON bit as the
         * effect. Apply that one proven pending bit to both destinations;
         * otherwise the owned central KON remains virtual-only. */
        hardware_update_mask |= s_song_kon_pending_mask & route_mask;
        s_song_kon_pending_mask &= (uint8_t)~route_mask;
      }
      dsp_writeHardwareVoiceMask(
          apu->dsp, addr, hardware_value, hardware_update_mask);
      const uint8_t routed_value = *value & route_mask;
      if (s_log &&
          (!s_logged_routed_seen[addr] ||
           s_logged_routed_mask[addr] != route_mask ||
           s_logged_routed_value[addr] != routed_value)) {
        fprintf(stderr,
                "[audio-ext] mask-write dsp=%02x x=%02x mask=%02x "
                "owner=%02x routed=%02x value=%02x\n",
                addr, logical_track, track_mask, ownership,
                route_mask, *value);
        s_logged_routed_seen[addr] = true;
        s_logged_routed_mask[addr] = route_mask;
        s_logged_routed_value[addr] = routed_value;
      }
      return false;
    }
  }
  return true;
}

static void PatchSpcOpcode(Spc *spc, uint16_t pc) {
  s_current_opcode_free = s_enabled && s_state.scheduler_active &&
      s_state.current_update_free;
  if (s_previous_opcode_patch)
    s_previous_opcode_patch(spc, pc);
  if (!s_enabled || !spc || !spc->apu) return;
  if (pc == kEffectDriverEntry) {
    StartEffectScheduler(spc);
    s_current_opcode_free =
        s_state.scheduler_active && s_state.current_update_free;
    return;
  }
  if (s_state.scheduler_active &&
      (pc == kEffectUpdateReturn || pc == kEffectCleanupReturn ||
       pc == kEffectEmptyReturn)) {
    FinishScheduledInstance(
        spc, pc == kEffectCleanupReturn || pc == kEffectEmptyReturn);
    if (spc->pc == kEffectUpdateEntry)
      s_current_opcode_free =
          s_state.scheduler_active && s_state.current_update_free;
    return;
  }
  if (pc == 0x080a) {
    s_sequence_track = spc->x;
    s_sequence_mask = spc->apu->ram[kCurrentTrackMaskAddress];
  }
  if (NativeAudioExtension_ShouldBypassMusicSuppression(
          pc, spc->x, spc->apu->ram[kCurrentTrackMaskAddress],
          spc->apu->ram[kEffectOwnershipMaskAddress])) {
    /* All three sites are BNE instructions whose Z flag was produced by
     * `$1A & $47`. Making only that proven branch fall through allows the
     * already-advancing song track to reach its normal DSP write. */
    spc->z = true;
    if (pc == 0x04d4)
      s_song_kon_pending_mask |=
          spc->apu->ram[kCurrentTrackMaskAddress];
    if (s_log) {
      fprintf(stderr,
              "[audio-ext] preserved song update pc=%04x x=%02x "
              "mask=%02x owner=%02x\n",
              pc, spc->x, spc->apu->ram[kCurrentTrackMaskAddress],
              spc->apu->ram[kEffectOwnershipMaskAddress]);
    }
  }
}

static int AdjustSpcOpcodeCycles(Spc *spc, uint16_t pc, int cycles) {
  if (s_previous_cycle_hook)
    cycles = s_previous_cycle_hook(spc, pc, cycles);
  return s_enabled && s_current_opcode_free ? 0 : cycles;
}

static void SaveLoadExtensionState(Apu *apu, SaveLoadInfo *sli) {
  if (s_previous_extra_saveload)
    s_previous_extra_saveload(apu, sli);
  if (s_enabled && sli)
    sli->func(sli, &s_state, sizeof(s_state));
}

static void CancelAllEffectsLocked(Apu *apu) {
  const uint64_t next_serial = s_state.next_serial;
  if (g_native_audio_extension_trace_cancel_hook) {
    for (uint32_t i = 0; i < s_state.queue_count; i++) {
      const uint32_t index =
          (s_state.queue_head + i) % kRequestQueueCapacity;
      g_native_audio_extension_trace_cancel_hook(
          s_state.queue[index].trace_serial);
    }
  }
  if (apu && apu->dsp) {
    for (int i = 0; i < kVirtualVoicePoolSize; i++) {
      if (s_state.instance[i].active) {
        dsp_writeVirtualVoiceControl(
            apu->dsp, kVirtualVoiceFirst + i, 0x5c, true);
        if (g_native_audio_extension_trace_cancel_hook)
          g_native_audio_extension_trace_cancel_hook(
              s_state.instance[i].trace_serial);
      }
    }
  }
  memset(&s_state, 0, sizeof(s_state));
  s_state.next_serial = next_serial;
  s_state.current_slot = 0xff;
  s_state.active_virtual_voice = 0xff;
  if (apu)
    ClearEffectScratchMasks(apu);
}

static void OnSpcUpload(uint32_t source) {
  if (s_previous_upload_hook)
    s_previous_upload_hook(source);
  if (!s_enabled || !g_snes || !g_snes->apu) return;
  RtlApuLock();
  CancelAllEffectsLocked(g_snes->apu);
  RtlApuUnlock();
  if (s_log)
    fprintf(stderr,
            "[audio-ext] canceled queued/active effects for SPC upload %06x\n",
            source);
}

void NativeAudioExtension_Install(void) {
  s_enabled = g_settings.audio_extended_channels;
  const char *log = getenv("AR_AUDIO_EXTLOG");
  s_log = log && log[0] && log[0] != '0';
  s_sequence_track = 0xff;
  s_sequence_mask = 0;
  s_song_kon_pending_mask = 0;
  memset(&s_state, 0, sizeof(s_state));
  s_state.current_slot = 0xff;
  s_state.active_virtual_voice = 0xff;
  memset(s_logged_routed_seen, 0, sizeof(s_logged_routed_seen));
  dsp_setExtendedVoicesEnabled(s_enabled);
  if (!s_enabled) {
    fprintf(stderr,
            "[audio-ext] extended sound channels disabled "
            "(authentic 8 voices)\n");
    return;
  }
  s_previous_filter = g_apu_spc_dsp_write_filter_hook;
  g_apu_spc_dsp_write_filter_hook = RouteDspWrite;
  s_previous_opcode_patch = g_spc_opcode_patch_hook;
  g_spc_opcode_patch_hook = PatchSpcOpcode;
  s_previous_cycle_hook = g_spc_opcode_cycle_hook;
  g_spc_opcode_cycle_hook = AdjustSpcOpcodeCycles;
  s_previous_extra_saveload = g_apu_extra_saveload_hook;
  g_apu_extra_saveload_hook = SaveLoadExtensionState;
  s_previous_upload_hook = g_rtl_spc_upload_hook;
  g_rtl_spc_upload_hook = OnSpcUpload;
  fprintf(stderr,
          "[audio-ext] extended sound channels enabled "
          "(%d voices, %d effect lanes)\n",
          kDspMaximumVoiceCount, kVirtualVoicePoolSize);
}

bool NativeAudioExtension_IsEnabled(void) {
  return s_enabled;
}
