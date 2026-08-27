#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SrCartMapping {
    SR_CART_MAPPING_NONE = 0,
    SR_CART_MAPPING_LOROM = 1,
    SR_CART_MAPPING_HIROM = 2,
} SrCartMapping;

typedef enum SrCartRegion {
    SR_CART_REGION_UNMAPPED = 0,
    SR_CART_REGION_ROM = 1,
    SR_CART_REGION_SRAM = 2,
} SrCartRegion;

typedef struct SrCartAddress {
    SrCartRegion region;
    uint32_t offset;
} SrCartAddress;

/* Pure address decoders: no allocation, global state, or host dependencies. */
SrCartAddress sr_cart_map_read(SrCartMapping mapping, uint8_t bank,
                               uint16_t address, uint32_t rom_size,
                               uint32_t ram_size);
SrCartAddress sr_cart_map_write(SrCartMapping mapping, uint8_t bank,
                                uint16_t address, uint32_t ram_size);

#ifdef __cplusplus
}
#endif
