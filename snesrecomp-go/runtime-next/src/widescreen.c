#include "widescreen.h"

#include <string.h>

void RtlWidescreenPresent(uint8_t *destination, size_t destination_pitch,
                          const uint8_t *source, int width, int height) {
    if (destination == NULL || source == NULL || width <= 0 || height <= 0) {
        return;
    }
    const size_t row_bytes = (size_t)width * 4u;
    for (int row = 0; row < height; ++row) {
        memcpy(destination + (size_t)row * destination_pitch,
               source + (size_t)row * row_bytes, row_bytes);
    }
}
