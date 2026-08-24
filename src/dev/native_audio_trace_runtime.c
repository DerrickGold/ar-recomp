#include "native_audio_trace.h"

#include <stdio.h>
#include <stdlib.h>

#include "common_rtl.h"
#include "native_audio_extension.h"
#include "run_dir.h"
#include "snes/apu.h"
#include "snes/spc.h"

enum {
  kRuntimeRequestCapacity = 32768,
  kRuntimeSongEventCapacity = 2048,
  kRuntimeProvenanceCapacity = 1024,
  kRuntimeSuppressionCapacity = 128,
  kRuntimePathCapacity = 512,
};

extern const char *g_last_recomp_func;
extern int snes_frame_counter;
extern uint64_t snes_apu_cycle_count(void);

static int s_enabled = -1;
static uint16_t s_current_spc_pc;

static int TraceEnabled(void) {
  if (s_enabled < 0) {
    const char *value = getenv("AR_NATIVE_AUDIO_TRACE");
    s_enabled = value && value[0] && value[0] != '0';
  }
  return s_enabled;
}

static void OnCpuPortWrite(uint8_t port, uint8_t value) {
  NativeAudioTraceModel_CpuPortWrite(
      port, value, g_last_recomp_func, (uint32_t)snes_frame_counter,
      snes_apu_cycle_count());
}

static void OnPortApply(Apu *apu, uint8_t port, uint8_t value) {
  (void)apu;
  NativeAudioTraceModel_PortApply(port, value, snes_apu_cycle_count());
}

static void OnSpcPortRead(Apu *apu, uint8_t port, uint8_t value) {
  NativeAudioTraceModel_SpcPortRead(
      port, value, apu->ram[0x35], snes_apu_cycle_count());
}

static void OnSpcOpcode(Spc *spc, uint16_t pc) {
  s_current_spc_pc = pc;
  NativeAudioTraceModel_SpcOpcode(
      pc, spc->a, spc->x, spc->apu->ram[0x35], spc->apu->inPorts[3],
      snes_apu_cycle_count());
  if (!NativeAudioExtension_IsEnabled() &&
      (pc == 0x04D0 || pc == 0x05B1 || pc == 0x080A)) {
    NativeAudioTraceModel_MusicUpdateSuppressed(
        pc, spc->x, spc->apu->ram[0x47], spc->apu->ram[0x1A],
        snes_apu_cycle_count());
  }
}

static void OnSpcDspWrite(Apu *apu, uint8_t addr, uint8_t value) {
  NativeAudioTraceModel_DspWrite(
      s_current_spc_pc, apu->spc->x, addr, value,
      apu->ram[0x47], apu->ram[0x1A], snes_apu_cycle_count());
}

static void OnSpcUpload(uint32_t image_src) {
  /* The upload observer runs just after the upload routine releases the APU
   * lock, unlike port/SPC observers which run inside it. Reacquire here so a
   * live audio callback cannot mutate the trace model concurrently. */
  RtlApuLock();
  NativeAudioTraceModel_SpcUpload(
      image_src, g_last_recomp_func, (uint32_t)snes_frame_counter,
      snes_apu_cycle_count());
  RtlApuUnlock();
}

void NativeAudioTrace_Init(void) {
  if (!TraceEnabled()) return;
  NativeAudioTraceModel_Reset();
  g_rtl_apu_port_trace_hook = OnCpuPortWrite;
  g_rtl_spc_upload_trace_hook = OnSpcUpload;
  g_apu_port_apply_trace_hook = OnPortApply;
  g_apu_spc_port_read_trace_hook = OnSpcPortRead;
  g_apu_spc_dsp_write_trace_hook = OnSpcDspWrite;
  g_spc_opcode_trace_hook = OnSpcOpcode;
  fprintf(stderr,
          "[native-audio-trace] enabled — serial request/lane provenance "
          "will be written at shutdown\n");
}

void NativeAudioTrace_OnCpuRequest(
    NativeAudioRequestKind kind, uint8_t id, int emitted,
    const char *caller, uint32_t site, uint32_t game_frame,
    uint16_t cpu_x, uint16_t cpu_y) {
  if (!TraceEnabled()) return;
  RtlApuLock();
  NativeAudioTraceModel_PostRequest(
      kind, id, emitted, caller, site, game_frame, cpu_x, cpu_y,
      snes_apu_cycle_count());
  RtlApuUnlock();
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
          "lanes_started,active_lanes,posted_cycle,port_write_cycle,"
          "port_apply_cycle,spc_read_cycle,start_cycle,end_cycle,"
          "replaced_by,cpu_x,cpu_y,music_updates_suppressed,"
          "music_suppressed_voice_mask\n");
  for (size_t i = 0; i < count; i++) {
    const NativeAudioRequestRecord *r = &records[i];
    fprintf(f,
            "%llu,%s,%02x,%02x,%u,%06x,\"%s\",%s,%02x,%02x,%02x,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%04x,%04x,%u,%02x\n",
            (unsigned long long)r->serial,
            r->kind == kNativeAudioRequest_Event ? "COP" : "BRK",
            r->id, r->effective_id, r->game_frame, r->site,
            r->caller ? r->caller : "?", FinalOutcome(r), r->flags,
            r->lanes_started, r->active_lanes,
            (unsigned long long)r->posted_cycle,
            (unsigned long long)r->port_write_cycle,
            (unsigned long long)r->port_apply_cycle,
            (unsigned long long)r->spc_read_cycle,
            (unsigned long long)r->sequence_start_cycle,
            (unsigned long long)r->sequence_end_cycle,
            (unsigned long long)r->replaced_by_serial,
            r->cpu_x, r->cpu_y, r->music_updates_suppressed,
            r->music_suppressed_voice_mask);
  }
  fclose(f);
  fprintf(stderr, "[native-audio-trace] wrote %s\n", path);
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
  fprintf(stderr,
          "[native-audio-trace] requests=%llu retained=%llu "
          "completed=%llu mailbox-coalesced=%llu mailbox-drop=%llu "
          "port-coalesced=%llu port-drop=%llu busy-drop=%llu "
          "lane-replaced=%llu song-cancelled=%llu suppressed=%llu "
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
          (unsigned long long)stats.outcome[kNativeAudioOutcome_ReplacedLane],
          (unsigned long long)stats.outcome[
              kNativeAudioOutcome_CanceledSongTransition],
          (unsigned long long)stats.outcome[kNativeAudioOutcome_SuppressedSetting],
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
