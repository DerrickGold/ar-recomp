#include "runner_next_internal.h"

/* Focused device tests do not need the complete observer implementation. Keep
 * their link boundary at the device under test instead of accidentally
 * selecting the whole runner object from the static contract archive. */
SrEventMask g_sr_runner_event_mask;

void sr_runner_emit_event(Snes *snes, SrEventMask event_mask,
                          SrRunnerEvent *event) {
    (void)snes;
    (void)event_mask;
    (void)event;
}

void sr_runner_emit_memory_write(Snes *snes, SrMemoryRegion region,
                                 uint32_t offset, uint32_t old_value,
                                 uint32_t new_value, uint32_t width) {
    (void)snes;
    (void)region;
    (void)offset;
    (void)old_value;
    (void)new_value;
    (void)width;
}

void sr_runner_emit_ppu_memory_write(Ppu *ppu, SrMemoryRegion region,
                                     uint32_t address,
                                     uint32_t previous_value,
                                     uint32_t value,
                                     uint32_t width_bytes) {
    (void)ppu;
    (void)region;
    (void)address;
    (void)previous_value;
    (void)value;
    (void)width_bytes;
}

void sr_runner_emit_register_access(Snes *snes, bool write,
                                    uint32_t address, uint32_t value,
                                    uint32_t width) {
    (void)snes;
    (void)write;
    (void)address;
    (void)value;
    (void)width;
}
