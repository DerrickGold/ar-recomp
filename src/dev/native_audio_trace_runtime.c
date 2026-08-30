#include "native_audio_trace.h"

#include <stdio.h>
#include <stdlib.h>

#include "snesrecomp/host/audio_trace.h"
#include "snesrecomp/game/runtime.h"
#include "native_audio_extension.h"
#include "snesrecomp/runner.h"
#include "run_dir.h"

enum {
  kRuntimeRequestCapacity = 32768,
  kRuntimeSongEventCapacity = 2048,
  kRuntimeProvenanceCapacity = 1024,
  kRuntimeSuppressionCapacity = 128,
  kRuntimePathCapacity = 512,
};

extern uint64_t snes_apu_cycle_count(void);

static int s_enabled = -1;
static const SnesRunnerApi *s_runner_api;
static SrRunnerHandle *s_runner;
static uint64_t s_audio_trace_subscription;

static int TraceEnabled(void) {
  if (s_enabled < 0) {
    const char *value = getenv("AR_NATIVE_AUDIO_TRACE");
    s_enabled = value && value[0] && value[0] != '0';
  }
  return s_enabled;
}

static void OnRunnerAudioTrace(void *user_data, SrRunnerHandle *runner,
                               const SrAudioTraceEvent *event) {
  (void)user_data;
  if (runner != s_runner || !event ||
      event->struct_size < SR_AUDIO_TRACE_EVENT_V2_SIZE ||
      !event->apu_ram || event->apu_ram_byte_size < SR_APU_RAM_BYTE_COUNT)
    return;
  switch (event->type) {
    case SR_AUDIO_TRACE_CPU_PORT_WRITE:
      NativeAudioTraceModel_CpuPortWrite(
          event->port, event->value, event->function_name,
          event->frame_counter, event->cycle_count);
      break;
    case SR_AUDIO_TRACE_SPC_UPLOAD:
      NativeAudioTraceModel_SpcUpload(
          event->source_address, event->function_name,
          event->frame_counter, event->cycle_count);
      break;
    case SR_AUDIO_TRACE_APU_PORT_APPLY:
      NativeAudioTraceModel_PortApply(
          event->port, event->value, event->cycle_count);
      break;
    case SR_AUDIO_TRACE_SPC_PORT_READ:
      NativeAudioTraceModel_SpcPortRead(
          event->port, event->value, event->apu_ram[0x35],
          event->cycle_count);
      break;
    case SR_AUDIO_TRACE_SPC_OPCODE:
      NativeAudioTraceModel_SpcOpcode(
          event->spc_pc, event->spc_a, event->spc_x,
          event->apu_ram[0x35], event->apu_input_ports[3],
          event->cycle_count);
      if (!NativeAudioExtension_IsEnabled() &&
          (event->spc_pc == 0x04D0 || event->spc_pc == 0x05B1 ||
           event->spc_pc == 0x080A)) {
        NativeAudioTraceModel_MusicUpdateSuppressed(
            event->spc_pc, event->spc_x, event->apu_ram[0x47],
            event->apu_ram[0x1A], event->cycle_count);
      }
      break;
    case SR_AUDIO_TRACE_DSP_WRITE:
      NativeAudioTraceModel_DspWrite(
          event->struct_size >= SR_AUDIO_TRACE_EVENT_V3_SIZE
              ? event->spc_instruction_pc : event->spc_pc,
          event->spc_x, event->dsp_address, event->value,
          event->apu_ram[0x47], event->apu_ram[0x1A], event->cycle_count);
      break;
    default:
      break;
  }
}

static void OnExtendedDisposition(
    uint64_t serial, uint64_t existing_serial,
    bool coalesced, bool overflow) {
  NativeAudioTraceModel_ExtendedDisposition(
      serial, existing_serial, coalesced, overflow,
      snes_apu_cycle_count());
}

static void OnExtendedStart(
    uint64_t serial, uint8_t lane, uint8_t virtual_voice) {
  NativeAudioTraceModel_ExtendedSequenceStart(
      serial, lane, virtual_voice, snes_apu_cycle_count());
}

static void OnExtendedEnd(uint64_t serial, uint8_t lane) {
  NativeAudioTraceModel_ExtendedSequenceEnd(
      serial, lane, snes_apu_cycle_count());
}

static void OnExtendedCancel(uint64_t serial) {
  NativeAudioTraceModel_ExtendedCancel(serial, snes_apu_cycle_count());
}

