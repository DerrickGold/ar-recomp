#pragma once

#include <stdbool.h>

#include "runner_next.h"

typedef struct Snes Snes;
typedef struct Ppu Ppu;
typedef SrResult SrRunnerCpuStateProvider(
    Snes *snes, SrCpuStateSnapshot *out_state);
typedef SrResult SrRunnerExecutionStateProvider(
    Snes *snes, SrExecutionSnapshot *out_state);
typedef SrResult SrRunnerPpuObjRasterProvider(
    Snes *snes, const SrPpuObjRasterRequest *request,
    SrPpuObjRasterResult *out_result);

/* Temporary bridge for in-tree consumers while compatibility globals are
 * migrated. None of these functions exposes an internal layout through the
 * public ABI. */
SrRunnerHandle *sr_runner_handle(Snes *snes);
void sr_runner_set_cpu_state_provider(
    Snes *snes, SrRunnerCpuStateProvider *provider,
    const void *component_handle);
void sr_runner_set_execution_state_provider(
    Snes *snes, SrRunnerExecutionStateProvider *provider);
void sr_runner_set_ppu_obj_raster_provider(
    Snes *snes, SrRunnerPpuObjRasterProvider *provider);
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
void sr_runner_bind_ppu_owner(Snes *snes, Ppu *ppu, bool enabled);
void sr_runner_clear_event_subscriptions(Snes *snes);
