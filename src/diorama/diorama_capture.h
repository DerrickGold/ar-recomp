#ifndef AR_DIORAMA_CAPTURE_H
#define AR_DIORAMA_CAPTURE_H
/* Diorama capture policy: requested, populated and additive plane masks
 * carried into presentation. Plane routing lives in diorama_planes.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "snesrecomp/runner.h"

typedef struct FrameSlot FrameSlot;
/* Producer-thread latch, reset at every capture preparation. Sources retain
 * native TM/TS bit order while their PPU overlays omit winner cropping. The
 * completed FrameSlot owns the resulting additive membership and mode. */
void DioramaCapture_SetIndependentSubscreenSources(uint8_t sources);
/* Capture the latched active flag and plane masks after PPU drawing.
 * NULL means no coherent PPU snapshot: requested planes remain meaningful,
 * but no content or additive input is published. Other slot fields stay put. */
void DioramaCapture_CaptureFrame(FrameSlot *frame, const SrPpuFrameSnapshot *ppu);

#endif /* AR_DIORAMA_CAPTURE_H */