bool NativeAudioTrace_Init(SrRunnerHandle *runner) {
  SrAudioTraceSubscription subscription = {
    .struct_size = sizeof(subscription),
    .callback = OnRunnerAudioTrace,
    .event_mask = SR_AUDIO_TRACE_MASK_ALL,
  };
  {
    const char *pcm = getenv("AR_NATIVE_AUDIO_PCM");
    if (pcm && pcm[0] && pcm[0] != '0') audio_trace_set_enabled(1);
  }
  if (!TraceEnabled()) return true;
  NativeAudioTrace_Shutdown();
  NativeAudioTraceModel_Reset();
  s_runner_api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
  s_runner = runner;
  if (!s_runner_api || !s_runner ||
      s_runner_api->struct_size < SNES_RUNNER_API_AUDIO_TRACE_OBSERVER_SIZE ||
      !(s_runner_api->capabilities & SR_RUNNER_CAP_AUDIO_TRACE_OBSERVERS) ||
      s_runner_api->subscribe_audio_trace(
          s_runner, &subscription, &s_audio_trace_subscription) !=
          SR_RESULT_OK) {
    s_runner_api = NULL;
    s_runner = NULL;
    s_audio_trace_subscription = 0;
    return false;
  }
  g_native_audio_extension_trace_disposition_hook = OnExtendedDisposition;
  g_native_audio_extension_trace_start_hook = OnExtendedStart;
  g_native_audio_extension_trace_end_hook = OnExtendedEnd;
  g_native_audio_extension_trace_cancel_hook = OnExtendedCancel;
  fprintf(stderr,
          "[native-audio-trace] enabled — serial request/lane provenance "
          "will be written at shutdown\n");
  return true;
}

void NativeAudioTrace_Shutdown(void) {
  if (s_runner_api && s_runner && s_audio_trace_subscription) {
    s_runner_api->unsubscribe_audio_trace(
        s_runner, s_audio_trace_subscription);
  }
  s_audio_trace_subscription = 0;
  s_runner = NULL;
  s_runner_api = NULL;
  if (g_native_audio_extension_trace_disposition_hook == OnExtendedDisposition)
    g_native_audio_extension_trace_disposition_hook = NULL;
  if (g_native_audio_extension_trace_start_hook == OnExtendedStart)
    g_native_audio_extension_trace_start_hook = NULL;
  if (g_native_audio_extension_trace_end_hook == OnExtendedEnd)
    g_native_audio_extension_trace_end_hook = NULL;
  if (g_native_audio_extension_trace_cancel_hook == OnExtendedCancel)
    g_native_audio_extension_trace_cancel_hook = NULL;
}

uint64_t NativeAudioTrace_OnCpuRequest(
    NativeAudioRequestKind kind, uint8_t id, int emitted,
    const char *caller, uint32_t site, uint32_t game_frame,
    uint16_t cpu_x, uint16_t cpu_y) {
  if (!TraceEnabled()) return 0;
  RtlApuLock();
  const uint64_t serial = emitted && NativeAudioExtension_IsEnabled()
      ? NativeAudioTraceModel_PostExtendedRequest(
            kind, id, caller, site, game_frame, cpu_x, cpu_y,
            snes_apu_cycle_count())
      : NativeAudioTraceModel_PostRequest(
            kind, id, emitted, caller, site, game_frame, cpu_x, cpu_y,
            snes_apu_cycle_count());
  RtlApuUnlock();
  return serial;
}

static const char *FinalOutcome(const NativeAudioRequestRecord *r) {
  if (r->outcome != kNativeAudioOutcome_Pending)
    return NativeAudioTrace_OutcomeName(
        (NativeAudioRequestOutcome)r->outcome);
  return r->active_lanes ? "active_at_shutdown" : "pending_at_shutdown";
}

static void WriteRequests(const NativeAudioRequestRecord *records,
                          size_t count) {
  char path[kRuntimePathCapacity];
  RunDirFile(path, sizeof(path), "native_audio_requests.csv");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
          "serial,kind,id,effective_id,frame,site,caller,outcome,flags,"
          "lanes_started,active_lanes,virtual_voices_started,"
          "posted_cycle,port_write_cycle,"
          "port_apply_cycle,spc_read_cycle,start_cycle,end_cycle,"
          "replaced_by,retrigger_first_cycle,retrigger_last_cycle,"
          "cpu_x,cpu_y,music_updates_suppressed,"
          "music_suppressed_voice_mask,native_lane_retriggers\n");
  for (size_t i = 0; i < count; i++) {
    const NativeAudioRequestRecord *r = &records[i];
    fprintf(f,
            "%llu,%s,%02x,%02x,%u,%06x,\"%s\",%s,%04x,%02x,%02x,%08x,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%04x,%04x,%u,%02x,%u\n",
            (unsigned long long)r->serial,
            r->kind == kNativeAudioRequest_Event ? "COP" : "BRK",
            r->id, r->effective_id, r->game_frame, r->site,
            r->caller ? r->caller : "?", FinalOutcome(r), r->flags,
            r->lanes_started, r->active_lanes,
            r->virtual_voices_started,
            (unsigned long long)r->posted_cycle,
            (unsigned long long)r->port_write_cycle,
            (unsigned long long)r->port_apply_cycle,
            (unsigned long long)r->spc_read_cycle,
            (unsigned long long)r->sequence_start_cycle,
            (unsigned long long)r->sequence_end_cycle,
            (unsigned long long)r->replaced_by_serial,
            (unsigned long long)r->native_retrigger_first_cycle,
            (unsigned long long)r->native_retrigger_last_cycle,
            r->cpu_x, r->cpu_y, r->music_updates_suppressed,
            r->music_suppressed_voice_mask, r->native_lane_retriggers);
  }
  fclose(f);
  fprintf(stderr, "[native-audio-trace] wrote %s\n", path);
}

