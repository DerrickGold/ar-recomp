#ifndef AR_HD_TILE_CENSUS_H
#define AR_HD_TILE_CENSUS_H

#include "snesrecomp/runner.h"

/* Survey the current PPU generation when AR_TILE_CENSUS or AR_M7_DUMP is
 * enabled. The runner handle is opaque; this developer tool never reaches
 * through it to concrete emulator state. */
void HdTileCensus_Frame(SrRunnerHandle *runner);

#endif /* AR_HD_TILE_CENSUS_H */
