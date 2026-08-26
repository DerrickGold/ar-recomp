#pragma once

#include "cart_map.h"

static inline uint32_t sr_cart_wrap_offset(uint32_t offset, uint32_t size) {
    if (size == 0u) {
        return 0u;
    }
    if ((size & (size - 1u)) == 0u) {
        return offset & (size - 1u);
    }
    return offset % size;
}

static inline int sr_cart_lorom_sram_window(uint8_t bank, uint16_t address) {
    return address < 0x8000u &&
           ((bank >= 0x70u && bank <= 0x7Du) || bank >= 0xF0u);
}

static inline int sr_cart_hirom_sram_window(uint8_t bank, uint16_t address) {
    const uint8_t mirrored_bank = bank & 0x7Fu;
    return mirrored_bank < 0x40u && address >= 0x6000u && address < 0x8000u;
}

static inline SrCartAddress sr_cart_unmapped(void) {
    const SrCartAddress result = {SR_CART_REGION_UNMAPPED, 0u};
    return result;
}

static inline SrCartAddress sr_cart_map_read_inline(
    SrCartMapping mapping, uint8_t bank, uint16_t address,
    uint32_t rom_size, uint32_t ram_size) {
    SrCartAddress result = sr_cart_unmapped();
    if (mapping == SR_CART_MAPPING_LOROM) {
        if (ram_size != 0u && sr_cart_lorom_sram_window(bank, address)) {
            const uint32_t raw = ((uint32_t)(bank & 0x0Fu) << 15) | address;
            result.region = SR_CART_REGION_SRAM;
            result.offset = sr_cart_wrap_offset(raw, ram_size);
            return result;
        }
        bank &= 0x7Fu;
        if (rom_size != 0u && (address >= 0x8000u || bank >= 0x40u)) {
            const uint32_t raw = ((uint32_t)bank << 15) | (address & 0x7FFFu);
            result.region = SR_CART_REGION_ROM;
            result.offset = sr_cart_wrap_offset(raw, rom_size);
        }
        return result;
    }

    if (mapping == SR_CART_MAPPING_HIROM) {
        bank &= 0x7Fu;
        if (ram_size != 0u && sr_cart_hirom_sram_window(bank, address)) {
            const uint32_t raw = ((uint32_t)(bank & 0x3Fu) << 13) |
                                 (address & 0x1FFFu);
            result.region = SR_CART_REGION_SRAM;
            result.offset = sr_cart_wrap_offset(raw, ram_size);
            return result;
        }
        if (rom_size != 0u && (address >= 0x8000u || bank >= 0x40u)) {
            const uint32_t raw = ((uint32_t)(bank & 0x3Fu) << 16) | address;
            result.region = SR_CART_REGION_ROM;
            result.offset = sr_cart_wrap_offset(raw, rom_size);
        }
    }
    return result;
}

static inline SrCartAddress sr_cart_map_write_inline(
    SrCartMapping mapping, uint8_t bank, uint16_t address, uint32_t ram_size) {
    SrCartAddress result = sr_cart_unmapped();
    if (ram_size == 0u) {
        return result;
    }

    uint32_t raw = 0u;
    if (mapping == SR_CART_MAPPING_LOROM &&
        sr_cart_lorom_sram_window(bank, address)) {
        raw = ((uint32_t)(bank & 0x0Fu) << 15) | address;
    } else if (mapping == SR_CART_MAPPING_HIROM &&
               sr_cart_hirom_sram_window(bank, address)) {
        raw = ((uint32_t)(bank & 0x3Fu) << 13) | (address & 0x1FFFu);
    } else {
        return result;
    }

    result.region = SR_CART_REGION_SRAM;
    result.offset = sr_cart_wrap_offset(raw, ram_size);
    return result;
}