static void WritePcmIfRequested(void) {
  const char *enabled = getenv("AR_NATIVE_AUDIO_PCM");
  if (!enabled || !enabled[0] || enabled[0] == '0') return;
  char path[kRuntimePathCapacity];
  RunDirFile(path, sizeof(path), "native_audio_pcm.wav");
  uint64_t start = 0, count = 0;
  if (audio_trace_dump_wav(path, -1, 0, &start, &count) == 0) {
    fprintf(stderr,
            "[native-audio-trace] wrote %s (samples %llu..%llu)\n",
            path, (unsigned long long)start,
            (unsigned long long)(start + count));
  } else {
    fprintf(stderr, "[native-audio-trace] could not write %s\n", path);
  }
}

static void WriteSongEvents(const NativeAudioSongEvent *events, size_t count) {
  char path[kRuntimePathCapacity];
  RunDirFile(path, sizeof(path), "native_audio_song_events.csv");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "cycle,frame,event,value,image_src,caller\n");
  for (size_t i = 0; i < count; i++) {
    const NativeAudioSongEvent *e = &events[i];
    fprintf(f, "%llu,%u,%s,%02x,%06x,\"%s\"\n",
            (unsigned long long)e->cycle, e->game_frame,
            NativeAudioTrace_SongEventName(
                (NativeAudioSongEventKind)e->kind),
            e->value, e->image_src, e->caller ? e->caller : "?");
  }
  fclose(f);
  fprintf(stderr, "[native-audio-trace] wrote %s\n", path);
}

static void WriteProvenance(const NativeAudioDspProvenance *entries,
                            size_t count) {
  char path[kRuntimePathCapacity];
  RunDirFile(path, sizeof(path), "native_audio_dsp_provenance.csv");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
          "spc_pc,spc_x,dsp_addr,track_mask,ownership_mask,writes,"
          "first_cycle,last_cycle,last_value\n");
  for (size_t i = 0; i < count; i++) {
    const NativeAudioDspProvenance *p = &entries[i];
    fprintf(f, "%04x,%02x,%02x,%02x,%02x,%llu,%llu,%llu,%02x\n",
            p->spc_pc, p->spc_x, p->dsp_addr, p->track_mask,
            p->ownership_mask, (unsigned long long)p->writes,
            (unsigned long long)p->first_cycle,
            (unsigned long long)p->last_cycle, p->last_value);
  }
  fclose(f);
  fprintf(stderr, "[native-audio-trace] wrote %s\n", path);
}

static void WriteMusicSuppressions(
    const NativeAudioMusicSuppression *entries, size_t count) {
  char path[kRuntimePathCapacity];
  RunDirFile(path, sizeof(path), "native_audio_music_suppression.csv");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
          "spc_pc,spc_x,track_mask,ownership_mask,occurrences,"
          "unattributed,first_cycle,last_cycle\n");
  for (size_t i = 0; i < count; i++) {
    const NativeAudioMusicSuppression *entry = &entries[i];
    fprintf(f, "%04x,%02x,%02x,%02x,%llu,%llu,%llu,%llu\n",
            entry->spc_pc, entry->spc_x, entry->track_mask,
            entry->ownership_mask,
            (unsigned long long)entry->occurrences,
            (unsigned long long)entry->unattributed,
            (unsigned long long)entry->first_cycle,
            (unsigned long long)entry->last_cycle);
  }
  fclose(f);
  fprintf(stderr, "[native-audio-trace] wrote %s\n", path);
}

