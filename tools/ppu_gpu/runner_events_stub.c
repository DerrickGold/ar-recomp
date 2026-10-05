/* Standalone PPU probes have no runner or memory-event subscribers. Windows
 * linkers still resolve these symbols in unused PPU register-write paths. */
#include <stdlib.h>
#include "snesrecomp/runner/events.h"
#include "snes/ppu.h"

SrEventMask g_sr_runner_event_mask;

void sr_runner_emit_ppu_memory_write(Ppu *ppu, SrMemoryRegion region,
                                    uint32_t address, uint32_t previous_value,
                                    uint32_t value, uint32_t width_bytes) {
  (void)ppu;
  (void)region;
  (void)address;
  (void)previous_value;
  (void)value;
  (void)width_bytes;
  abort(); /* A probe must never request events without a runner. */
}
