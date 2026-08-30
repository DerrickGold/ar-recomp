#include "snes/dsp.h"

#include <stdbool.h>
#include <string.h>

void dsp_setBusGains(int music_percent, int sfx_percent) {
  (void)music_percent;
  (void)sfx_percent;
}

void dsp_setMusicBusMuted(bool muted) { (void)muted; }

void dsp_setUnclassifiedMusicSourceMinimum(int source_number) {
  (void)source_number;
}

bool dsp_extendedVoicesEnabled(void) { return false; }

void dsp_copyRegisters(const Dsp *dsp, uint8_t registers[0x80]) {
  if (dsp == NULL || registers == NULL) return;
  memcpy(registers, dsp->ram, sizeof(dsp->ram));
}
