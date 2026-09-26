#ifndef AR_SIM_WORLD_NAVIGATION_PALACE_H
#define AR_SIM_WORLD_NAVIGATION_PALACE_H
/* SimWorldNavigationPalace: separates the Sky Palace's native foreground from
 * its sky using the PPU's BG1-winner mask so the enhanced sky can show
 * through, and reports PPU states where that separation is unsafe.
 * Phase: pure.
 * Tests: tests/sim_world_navigation_palace_test.c */

#include <stdbool.h>
#include <stdint.h>
#include "snesrecomp/runner.h"

enum {
  kSimWorldNavigationPalaceMaxWidth = 512,
  kSimWorldNavigationPalaceMaxHeight = 240,
};

/* Observational BG1 winner capture is usable only when the original sky is
 * not an input to another layer's color math. Unsupported states retain the
 * authentic composition. No PPU state or capture binding is changed here. */
bool SimWorldNavigationPalace_PpuSupported(const SrPpuStateSnapshot *ppu);

/* Compose ARGB integer pixels from the immutable native frame and the PPU's
 * main-screen BG1-winner mask (opaque white = sky, opaque black = foreground).
 * Preserve all native RGB, including black dialogue fills; only actual sky
 * winners become transparent. All pitches are bytes. Output must not alias
 * either input and is unpublished/unspecified on failure. */
bool SimWorldNavigationPalace_ComposeForeground(
    uint32_t *destination, int destination_pitch,
    const uint8_t *native_pixels, int native_pitch,
    const uint8_t *sky_mask, int mask_pitch, int width, int height);

#endif /* AR_SIM_WORLD_NAVIGATION_PALACE_H */
