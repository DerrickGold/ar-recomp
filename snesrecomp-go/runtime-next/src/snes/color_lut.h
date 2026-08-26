/* Present-only color transform. The colorimetry model is derived from the
 * recomp ecosystem's MIT OR Apache-2.0 screen component by Jrickey. */
#ifndef RUNTIME_NEXT_COLOR_LUT_H
#define RUNTIME_NEXT_COLOR_LUT_H

#include <stddef.h>
#include <stdint.h>

typedef enum SnesColorModel {
    kSnesColorModelRaw = 0,
    kSnesColorModelCrt,
    kSnesColorModelTrinitron
} SnesColorModel;

int snes_color_lut_setup(void);
int snes_color_lut_configure(SnesColorModel model);
int snes_color_lut_active(void);
void snes_color_lut_map(const uint32_t *source, uint32_t *destination,
                        size_t pixel_count);

#endif
