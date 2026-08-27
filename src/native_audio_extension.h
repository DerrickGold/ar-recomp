#ifndef AR_NATIVE_AUDIO_EXTENSION_H
#define AR_NATIVE_AUDIO_EXTENSION_H

#include <stdbool.h>
#include <stdint.h>

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
 * core stores enablement globally and initializes its virtual pool on reset. */
void NativeAudioExtension_Install(void);
bool NativeAudioExtension_IsEnabled(void);

#endif
