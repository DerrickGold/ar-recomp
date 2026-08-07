#include "cpu_state.h"
#include "funcs.h"
#include "actraiser_rtl.h"

void ResetSpritesFunc(int wh) {
  for (; wh < 128; wh++)
    g_ram[0x201 + wh * 4] = 0xf0;
}
