#ifndef AR_PRESENT_SIM_MENU_H
#define AR_PRESENT_SIM_MENU_H
/* PresentSimMenu: draws the SIM town menu (categories, inventory, help pages
 * and dialogue frames) from the menu frame captured into the FrameSlot.
 * Phase: present (FrameSlot only).
 * Tests: tests/present_frame_order_test.c */

#include "present/present.h"

bool PresentSimMenu_Active(const FrameSlot *slot);
void PresentSimMenu_Draw(const FrameSlot *slot, ArRenderRectI viewport);
void PresentSimMenu_DrawNativeHelp(const FrameSlot *slot, ArRenderRectI viewport);
void PresentSimMenu_Reset(void);

#endif
