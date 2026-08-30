#include "snesrecomp/spc_upload.h"

#include <stdio.h>
#include <string.h>

static int failures;

static int marked(const uint8_t bitmap[0x2000], uint16_t address) {
    return (bitmap[address >> 3] & (uint8_t)(1u << (address & 7u))) != 0u;
}

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime SPC upload failed: %s\n", message);
        ++failures;
    }
}

static void test_image_blocks_and_wrapping(void) {
    uint8_t rom[32] = {0};
    uint8_t aram[0x10000] = {0};
    SrSpcUploadResult result;
    uint8_t written[0x2000] = {0};
    /* Two bytes at $ffff wrap the second byte to ARAM $0000. */
    rom[0] = 2; rom[2] = 0xff; rom[3] = 0xff;
    rom[4] = 0xa5; rom[5] = 0x5a;
    /* Terminator and entry point. */
    rom[6] = 0; rom[7] = 0; rom[8] = 0x34; rom[9] = 0x12;
    sr_spc_upload_begin_write_tracking(written, sizeof(written));
    check(sr_spc_upload_image(rom, sizeof(rom), 0, aram, &result),
          "valid IPL image");
    sr_spc_upload_end_write_tracking();
    check(aram[0xffff] == 0xa5 && aram[0] == 0x5a,
          "ARAM destination wrapping");
    check(result.entry_point == 0x1234 && result.block_count == 1,
          "entry and block count");
    check(result.script_offset == 9, "stage-two script starts at entry high byte");
    check(marked(written, 0xffff) && marked(written, 0) &&
          !marked(written, 1), "IPL write provenance follows ARAM wrapping");
}

static void test_rom_mirror(void) {
    uint8_t rom[8] = {0};
    uint8_t aram[0x10000] = {0};
    SrSpcUploadResult result;
    /* Start at offset 6: the four-byte terminator wraps across the ROM end. */
    rom[6] = 0; rom[7] = 0; rom[0] = 0x00; rom[1] = 0x04;
    check(sr_spc_upload_image(rom, sizeof(rom), 6, aram, &result),
          "mirrored terminator");
    check(result.entry_point == 0x0400, "mirrored entry point");
}

static void test_raw_rom_copy_and_wrapping(void) {
    uint8_t rom[4] = {0x10, 0x20, 0x30, 0x40};
    uint8_t aram[0x10000] = {0};
    uint8_t written[0x2000] = {0};
    sr_spc_upload_begin_write_tracking(written, sizeof(written));
    check(sr_spc_upload_copy_rom(rom, sizeof(rom), 3u, aram, 0xffffu, 3u),
          "valid raw ROM copy");
    sr_spc_upload_end_write_tracking();
    check(aram[0xffff] == 0x40 && aram[0] == 0x10 && aram[1] == 0x20,
          "raw copy mirrors ROM and wraps ARAM");
    check(marked(written, 0xffff) && marked(written, 0) &&
              marked(written, 1) && !marked(written, 2),
          "raw copy write provenance");
    check(!sr_spc_upload_copy_rom(rom, sizeof(rom), 0u, aram, 0u,
                                  0x10001u),
          "oversized raw copy rejected");
}

static void test_sample_script(void) {
    uint8_t rom[64] = {0};
    uint8_t aram[0x10000] = {0};
    uint16_t last_destination = 0;
    uint16_t last_length = 0;
    uint8_t written[0x2000] = {0};
    rom[2] = 1; rom[3] = 0;       /* script: chunk 1, then chunk 0 */
    rom[16] = 3; rom[17] = 0;     /* chunk 0 */
    rom[18] = 0x10; rom[19] = 0x11; rom[20] = 0x12;
    rom[21] = 2; rom[22] = 0;     /* chunk 1 */
    rom[23] = 0x20; rom[24] = 0x21;
    sr_spc_upload_begin_write_tracking(written, sizeof(written));
    check(sr_spc_upload_samples(rom, sizeof(rom), 2, 2, 16, 0xfffe,
                                aram, &last_destination, &last_length),
          "valid second-stage script");
    sr_spc_upload_end_write_tracking();
    check(aram[0xfffe] == 0x20 && aram[0xffff] == 0x21,
          "selected first sample chunk");
    check(aram[0] == 0x10 && aram[1] == 0x11 && aram[2] == 0x12,
          "packed second chunk with ARAM wrap");
    check(last_destination == 0 && last_length == 3,
          "second-stage exit metadata");
    check(marked(written, 0xfffe) && marked(written, 0xffff) &&
          marked(written, 0) && marked(written, 1) && marked(written, 2) &&
          !marked(written, 3),
          "sample write provenance covers packed wrapped chunks");
}

static void test_invalid_arguments(void) {
    uint8_t aram[0x10000];
    SrSpcUploadResult result;
    check(!sr_spc_upload_image(NULL, 1, 0, aram, &result), "null ROM rejected");
    check(!sr_spc_upload_image((const uint8_t *)"x", 0, 0, aram, &result),
          "empty ROM rejected");
    check(!sr_spc_upload_samples(NULL, 1, 0, 1, 0, 0, aram, NULL, NULL),
          "null sample ROM rejected");
}

int main(void) {
    test_image_blocks_and_wrapping();
    test_rom_mirror();
    test_raw_rom_copy_and_wrapping();
    test_sample_script();
    test_invalid_arguments();
    return failures != 0;
}
