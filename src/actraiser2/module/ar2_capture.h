#ifndef AR_AR2_CAPTURE_H
#define AR_AR2_CAPTURE_H
#include "actraiser2/imported_game_api.h"
#include "ar2_world.h"

/* AR2 module capture owns native PPU planes and the independent HUD surface. */

bool Ar2Capture_Draw(SrRunnerHandle *runner, const SnesRunnerApi *api, const uint8_t *wram,
                     int display_extra, bool diorama, int camera_shift,
                     SrPpuScanoutIrqCallback irq);
const Ar2VideoFrame *Ar2Capture_Frame(void);
#endif
