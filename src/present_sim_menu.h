#ifndef PRESENT_SIM_MENU_H
#define PRESENT_SIM_MENU_H

#include "present.h"

bool PresentSimMenu_Active(const FrameSlot *slot);
void PresentSimMenu_Draw(const FrameSlot *slot, ArRenderRectI viewport);
void PresentSimMenu_DrawNativeHelp(const FrameSlot *slot, ArRenderRectI viewport);
void PresentSimMenu_Reset(void);

#endif
