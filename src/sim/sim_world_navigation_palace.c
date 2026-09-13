#include "sim_world_navigation_palace.h"
#include "sim_ppu_color_math.h"

#include <stddef.h>
#include <string.h>

bool SimWorldNavigationPalace_PpuSupported(const SrPpuStateSnapshot *ppu) {
  return ppu && ppu->struct_size >= SR_PPU_STATE_SNAPSHOT_V2_SIZE &&
      ppu->bg_mode_control == 9 && ppu->main_screen == 0x17 &&
      ppu->sub_screen == 0 &&
      (ppu->flags & SR_PPU_STATE_FORCED_BLANK) == 0 &&
      SimPpuColorMath_IsNoOp(ppu->color_math_control,
          ppu->color_math_designation, ppu->fixed_color);
}

bool SimWorldNavigationPalace_ComposeForeground(
    uint32_t *destination, int destination_pitch,
    const uint8_t *native_pixels, int native_pitch,
    const uint8_t *sky_mask, int mask_pitch, int width, int height) {
  if (!destination || !native_pixels || !sky_mask || width <= 0 || height <= 0 ||
      width > kSimWorldNavigationPalaceMaxWidth ||
      height > kSimWorldNavigationPalaceMaxHeight ||
      destination_pitch < width * (int)sizeof(uint32_t) ||
      destination_pitch % (int)sizeof(uint32_t) != 0 ||
      native_pitch < width * (int)sizeof(uint32_t) ||
      mask_pitch < width * (int)sizeof(uint32_t)) return false;
  if ((size_t)destination_pitch > SIZE_MAX / (size_t)height ||
      (size_t)native_pitch > SIZE_MAX / (size_t)height ||
      (size_t)mask_pitch > SIZE_MAX / (size_t)height) return false;
  for (int y = 0; y < height; y++) {
    uint32_t *row = destination + (size_t)y *
        ((size_t)destination_pitch / sizeof(uint32_t));
    const uint8_t *native = native_pixels + (size_t)y * (size_t)native_pitch;
    const uint8_t *mask = sky_mask + (size_t)y * (size_t)mask_pitch;
    for (int x = 0; x < width; x++) {
      uint32_t winner, pixel;
      memcpy(&winner, mask + (size_t)x * sizeof(winner), sizeof(winner));
      memcpy(&pixel, native + (size_t)x * sizeof(pixel), sizeof(pixel));
      if (winner != UINT32_C(0xff000000) && winner != UINT32_C(0xffffffff))
        return false; /* Absent/stale/arbitrary overlays are not a sky mask. */
      row[x] = winner == UINT32_C(0xffffffff) ? 0 : pixel | UINT32_C(0xff000000);
    }
  }
  return true;
}
