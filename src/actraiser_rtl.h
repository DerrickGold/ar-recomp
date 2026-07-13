#ifndef ACTRAISER_RTL_H
#define ACTRAISER_RTL_H

#include "common_rtl.h"
#include "common_cpu_infra.h"
#include "snes/snes_regs.h"

void ActRaiserDrawPpuFrame(void);
void RunOneFrameOfGame(void);

/* BG-only action-stage widescreen refresh. These never replace the game's
 * OAM builder or normal tile streamers. */
int ActRaiser_WidescreenBgRefreshEnabled(void);
void ActRaiser_WidescreenMarginRefresh(void);
void ActRaiser_WidescreenSpriteActivationProbe(void);

#endif  // ACTRAISER_RTL_H
