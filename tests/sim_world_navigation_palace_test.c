#include "sim/sim_world_navigation_palace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void TestPpuGate(void) {
  SrPpuStateSnapshot ppu = {
    .struct_size = SR_PPU_STATE_SNAPSHOT_V2_SIZE,
    .bg_mode_control = 9, .main_screen = 0x17,
    .color_math_designation = 1, .brightness = 15,
  };
  assert(SimWorldNavigationPalace_PpuSupported(&ppu));
  ppu.brightness = 0;
  assert(SimWorldNavigationPalace_PpuSupported(&ppu)); /* load under native fade */
  ppu.flags = SR_PPU_STATE_FORCED_BLANK;
  assert(!SimWorldNavigationPalace_PpuSupported(&ppu));
  ppu.flags = 0;
  ppu.bg_mode_control = 7;
  assert(!SimWorldNavigationPalace_PpuSupported(&ppu));
  ppu.bg_mode_control = 9;
  ppu.main_screen = 0x15;
  assert(!SimWorldNavigationPalace_PpuSupported(&ppu));
  ppu.main_screen = 0x17;
  ppu.sub_screen = 1;
  assert(!SimWorldNavigationPalace_PpuSupported(&ppu));
  ppu.sub_screen = 0;
  ppu.fixed_color = 1;
  assert(!SimWorldNavigationPalace_PpuSupported(&ppu));
  ppu.fixed_color = 0;
  ppu.color_math_control = 2;
  assert(!SimWorldNavigationPalace_PpuSupported(&ppu));
  ppu.color_math_control = 0;
  ppu.color_math_designation = 0x81;
  assert(!SimWorldNavigationPalace_PpuSupported(&ppu));
  ppu.color_math_designation = 1;
  ppu.struct_size--;
  assert(!SimWorldNavigationPalace_PpuSupported(&ppu));
  assert(!SimWorldNavigationPalace_PpuSupported(NULL));
}

static void TestForeground(void) {
  const uint32_t native[2][5] = {
    {0x123456, 0, 0xffffff, 0x789abc, 0x11223344},
    {0x654321, 0x142536, 0x28394a, 0x445566, 0x55667788},
  };
  uint32_t mask[2][6] = {
    {0xffffffff, 0xff000000, 0xff000000, 0xffffffff, 0x12345678, 0},
    {0xff000000, 0xffffffff, 0xffffffff, 0xff000000, 0, 0x98765432},
  };
  uint32_t out[2][7];
  memset(out, 0x7b, sizeof(out));
  assert(SimWorldNavigationPalace_ComposeForeground(&out[0][0], sizeof(out[0]),
      (const uint8_t *)native, sizeof(native[0]),
      (const uint8_t *)mask, sizeof(mask[0]), 4, 2));
  assert(out[0][0] == 0 && out[0][3] == 0);
  assert(out[0][1] == 0xff000000); /* Never color-key black dialogue fills. */
  assert(out[0][2] == 0xffffffff);
  assert(out[1][0] == 0xff654321 && out[1][3] == 0xff445566);
  assert(out[1][1] == 0 && out[1][2] == 0);
  for (int y = 0; y < 2; y++)
    for (int x = 4; x < 7; x++) assert(out[y][x] == 0x7b7b7b7b);
  /* Same RGB can be either sky or native foreground. Membership comes only
   * from the PPU winner, not palette guesses or a rectangular cutout. */
  mask[0][0] = 0xff000000;
  mask[0][2] = 0xffffffff;
  assert(SimWorldNavigationPalace_ComposeForeground(&out[0][0], sizeof(out[0]),
      (const uint8_t *)native, sizeof(native[0]),
      (const uint8_t *)mask, sizeof(mask[0]), 4, 2));
  assert(out[0][0] == 0xff123456 && out[0][2] == 0);
  mask[0][0] = 0; /* Missing capture, not an opaque-black winner mask. */
  assert(!SimWorldNavigationPalace_ComposeForeground(&out[0][0], sizeof(out[0]),
      (const uint8_t *)native, sizeof(native[0]),
      (const uint8_t *)mask, sizeof(mask[0]), 4, 2));
  mask[0][0] = 0xff000000;
  assert(!SimWorldNavigationPalace_ComposeForeground(&out[0][0], 15,
      (const uint8_t *)native, 20, (const uint8_t *)mask, 24, 4, 2));
  assert(!SimWorldNavigationPalace_ComposeForeground(&out[0][0], 28,
      (const uint8_t *)native, 15, (const uint8_t *)mask, 24, 4, 2));
  assert(!SimWorldNavigationPalace_ComposeForeground(&out[0][0], 28,
      (const uint8_t *)native, 20, (const uint8_t *)mask, 15, 4, 2));
  assert(!SimWorldNavigationPalace_ComposeForeground(&out[0][0], 28,
      (const uint8_t *)native, 20, (const uint8_t *)mask, 24, 0, 2));
  assert(!SimWorldNavigationPalace_ComposeForeground(&out[0][0], 28,
      (const uint8_t *)native, 20, (const uint8_t *)mask, 24, 4, 241));
}

int main(void) {
  TestPpuGate();
  TestForeground();
  puts("sim_world_navigation_palace_test: PASS");
  return 0;
}
