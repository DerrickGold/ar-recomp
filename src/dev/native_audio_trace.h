#ifndef AR_NATIVE_AUDIO_TRACE_H
#define AR_NATIVE_AUDIO_TRACE_H

#include <stddef.h>
#include <stdint.h>

/* Behavior-neutral provenance trace for ActRaiser's native audio requests.
 *
 * The model API is deliberately independent of the emulator. Unit tests feed
 * it the same observations that native_audio_trace_runtime.c receives from
 * the CPU/APU/SPC hooks. The runtime wrapper is opt-in through
 * AR_NATIVE_AUDIO_TRACE=1 and never changes a request or DSP write. */

typedef enum NativeAudioRequestKind {
  kNativeAudioRequest_Event = 2, /* COP -> $035A -> port 2 -> track $10 */
  kNativeAudioRequest_Sfx = 3,   /* BRK -> $035B -> port 3 -> track $12 */
} NativeAudioRequestKind;

typedef enum NativeAudioRequestOutcome {
  kNativeAudioOutcome_Pending = 0,
  kNativeAudioOutcome_SuppressedSetting,
  kNativeAudioOutcome_CoalescedMailboxDuplicate,
  kNativeAudioOutcome_OverwrittenMailbox,
  kNativeAudioOutcome_CoalescedPortDuplicate,
  kNativeAudioOutcome_OverwrittenPort,
  kNativeAudioOutcome_RejectedDualBusy,
  kNativeAudioOutcome_ReplacedLane,
  kNativeAudioOutcome_CanceledSongTransition,
  kNativeAudioOutcome_CoalescedExtendedDuplicate,
  kNativeAudioOutcome_ExtendedFifoOverflow,
  kNativeAudioOutcome_Completed,
  kNativeAudioOutcome_Count,
} NativeAudioRequestOutcome;

enum NativeAudioRequestFlags {
  kNativeAudioFlag_MailboxPosted = 1u << 0,
  kNativeAudioFlag_PortWritten = 1u << 1,
  kNativeAudioFlag_PortApplied = 1u << 2,
  kNativeAudioFlag_SpcRead = 1u << 3,
  kNativeAudioFlag_SequenceStarted = 1u << 4,
  kNativeAudioFlag_DualBusySeen = 1u << 5,
  kNativeAudioFlag_LaneReplaced = 1u << 6,
  kNativeAudioFlag_ExtendedTransport = 1u << 7,
};

typedef struct NativeAudioRequestRecord {
  uint64_t serial;
  uint64_t posted_cycle;
  uint64_t port_write_cycle;
  uint64_t port_apply_cycle;
  uint64_t spc_read_cycle;
  uint64_t sequence_start_cycle;
  uint64_t sequence_end_cycle;
  uint64_t replaced_by_serial;
  const char *caller;
  uint32_t site;
  uint32_t game_frame;
  uint16_t cpu_x;
  uint16_t cpu_y;
  uint8_t kind;
  uint8_t id;
  uint8_t effective_id;
  uint8_t outcome;
  uint16_t flags;
  uint8_t lanes_started; /* bit 0 = X $10, bit 1 = X $12 */
  uint8_t active_lanes;
  uint16_t virtual_voices_started; /* bit 0 = virtual voice 8 */
  uint8_t music_suppressed_voice_mask;
  uint32_t music_updates_suppressed;
} NativeAudioRequestRecord;

typedef enum NativeAudioSongEventKind {
  kNativeAudioSong_Idle = 0,
  kNativeAudioSong_Halt,
  kNativeAudioSong_Prepare,
  kNativeAudioSong_Pause,
  kNativeAudioSong_Uploader,
  kNativeAudioSong_Play,
  kNativeAudioSong_ImageUploaded,
} NativeAudioSongEventKind;

typedef struct NativeAudioSongEvent {
  uint64_t cycle;
  const char *caller;
  uint32_t game_frame;
  uint32_t image_src;
  uint8_t kind;
  uint8_t value;
} NativeAudioSongEvent;

typedef struct NativeAudioDspProvenance {
  uint64_t first_cycle;
  uint64_t last_cycle;
  uint64_t writes;
  uint16_t spc_pc;
  uint8_t spc_x;
  uint8_t dsp_addr;
  uint8_t track_mask;
  uint8_t ownership_mask;
  uint8_t last_value;
} NativeAudioDspProvenance;

typedef struct NativeAudioMusicSuppression {
  uint64_t first_cycle;
  uint64_t last_cycle;
  uint64_t occurrences;
  uint64_t unattributed;
  uint16_t spc_pc;
  uint8_t spc_x;
  uint8_t track_mask;
  uint8_t ownership_mask;
} NativeAudioMusicSuppression;

