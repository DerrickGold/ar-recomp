#include "cart_map_internal.h"

SrCartAddress sr_cart_map_read(SrCartMapping mapping, uint8_t bank,
                               uint16_t address, uint32_t rom_size,
                               uint32_t ram_size) {
    return sr_cart_map_read_inline(mapping, bank, address, rom_size, ram_size);
}

SrCartAddress sr_cart_map_write(SrCartMapping mapping, uint8_t bank,
                                uint16_t address, uint32_t ram_size) {
    return sr_cart_map_write_inline(mapping, bank, address, ram_size);
}
