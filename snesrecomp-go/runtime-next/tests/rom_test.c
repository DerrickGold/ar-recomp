#include "snes/rom.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next ROM contract failed: %s\n", message);
        ++failures;
    }
}

static void write_header(uint8_t *data, size_t location, const char *title,
                         uint8_t map_mode, uint8_t chips, uint8_t ram_exp,
                         uint8_t region, uint16_t reset_vector) {
    memset(data + location, 0, 0x40u);
    memset(data + location, ' ', 21u);
    const size_t title_length = strlen(title) < 21u ? strlen(title) : 21u;
    memcpy(data + location, title, title_length);
    data[location + 0x15u] = map_mode;
    data[location + 0x16u] = chips;
    data[location + 0x17u] = 5u;
    data[location + 0x18u] = ram_exp;
    data[location + 0x19u] = region;
    data[location + 0x1cu] = 0x34u;
    data[location + 0x1du] = 0x12u;
    data[location + 0x1eu] = 0xcbu;
    data[location + 0x1fu] = 0xedu;
    data[location + 0x3cu] = (uint8_t)reset_vector;
    data[location + 0x3du] = (uint8_t)(reset_vector >> 8);
}

static void test_validation(void) {
    uint8_t small[16] = {0};
    SrRomInfo info;
    check(sr_rom_analyze(NULL, sizeof(small), &info) == SR_ROM_INVALID_ARGUMENT,
          "null source rejected");
    check(sr_rom_analyze(small, sizeof(small), &info) == SR_ROM_TOO_SMALL,
          "small image rejected");
    check(sr_rom_analyze(small, sizeof(small), NULL) == SR_ROM_INVALID_ARGUMENT,
          "null result rejected");
    check(strcmp(sr_rom_status_string(SR_ROM_TOO_SMALL),
                 "ROM is smaller than 32 KiB") == 0,
          "status text remains useful to frontends");
}

static void test_lorom(void) {
    uint8_t *rom = (uint8_t *)calloc(0x8000u, 1u);
    check(rom != NULL, "LoROM fixture allocation");
    if (rom == NULL) return;
    write_header(rom, 0x7fc0u, "PORTABLE LOROM", 0x20u, 0x01u, 3u, 0u, 0x8000u);
    rom[0] = 0x78u;

    SrRomInfo info;
    check(sr_rom_analyze(rom, 0x8000u, &info) == SR_ROM_OK, "LoROM analysis");
    check(info.mapping == SR_CART_MAPPING_LOROM, "LoROM mapping selected");
    check(info.payload_offset == 0u && info.payload_size == 0x8000u,
          "headerless payload range");
    check(info.ram_size == 0x2000u, "SRAM capacity decoded");
    check(!info.pal && strncmp(info.title, "PORTABLE LOROM", 14u) == 0,
          "title and NTSC metadata");
    free(rom);
}

static void test_hirom_preference(void) {
    uint8_t *rom = (uint8_t *)calloc(0x10000u, 1u);
    check(rom != NULL, "HiROM fixture allocation");
    if (rom == NULL) return;
    write_header(rom, 0x7fc0u, "BAD LOW HEADER", 0xf0u, 0x77u, 0xffu, 0xffu, 0x1000u);
    write_header(rom, 0xffc0u, "PORTABLE HIROM", 0x21u, 0x00u, 0u, 2u, 0x8000u);
    rom[0x8000u] = 0x18u;

    SrRomInfo info;
    check(sr_rom_analyze(rom, 0x10000u, &info) == SR_ROM_OK, "HiROM analysis");
    check(info.mapping == SR_CART_MAPPING_HIROM, "higher-scoring HiROM selected");
    check(info.pal && info.ram_size == 0u, "PAL and ROM-only metadata");
    free(rom);
}

static void test_copier_header_and_mirroring(void) {
    const size_t payload_size = 0x18000u;
    const size_t total_size = payload_size + 0x200u;
    uint8_t *rom = (uint8_t *)malloc(total_size);
    check(rom != NULL, "copier fixture allocation");
    if (rom == NULL) return;
    memset(rom, 0xa5, 0x200u);
    for (size_t index = 0; index < payload_size; ++index) {
        rom[0x200u + index] = (uint8_t)(index >> 15);
    }
    write_header(rom, 0x81c0u, "COPIER LOROM", 0x20u, 0x00u, 0u, 0u, 0x8000u);
    rom[0x200u] = 0x78u;

    SrRomImage image;
    check(sr_rom_prepare(rom, total_size, &image) == SR_ROM_OK,
          "copier image preparation");
    check(image.info.payload_offset == 0x200u && image.size == 0x20000u,
          "copier header removed and image rounded");
    check(image.data != NULL && image.data[0] == 0x78u,
          "payload begins after copier header");
    check(image.data != NULL && image.data[0x18000u] == image.data[0x10000u] &&
          image.data[0x1ffffu] == image.data[0x17fffu],
          "non-power-of-two tail mirrors into capacity");
    sr_rom_release(&image);
    check(image.data == NULL && image.size == 0u, "release clears ownership");
    free(rom);
}

int main(void) {
    test_validation();
    test_lorom();
    test_hirom_preference();
    test_copier_header_and_mirroring();
    return failures == 0 ? 0 : 1;
}
