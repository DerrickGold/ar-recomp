#include "cart.h"
#include "cart_map_internal.h"
#include "runner_internal.h"
#include "saveload.h"

#include <stdlib.h>
#include <string.h>

#ifndef SNESRECOMP_TRACE
#define SNESRECOMP_TRACE 0
#endif

#if SNESRECOMP_TRACE
#include "snesrecomp/game/trace.h"

static void report_unmapped(uint8_t bank, uint16_t address) {
    cpu_trace_offrails("cart_read", ((uint32_t)bank << 16) | address);
}
#else
static void report_unmapped(uint8_t bank, uint16_t address) {
    (void)bank;
    (void)address;
}
#endif

Cart *cart_init(Snes *snes) {
    Cart *cart = (Cart *)calloc(1u, sizeof(*cart));
    if (cart != NULL) {
        cart->snes = snes;
    }
    return cart;
}

void cart_free(Cart *cart) {
    if (cart == NULL) {
        return;
    }
    free(cart->rom);
    free(cart->ram);
    free(cart);
}

void cart_reset(Cart *cart) {
    /* Battery-backed SRAM and immutable ROM both survive console reset. */
    (void)cart;
}

void cart_load(Cart *cart, int type, uint8_t *rom, int rom_size, int ram_size) {
    if (cart == NULL || rom == NULL || rom_size <= 0 || ram_size < 0 ||
        (type != SR_CART_MAPPING_LOROM && type != SR_CART_MAPPING_HIROM)) {
        return;
    }

    uint8_t *new_rom = (uint8_t *)malloc((size_t)rom_size);
    uint8_t *new_ram = NULL;
    if (new_rom == NULL) {
        return;
    }
    if (ram_size > 0) {
        new_ram = (uint8_t *)calloc((size_t)ram_size, 1u);
        if (new_ram == NULL) {
            free(new_rom);
            return;
        }
    }
    memcpy(new_rom, rom, (size_t)rom_size);

    free(cart->rom);
    free(cart->ram);
    cart->type = (uint8_t)type;
    cart->rom = new_rom;
    cart->romSize = (uint32_t)rom_size;
    cart->ram = new_ram;
    cart->ramSize = (uint32_t)ram_size;
}

uint8_t cart_read(Cart *cart, uint8_t bank, uint16_t address) {
    if (cart == NULL) {
        return 0u;
    }
    const SrCartAddress mapped = sr_cart_map_read_inline(
        (SrCartMapping)cart->type, bank, address, cart->romSize, cart->ramSize);
    if (mapped.region == SR_CART_REGION_ROM && cart->rom != NULL) {
        return cart->rom[mapped.offset];
    }
    if (mapped.region == SR_CART_REGION_SRAM && cart->ram != NULL) {
        return cart->ram[mapped.offset];
    }
    report_unmapped(bank, address);
    return 0u;
}

void cart_write(Cart *cart, uint8_t bank, uint16_t address, uint8_t value) {
    if (cart == NULL || cart->ram == NULL) {
        return;
    }
    const SrCartAddress mapped = sr_cart_map_write_inline(
        (SrCartMapping)cart->type, bank, address, cart->ramSize);
    if (mapped.region == SR_CART_REGION_SRAM) {
        if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE)) {
            const uint8_t old_value = cart->ram[mapped.offset];
            cart->ram[mapped.offset] = value;
            sr_runner_emit_memory_write(
                cart->snes, SR_MEMORY_SRAM, mapped.offset,
                old_value, value, 1u);
        } else {
            cart->ram[mapped.offset] = value;
        }
    }
}

void cart_saveload(Cart *cart, SaveLoadInfo *info) {
    if (cart != NULL && info != NULL && info->func != NULL) {
        info->func(info, cart->ram, cart->ramSize);
    }
}
