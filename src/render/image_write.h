#ifndef AR_IMAGE_WRITE_H
#define AR_IMAGE_WRITE_H
/* Lossless diagnostic image output. Encoding belongs to the host adapter;
 * callers supply borrowed pixels and retain ownership of the open file. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef enum ArImageFormat {
  kArImageRgb24, kArImageRgba32, kArImageXrgb8888
} ArImageFormat;
bool ArImage_WritePng(FILE *file, const uint8_t *pixels, int width, int height,
    int pitch, ArImageFormat format);
#endif
