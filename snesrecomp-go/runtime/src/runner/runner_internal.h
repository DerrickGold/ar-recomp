#pragma once

/* Private runner implementation contract. Never include from game code. */

#include <stdbool.h>

#include "snesrecomp/game.h"
#include "snesrecomp/runner.h"

typedef struct Snes Snes;
typedef struct Ppu Ppu;
typedef struct Apu Apu;
typedef SrResult SrRunnerPpuObjRasterProvider(
    Snes *snes, const SrPpuObjRasterRequest *request,
    SrPpuObjRasterResult *out_result);
typedef SrResult SrRunnerPpuObjResolveProvider(
    Snes *snes, const SrPpuObjResolveRequest *request,
    SrPpuObjResolveResult *out_result);
typedef SrResult SrRunnerPpuObjPartsRasterProvider(
    Snes *snes, const SrPpuObjPartsRasterRequest *request,
    SrPpuObjRasterResult *out_result);
typedef SrResult SrRunnerPpuScanoutProvider(
    Snes *snes, const SrPpuScanoutRequest *request,
    SrPpuScanoutResult *out_result);

/* Private composition hooks connecting the hardware model, generated-game
 * bridge, and public opaque ABI. None exposes an internal layout publicly. */
SrRunnerHandle *sr_runner_handle(Snes *snes);
void sr_runner_set_cpu_state_provider(
    Snes *snes, RtlGameCpuStateQueryFunc *provider, void *user_data,
    const void *component_handle);
void sr_runner_set_execution_state_provider(
    Snes *snes, RtlGameExecutionStateQueryFunc *provider, void *user_data);
void sr_runner_set_ppu_obj_raster_provider(
    Snes *snes, SrRunnerPpuObjRasterProvider *provider);
void sr_runner_set_ppu_obj_resolve_provider(
    Snes *snes, SrRunnerPpuObjResolveProvider *provider);
void sr_runner_set_ppu_obj_parts_raster_provider(
    Snes *snes, SrRunnerPpuObjPartsRasterProvider *provider);
void sr_runner_set_ppu_scanout_provider(
    Snes *snes, SrRunnerPpuScanoutProvider *provider);
void sr_runner_bind_ppu_services(Snes *snes, bool enabled);
void sr_runner_note_tick(Snes *snes);
void sr_runner_note_reset(Snes *snes);
void sr_runner_note_load(Snes *snes);
void sr_runner_note_mutation(Snes *snes);
void sr_runner_apply_pending_mutations(Snes *snes, uint32_t *inputs,
                                       uint64_t frame_counter);
void sr_runner_clear_mutations(Snes *snes);

/* Hot instrumentation checks this union mask before constructing an event.
 * It is zero when no observer wants the class, making disabled observation a
 * single predictable branch at the existing block/dispatch seams. */
extern SrEventMask g_sr_runner_event_mask;
#if defined(__GNUC__) || defined(__clang__)
static inline bool sr_runner_event_enabled(SrEventMask event_mask) {
    return __builtin_expect(
        (g_sr_runner_event_mask & event_mask) != 0u, 0);
}
#else
static inline bool sr_runner_event_enabled(SrEventMask event_mask) {
    return (g_sr_runner_event_mask & event_mask) != 0u;
}
#endif
void sr_runner_emit_event(Snes *snes, SrEventMask event_mask,
                          SrRunnerEvent *event);
void sr_runner_emit_memory_write(Snes *snes, SrMemoryRegion region,
                                 uint32_t address, uint32_t previous_value,
                                 uint32_t value, uint32_t width_bytes);
void sr_runner_emit_ppu_memory_write(Ppu *ppu, SrMemoryRegion region,
                                     uint32_t address,
                                     uint32_t previous_value,
                                     uint32_t value,
                                     uint32_t width_bytes);
void sr_runner_emit_register_access(Snes *snes, bool write,
                                    uint32_t address, uint32_t value,
                                    uint32_t width_bytes);
void sr_runner_emit_frame_boundary(Snes *snes, uint32_t flags,
                                   const char *label);
void sr_runner_emit_audio_produced(Snes *snes, const int16_t *samples,
                                   uint64_t frame_offset,
                                   uint32_t frame_count,
                                   uint32_t sample_rate,
                                   uint16_t channel_count);
void sr_runner_emit_interrupt(Snes *snes, SrInterruptKind kind,
                              uint32_t flags, uint32_t pc24,
                              uint16_t vector, int32_t scanline,
                              const char *label);
void sr_runner_emit_error(Snes *snes, SrRunnerErrorCode code,
                          uint32_t flags, uint32_t pc24,
                          uint32_t source_pc24, const char *label);
/* Disabled audio observation retains the single predictable branch that the
 * former concrete trace-hook pointer occupied at each existing seam. */
extern uint32_t g_sr_runner_audio_trace_observer_count;
#if defined(__GNUC__) || defined(__clang__)
static inline bool sr_runner_audio_trace_enabled(void) {
    return __builtin_expect(g_sr_runner_audio_trace_observer_count != 0u, 0);
}
#else
static inline bool sr_runner_audio_trace_enabled(void) {
    return g_sr_runner_audio_trace_observer_count != 0u;
}
#endif
void sr_runner_emit_audio_trace(Apu *apu, SrAudioTraceEventType type,
                                uint16_t opcode_pc, uint8_t port,
                                uint8_t dsp_address, uint8_t value,
                                uint64_t cycle_count,
                                uint32_t source_address,
                                uint32_t frame_counter,
                                const char *function_name);
SrResult sr_runner_subscribe_audio_trace(
    SrRunnerHandle *runner,
    const SrAudioTraceSubscription *subscription,
    uint64_t *out_subscription_id);
SrResult sr_runner_unsubscribe_audio_trace(SrRunnerHandle *runner,
                                           uint64_t subscription_id);
void sr_runner_bind_ppu_owner(Snes *snes, Ppu *ppu, bool enabled);
void sr_runner_clear_event_subscriptions(Snes *snes);
void sr_runner_clear_audio_trace_subscriptions(Snes *snes);
SrResult sr_runner_compare_exchange_spc_pc(
    SrRunnerHandle *runner, const SrSpcPcControlRequest *request,
    SrSpcPcControlResult *out_result);
SrResult sr_runner_configure_audio_mix(
    SrRunnerHandle *runner, const SrAudioMixControl *control);
SrResult sr_runner_apply_ppu_frame_policy(
    Snes *snes, const SrPpuFramePolicy *policy);