typedef struct NativeAudioTraceStats {
  uint64_t requests;
  uint64_t retained_requests;
  uint64_t outcome[kNativeAudioOutcome_Count];
  uint64_t song_events;
  uint64_t dsp_writes;
  uint64_t music_updates_suppressed;
  uint64_t music_suppressions_unattributed;
  uint64_t request_records_evicted;
  uint64_t scheduled_port_overflow;
  uint64_t song_event_overflow;
  uint64_t provenance_overflow;
  uint64_t suppression_provenance_overflow;
} NativeAudioTraceStats;

/* Pure model API. Call Reset before feeding observations. */
void NativeAudioTraceModel_Reset(void);
uint64_t NativeAudioTraceModel_PostRequest(
    NativeAudioRequestKind kind, uint8_t id, int emitted,
    const char *caller, uint32_t site, uint32_t game_frame,
    uint16_t cpu_x, uint16_t cpu_y, uint64_t cycle);
uint64_t NativeAudioTraceModel_PostExtendedRequest(
    NativeAudioRequestKind kind, uint8_t id, const char *caller,
    uint32_t site, uint32_t game_frame, uint16_t cpu_x,
    uint16_t cpu_y, uint64_t cycle);
void NativeAudioTraceModel_ExtendedDisposition(
    uint64_t serial, uint64_t existing_serial,
    int coalesced, int overflow, uint64_t cycle);
void NativeAudioTraceModel_ExtendedSequenceStart(
    uint64_t serial, uint8_t lane, uint8_t virtual_voice, uint64_t cycle);
void NativeAudioTraceModel_ExtendedSequenceEnd(
    uint64_t serial, uint8_t lane, uint64_t cycle);
void NativeAudioTraceModel_ExtendedCancel(
    uint64_t serial, uint64_t cycle);
void NativeAudioTraceModel_CpuPortWrite(
    uint8_t port, uint8_t value, const char *caller,
    uint32_t game_frame, uint64_t cycle);
void NativeAudioTraceModel_PortApply(
    uint8_t port, uint8_t value, uint64_t cycle);
void NativeAudioTraceModel_SpcPortRead(
    uint8_t port, uint8_t value, uint8_t dual_busy, uint64_t cycle);
void NativeAudioTraceModel_SpcOpcode(
    uint16_t pc, uint8_t a, uint8_t x, uint8_t dual_busy,
    uint8_t port3_value, uint64_t cycle);
void NativeAudioTraceModel_SpcUpload(
    uint32_t image_src, const char *caller,
    uint32_t game_frame, uint64_t cycle);
void NativeAudioTraceModel_DspWrite(
    uint16_t spc_pc, uint8_t spc_x, uint8_t dsp_addr, uint8_t value,
    uint8_t track_mask, uint8_t ownership_mask, uint64_t cycle);
void NativeAudioTraceModel_MusicUpdateSuppressed(
    uint16_t spc_pc, uint8_t spc_x, uint8_t track_mask,
    uint8_t ownership_mask, uint64_t cycle);

const NativeAudioRequestRecord *NativeAudioTraceModel_FindRequest(
    uint64_t serial);
size_t NativeAudioTraceModel_CopyRequests(
    NativeAudioRequestRecord *out, size_t capacity);
size_t NativeAudioTraceModel_CopySongEvents(
    NativeAudioSongEvent *out, size_t capacity);
size_t NativeAudioTraceModel_CopyDspProvenance(
    NativeAudioDspProvenance *out, size_t capacity);
size_t NativeAudioTraceModel_CopyMusicSuppressions(
    NativeAudioMusicSuppression *out, size_t capacity);
void NativeAudioTraceModel_GetStats(NativeAudioTraceStats *out);
const char *NativeAudioTrace_OutcomeName(NativeAudioRequestOutcome outcome);
const char *NativeAudioTrace_SongEventName(NativeAudioSongEventKind kind);

/* Runtime wrapper. Safe no-ops unless AR_NATIVE_AUDIO_TRACE is enabled. */
void NativeAudioTrace_Init(void);
void NativeAudioTrace_Report(void);
uint64_t NativeAudioTrace_OnCpuRequest(
    NativeAudioRequestKind kind, uint8_t id, int emitted,
    const char *caller, uint32_t site, uint32_t game_frame,
    uint16_t cpu_x, uint16_t cpu_y);

#endif /* AR_NATIVE_AUDIO_TRACE_H */
