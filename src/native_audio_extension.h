#ifndef AR_NATIVE_AUDIO_EXTENSION_H
#define AR_NATIVE_AUDIO_EXTENSION_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "snesrecomp/game_audio.h"

/* Pure routing helpers for ActRaiser's two effect sequencer tracks. */
bool NativeAudioExtension_RouteVoiceWrite(
    uint8_t dsp_addr, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask, int *hardware_voice, int *virtual_voice);
uint8_t NativeAudioExtension_RoutedGlobalMask(
    uint8_t logical_track, uint8_t track_mask, uint8_t ownership_mask);
bool NativeAudioExtension_ShouldBypassMusicSuppression(
    uint16_t spc_pc, uint8_t logical_track, uint8_t track_mask,
    uint8_t ownership_mask);

/* Policies apply only to the same nonzero source key AND effect ID/kind.
 * ReplaceSelf waits for the old KOF release; RestartSelf immediately resets
 * the sequence at the next driver tick. LatestPending keeps only one successor.
 * None can reserve another effect's destination or block another emitter. */
typedef enum NativeAudioOverlapPolicy {
  kNativeAudio_Independent = 0,
  kNativeAudio_BlockSelf,
  kNativeAudio_RestartSelf,
  kNativeAudio_ReplaceSelf,
  kNativeAudio_LatestPending,
} NativeAudioOverlapPolicy;

typedef enum NativeAudioDisposition {
  kNativeAudioDisposition_BlockedSelf = 0,
  kNativeAudioDisposition_RestartedSelf,
  kNativeAudioDisposition_ReplacedSelf,
  kNativeAudioDisposition_ReplacedPending,
  kNativeAudioDisposition_CapacityDrop,
  kNativeAudioDisposition_SequenceUnavailable,
} NativeAudioDisposition;

typedef struct NativeAudioRequest {
  uint64_t source_key; /* stable emitter lifetime; zero means unclassified */
  uint64_t trace_serial;
  uint32_t caller_pc;
  uint32_t game_frame; /* diagnostic only, never part of emitter identity */
  uint16_t actor_x, actor_y; /* diagnostic CPU registers, not world coordinates */
  uint8_t id, event_request, policy;
} NativeAudioRequest;

/* Game-thread-only WRAM capture. Tick observations and QueueGameRequest retain
 * full lifecycle observation; WRAM reads occur outside the audio mutex. */
void NativeAudioExtension_ObserveGameState(const uint8_t *wram, size_t size);
bool NativeAudioExtension_QueueGameRequest(
    const uint8_t *wram, size_t size, bool event_request, uint8_t id,
    uint32_t caller_pc, uint32_t game_frame, uint16_t actor_x, uint16_t actor_y,
    uint64_t trace_serial);
bool NativeAudioExtension_QueueIdentifiedRequest(const NativeAudioRequest *request);
extern void (*g_native_audio_extension_trace_policy_hook)(
    uint64_t trace_serial, uint64_t other_serial, NativeAudioDisposition disposition);

/* Capture a game-side BRK/COP sound request before the native depth-one WRAM
 * mailbox can overwrite it. Returns true only when extended mode owns the
 * request and the caller should skip the original mailbox write. Exact repeats
 * from the same producer/actor in one game frame are coalesced deliberately. */
bool NativeAudioExtension_QueueRequest(
    bool event_request, uint8_t id, uint32_t caller_pc,
    uint32_t game_frame, uint16_t actor_x, uint16_t actor_y,
    uint64_t trace_serial);
int NativeAudioExtension_QueuedRequestCount(void);
int NativeAudioExtension_ActiveInstanceCount(void);

/* Optional diagnostic observers. They never influence allocation and remain
 * NULL unless the native-audio trace is enabled. `lane` is 0 for X=$10 and 1
 * for X=$12. */
extern void (*g_native_audio_extension_trace_disposition_hook)(
    uint64_t trace_serial, uint64_t existing_trace_serial,
    bool coalesced, bool overflow);
extern void (*g_native_audio_extension_trace_start_hook)(
    uint64_t trace_serial, uint8_t lane, uint8_t virtual_voice);
extern void (*g_native_audio_extension_trace_end_hook)(
    uint64_t trace_serial, uint8_t lane);
extern void (*g_native_audio_extension_trace_cancel_hook)(
    uint64_t trace_serial);

/* Fixed-width game-adapter callbacks. The runner invokes these synchronously
 * while it owns the live audio state; no concrete APU/SPC/DSP layout crosses
 * this boundary. */
bool NativeAudioExtension_FilterDspWrite(
    RtlAudioExtensionContext *context, uint8_t address, uint8_t *value);
void NativeAudioExtension_PatchSpcOpcode(
    RtlAudioExtensionContext *context, uint16_t opcode_pc);
int NativeAudioExtension_AdjustSpcOpcodeCycles(
    uint16_t opcode_pc, int cycles);
void NativeAudioExtension_SaveState(RtlAudioSaveContext *context);
void NativeAudioExtension_OnSpcUpload(
    RtlAudioExtensionContext *context, uint32_t source24);

/* Install the restart-class optional bridge. Safe before SnesInit; the DSP
 * core stores enablement globally and initializes its virtual banks on reset. */
void NativeAudioExtension_Install(void);
bool NativeAudioExtension_IsEnabled(void);

#endif
