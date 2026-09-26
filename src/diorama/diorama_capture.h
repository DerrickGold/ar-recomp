#ifndef AR_DIORAMA_CAPTURE_H
#define AR_DIORAMA_CAPTURE_H
/* Diorama capture policy: requested, populated and additive plane masks
 * carried into presentation. Plane routing lives in diorama_planes.h. */

#include <stddef.h>
#include <stdint.h>
#include "snesrecomp/runner.h"

typedef struct FrameSlot FrameSlot;
/* Capture the latched active flag and plane masks after PPU drawing.
 * NULL means no coherent PPU snapshot: requested planes remain meaningful,
 * but no content or additive input is published. Other slot fields stay put. */
void DioramaCapture_CaptureFrame(FrameSlot *frame, const SrPpuFrameSnapshot *ppu);

#endif /* AR_DIORAMA_CAPTURE_H */
