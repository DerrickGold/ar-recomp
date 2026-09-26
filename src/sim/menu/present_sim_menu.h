#ifndef AR_PRESENT_SIM_MENU_H
#define AR_PRESENT_SIM_MENU_H

#include "present/present.h"

bool PresentSimMenu_Active(const FrameSlot *slot);
void PresentSimMenu_Draw(const FrameSlot *slot, ArRenderRectI viewport);
void PresentSimMenu_DrawNativeHelp(const FrameSlot *slot, ArRenderRectI viewport);
void PresentSimMenu_Reset(void);

#endif
