#ifndef ACTRAISER_RTL_H
#define ACTRAISER_RTL_H

#include "common_rtl.h"
#include "common_cpu_infra.h"
#include "snes/snes_regs.h"

void ActRaiserDrawPpuFrame(void);
void ActRaiser_RebindPpuOutputSurfaces(void);
void RunOneFrameOfGame(void);
int ActRaiser_ReadRdnmi(Snes *snes);
bool ActRaiser_RecoverDispatchMiss(uint32 source_pc24, uint32 target_pc24);

/* BG-only widescreen presentation helpers. These never replace the game's OAM
 * builder or normal tile streamers. The Sky Palace pair temporarily decodes a
 * box-free source map into only BG2's margin columns, then restores game VRAM. */
int ActRaiser_WidescreenBgRefreshEnabled(void);
void ActRaiser_WidescreenMarginRefresh(void);
void ActRaiser_WidescreenSkyPalacePrepare(void);
void ActRaiser_WidescreenSkyPalaceRestore(void);
void ActRaiser_WidescreenSpriteActivationProbe(void);

#endif  // ACTRAISER_RTL_H
