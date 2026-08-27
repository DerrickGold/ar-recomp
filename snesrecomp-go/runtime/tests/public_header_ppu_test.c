#include "snesrecomp/runner/ppu.h"

int snesrecomp_public_header_ppu_probe(void) {
    return SR_PPU_NATIVE_WIDTH == 256u ? 0 : 1;
}
