#ifndef AR_SIM_FRAME_CAPTURE_H
#define AR_SIM_FRAME_CAPTURE_H
/* SimFrameCapture: the SIM pipeline's producer work for each drawn frame. It
 * builds the developed world map, captures and annotates the frame's SIM
 * metadata, renders the town canvas (fanning its pixel rows out to a small
 * worker group of its own), and feeds the scene inspector and the metadata
 * trace. Frame submission reuses the SimFrameData it fills.
 * Phase: game (main thread, after the PPU frame draw). */

#include "sim/sim_render_metadata.h"

void SimFrameCapture_Produce(SimFrameData *sim);
/* Releases the town canvas worker group. */
void SimFrameCapture_Shutdown(void);

#endif  /* AR_SIM_FRAME_CAPTURE_H */
