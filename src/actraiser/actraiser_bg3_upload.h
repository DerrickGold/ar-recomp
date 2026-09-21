#ifndef ACTRAISER_BG3_UPLOAD_H
#define ACTRAISER_BG3_UPLOAD_H

#include "snesrecomp/game/cpu.h"

/* One native upload seam publishes both HUD and credits ownership. */
void ActRaiserBg3Upload_Reset(void);
bool ActRaiser_Bg3UploadEntry(CpuState *cpu);
RecompReturn ActRaiser_Bg3Upload(CpuState *cpu);

#endif
