#ifndef AR_SIM_MENU_CAPTURE_H
#define AR_SIM_MENU_CAPTURE_H
/* Capture the modern SIM menu's model, help, input hint and native artwork.
 * The presenter consumes this snapshot without querying live game/input state. */

#include "sim/menu/sim_menu_art.h"

/* Called with a zeroed frame after the coherent PPU snapshot is acquired.
 * Pass a NULL API when that snapshot is unavailable: model/help still copy,
 * but input hints, native confirmation and art stay uncaptured. */
void SimMenu_CaptureFrame(SimMenuFrame *frame, const SnesRunnerApi *api,
                           SrRunnerHandle *runner);

#endif /* AR_SIM_MENU_CAPTURE_H */
