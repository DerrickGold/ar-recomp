#include "snes/dsp.h"

#include <stdbool.h>
#include <string.h>

void dsp_setBusGains(int music_percent, int sfx_percent) {
  (void)music_percent;
  (void)sfx_percent;
}

bool dsp_extendedVoicesEnabled(void) { return false; }

void dsp_copyRegisters(const Dsp *dsp, uint8_t registers[0x80]) {
  if (dsp == NULL || registers == NULL) return;
  memcpy(registers, dsp->ram, sizeof(dsp->ram));
}
