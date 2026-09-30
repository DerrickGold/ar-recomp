#ifndef AR_DIORAMA_CAPTURE_H
#define AR_DIORAMA_CAPTURE_H
/* Diorama capture policy: requested, populated and additive plane masks
 * carried into presentation. Plane routing lives in diorama_planes.h. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "snesrecomp/runner.h"

typedef struct FrameSlot FrameSlot;
/* Capture the latched active flag and plane masks after PPU drawing.
 * NULL means no coherent PPU snapshot: requested planes remain meaningful,
 * but no content or additive input is published. Other slot fields stay put. */
void DioramaCapture_CaptureFrame(FrameSlot *frame, const SrPpuFrameSnapshot *ppu);

/* Presentation pixel masks obey their BG's main/subscreen window owner.
 * Row codes are 0 = original, 1/2/3 = opaque black in far/base/high.
 * Caller supplies the first display pixel of each row (apron/pitch already
 * accounted for). Other pixels and missing destinations remain untouched. */
bool DioramaCapture_PixelLayerVisible(const SrPpuStateSnapshot *ppu,
                                       unsigned bg, int screen_x);
uint32_t DioramaCapture_PaintBlackRow(unsigned bg, const uint8_t *bands,
                                     uint32_t *rows[3], size_t width);
/* Codes 4/5/6 replace all bands of one BG with stamped far/base/high pixels.
 * A transparent stamp clears prior scenery and retains the plane's backing. */
uint32_t DioramaCapture_PaintEditedRow(unsigned bg, const uint8_t *codes,
                                      const uint32_t *colors, uint32_t backing,
                                      uint32_t *rows[3], size_t width);
uint32_t DioramaCapture_StampColor(const SrPpuStateSnapshot *ppu, unsigned bg,
                                  const uint16_t *vram, const uint16_t *cgram,
                                  uint16_t entry, unsigned x, unsigned y,
                                  uint32_t capture_flags);

#endif /* AR_DIORAMA_CAPTURE_H */