void NativeAudioTrace_Report(void) {
  if (!TraceEnabled()) return;

  NativeAudioRequestRecord *requests =
      malloc(kRuntimeRequestCapacity * sizeof(*requests));
  NativeAudioSongEvent *songs =
      malloc(kRuntimeSongEventCapacity * sizeof(*songs));
  NativeAudioDspProvenance *provenance =
      malloc(kRuntimeProvenanceCapacity * sizeof(*provenance));
  NativeAudioMusicSuppression *suppressions =
      malloc(kRuntimeSuppressionCapacity * sizeof(*suppressions));
  if (!requests || !songs || !provenance || !suppressions) {
    fprintf(stderr, "[native-audio-trace] report allocation failed\n");
    free(requests);
    free(songs);
    free(provenance);
    free(suppressions);
    return;
  }

  NativeAudioTraceStats stats;
  RtlApuLock();
  size_t request_count = NativeAudioTraceModel_CopyRequests(
      requests, kRuntimeRequestCapacity);
  size_t song_count = NativeAudioTraceModel_CopySongEvents(
      songs, kRuntimeSongEventCapacity);
  size_t provenance_count = NativeAudioTraceModel_CopyDspProvenance(
      provenance, kRuntimeProvenanceCapacity);
  size_t suppression_count = NativeAudioTraceModel_CopyMusicSuppressions(
      suppressions, kRuntimeSuppressionCapacity);
  NativeAudioTraceModel_GetStats(&stats);
  RtlApuUnlock();

  WriteRequests(requests, request_count);
  WriteSongEvents(songs, song_count);
  WriteProvenance(provenance, provenance_count);
  WriteMusicSuppressions(suppressions, suppression_count);
  WritePcmIfRequested();
  fprintf(stderr,
          "[native-audio-trace] requests=%llu retained=%llu "
          "completed=%llu mailbox-coalesced=%llu mailbox-drop=%llu "
          "port-coalesced=%llu port-drop=%llu busy-drop=%llu "
          "lane-retriggered=%llu lane-replaced=%llu "
          "song-cancelled=%llu suppressed=%llu "
          "native-retriggers=%llu "
          "extended-coalesced=%llu extended-overflow=%llu "
          "pending=%llu dsp-writes=%llu "
          "music-updates-suppressed=%llu unattributed=%llu\n",
          (unsigned long long)stats.requests,
          (unsigned long long)stats.retained_requests,
          (unsigned long long)stats.outcome[kNativeAudioOutcome_Completed],
          (unsigned long long)stats.outcome[
              kNativeAudioOutcome_CoalescedMailboxDuplicate],
          (unsigned long long)stats.outcome[kNativeAudioOutcome_OverwrittenMailbox],
          (unsigned long long)stats.outcome[
              kNativeAudioOutcome_CoalescedPortDuplicate],
          (unsigned long long)stats.outcome[kNativeAudioOutcome_OverwrittenPort],
          (unsigned long long)stats.outcome[kNativeAudioOutcome_RejectedDualBusy],
          (unsigned long long)stats.outcome[
              kNativeAudioOutcome_RetriggeredLane],
          (unsigned long long)stats.outcome[kNativeAudioOutcome_ReplacedLane],
          (unsigned long long)stats.outcome[
              kNativeAudioOutcome_CanceledSongTransition],
          (unsigned long long)stats.outcome[kNativeAudioOutcome_SuppressedSetting],
          (unsigned long long)stats.native_lane_retriggers,
          (unsigned long long)stats.outcome[
              kNativeAudioOutcome_CoalescedExtendedDuplicate],
          (unsigned long long)stats.outcome[
              kNativeAudioOutcome_ExtendedFifoOverflow],
          (unsigned long long)stats.outcome[kNativeAudioOutcome_Pending],
          (unsigned long long)stats.dsp_writes,
          (unsigned long long)stats.music_updates_suppressed,
          (unsigned long long)stats.music_suppressions_unattributed);
  if (stats.request_records_evicted || stats.scheduled_port_overflow ||
      stats.song_event_overflow || stats.provenance_overflow ||
      stats.suppression_provenance_overflow) {
    fprintf(stderr,
            "[native-audio-trace] WARNING incomplete retention: "
            "requests-evicted=%llu scheduled-overflow=%llu "
            "song-overflow=%llu dsp-provenance-overflow=%llu "
            "suppression-provenance-overflow=%llu\n",
            (unsigned long long)stats.request_records_evicted,
            (unsigned long long)stats.scheduled_port_overflow,
            (unsigned long long)stats.song_event_overflow,
            (unsigned long long)stats.provenance_overflow,
            (unsigned long long)stats.suppression_provenance_overflow);
  }

  free(requests);
  free(songs);
  free(provenance);
  free(suppressions);
}
