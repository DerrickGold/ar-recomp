#ifndef SNESRECOMP_ROM_H
#define SNESRECOMP_ROM_H

#include "cart_map.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SrRomStatus {
    SR_ROM_OK = 0,
    SR_ROM_INVALID_ARGUMENT,
    SR_ROM_TOO_SMALL,
    SR_ROM_TOO_LARGE,
    SR_ROM_OUT_OF_MEMORY
} SrRomStatus;

typedef struct SrRomInfo {
    SrCartMapping mapping;
    size_t header_offset;
    size_t payload_offset;
    size_t payload_size;
    uint32_t declared_rom_size;
    uint32_t ram_size;
    int16_t header_score;
    uint8_t region;
    uint8_t version;
    bool pal;
    char title[22];
} SrRomInfo;

typedef struct SrRomImage {
    uint8_t *data;
    size_t size;
    SrRomInfo info;
} SrRomImage;

SrRomStatus sr_rom_analyze(const uint8_t *data, size_t size, SrRomInfo *info);
SrRomStatus sr_rom_prepare(const uint8_t *data, size_t size, SrRomImage *image);
void sr_rom_release(SrRomImage *image);
const char *sr_rom_status_string(SrRomStatus status);

#ifdef __cplusplus
}
#endif

#endif
