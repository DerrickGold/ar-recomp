#include "snes/cart.h"
#include "snes/cart_map.h"
#include "snes/saveload.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static unsigned save_calls = 0u;
static size_t saved_size = 0u;
static uint8_t saved_first = 0u;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next cart contract failed: %s\n", message);
        ++failures;
    }
}

static void capture_save(SaveLoadInfo *info, void *data, size_t data_size) {
    (void)info;
    ++save_calls;
    saved_size = data_size;
    saved_first = data_size == 0u ? 0u : ((const uint8_t *)data)[0];
}

static void test_mapping_core(void) {
    SrCartAddress mapped = sr_cart_map_read(
        SR_CART_MAPPING_LOROM, 0x01u, 0x8000u, 0x20000u, 0x8000u);
    check(mapped.region == SR_CART_REGION_ROM && mapped.offset == 0x8000u,
          "LoROM bank mapping");

    mapped = sr_cart_map_read(
        SR_CART_MAPPING_LOROM, 0x81u, 0x8000u, 0x20000u, 0x8000u);
    check(mapped.region == SR_CART_REGION_ROM && mapped.offset == 0x8000u,
          "LoROM high-bank mirror");

    mapped = sr_cart_map_write(
        SR_CART_MAPPING_LOROM, 0xF0u, 0x1234u, 0x8000u);
    check(mapped.region == SR_CART_REGION_SRAM && mapped.offset == 0x1234u,
          "LoROM F0 SRAM write window");

    mapped = sr_cart_map_read(
        SR_CART_MAPPING_HIROM, 0x00u, 0x8000u, 0x20000u, 0x2000u);
    check(mapped.region == SR_CART_REGION_ROM && mapped.offset == 0x8000u,
          "HiROM low-bank upper-half mapping");

    mapped = sr_cart_map_write(
        SR_CART_MAPPING_HIROM, 0xA0u, 0x6000u, 0x2000u);
    check(mapped.region == SR_CART_REGION_SRAM && mapped.offset == 0u,
          "HiROM SRAM mirror");

    mapped = sr_cart_map_read(
        SR_CART_MAPPING_LOROM, 0x00u, 0x2000u, 0x20000u, 0u);
    check(mapped.region == SR_CART_REGION_UNMAPPED, "unmapped LoROM window");

    mapped = sr_cart_map_read(
        SR_CART_MAPPING_LOROM, 0x03u, 0x8000u, 0x18000u, 0u);
    check(mapped.region == SR_CART_REGION_ROM && mapped.offset == 0u,
          "non-power-of-two ROM wrapping");
}

static void test_cart_adapter(void) {
    uint8_t rom[0x20000];
    for (size_t index = 0; index < sizeof(rom); ++index) {
        rom[index] = (uint8_t)(index >> 15);
    }

    Cart *cart = cart_init(NULL);
    check(cart != NULL, "cart allocation");
    if (cart == NULL) {
        return;
    }
    cart_load(cart, SR_CART_MAPPING_LOROM, rom, (int)sizeof(rom), 0x8000);
    check(cart->rom != rom, "ROM must be owned copy");
    check(cart_read(cart, 0x00u, 0x8000u) == 0u, "LoROM first bank read");
    check(cart_read(cart, 0x01u, 0x8000u) == 1u, "LoROM second bank read");

    cart_write(cart, 0x70u, 0x0000u, 0xA5u);
    check(cart_read(cart, 0x70u, 0x0000u) == 0xA5u, "LoROM SRAM read/write");
    cart_write(cart, 0xF0u, 0x0001u, 0x5Au);
    check(cart_read(cart, 0xF0u, 0x0001u) == 0x5Au,
          "LoROM mirrored SRAM read/write");

    const uint8_t before = cart_read(cart, 0x00u, 0x8000u);
    cart_write(cart, 0x00u, 0x8000u, (uint8_t)(before + 1u));
    check(cart_read(cart, 0x00u, 0x8000u) == before, "ROM writes are ignored");

    SaveLoadInfo save = {capture_save};
    cart_saveload(cart, &save);
    check(save_calls == 1u && saved_size == 0x8000u && saved_first == 0xA5u,
          "SRAM save/load span");

    cart_load(cart, SR_CART_MAPPING_HIROM, rom, (int)sizeof(rom), 0x2000);
    check(cart->ram != NULL && cart->ram[0] == 0u, "reload clears new SRAM");
    cart_write(cart, 0x20u, 0x6000u, 0x3Cu);
    check(cart_read(cart, 0xA0u, 0x6000u) == 0x3Cu, "HiROM SRAM mirror read/write");
    check(cart_read(cart, 0x00u, 0x8000u) == 1u, "HiROM ROM mapping");

    cart_free(cart);

    Cart *rom_only = cart_init(NULL);
    check(rom_only != NULL, "ROM-only cart allocation");
    if (rom_only != NULL) {
        cart_load(rom_only, SR_CART_MAPPING_LOROM, rom, (int)sizeof(rom), 0);
        saved_size = 1u;
        cart_saveload(rom_only, &save);
        check(save_calls == 2u && saved_size == 0u,
              "ROM-only save/load keeps the zero-length callback");
        cart_free(rom_only);
    }
}

int main(void) {
    test_mapping_core();
    test_cart_adapter();
    return failures == 0 ? 0 : 1;
}
