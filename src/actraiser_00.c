#include "cpu_state.h"
#include "funcs.h"
#include "actraiser_rtl.h"
#include "variables.h"

uint16 counter_global_frames = 0;

void ResetSpritesFunc(int wh) {
  for (; wh < 128; wh++)
    g_ram[0x201 + wh * 4] = 0xf0;
}
