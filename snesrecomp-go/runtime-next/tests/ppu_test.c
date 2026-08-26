#include "snes/ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do {                                                \
    if (!(condition)) {                                                      \
        fprintf(stderr, "%s:%d: check failed: %s\n",                       \
                __FILE__, __LINE__, #condition);                             \
        ++failures;                                                          \
    }                                                                        \
} while (0)

static void test_register_memory_ports(Ppu *ppu) {
    ppu_reset(ppu);

    ppu_write(ppu, 0x00, 0x8fu);
    CHECK(ppu->inidisp == 0x8fu);

    /* Increment-on-high keeps the low/high writes on the same VRAM word. */
    ppu_write(ppu, 0x15, 0x80);
    ppu_write(ppu, 0x16, 0x34);
    ppu_write(ppu, 0x17, 0x12);
    ppu_write(ppu, 0x18, 0xcd);
    CHECK(ppu->vramPointer == 0x1234u);
    ppu_write(ppu, 0x19, 0xab);
    CHECK(ppu->vram[0x1234] == 0xabcdu);
    CHECK(ppu->vramPointer == 0x1235u);

    ppu_write(ppu, 0x21, 7);
    ppu_write(ppu, 0x22, 0x5a);
    ppu_write(ppu, 0x22, 0xe3);
    CHECK(ppu->cgram[7] == 0x635au);
    CHECK(ppu->cgramPointer == 8u);

    ppu_write(ppu, 0x02, 3);
    ppu_write(ppu, 0x03, 0);
    ppu_write(ppu, 0x04, 0x78);
    ppu_write(ppu, 0x04, 0x56);
    CHECK(ppu->oam[3] == 0x5678u);

    CHECK(ppu_read(ppu, 0x37) == 0u);
    CHECK(ppu_read(ppu, 0x3d) == 0xc0u);
    CHECK(ppu_read(ppu, 0x3d) == 0u);
    CHECK((ppu_read(ppu, 0x3f) & 0x40u) != 0u);
    CHECK(!ppu->countersLatched);
}

static void test_basic_bg_scanout(Ppu *ppu) {
    uint32_t pixels[kPpuXPixels * 2];
    ppu_reset(ppu);
    memset(pixels, 0xa5, sizeof(pixels));
    ppu->inidisp = 0x0fu;
    ppu->bgmode = 1u;
    ppu->screenEnabled[0] = 1u;
    ppu->bgXsc[0] = 0x20u;
    ppu->cgram[0x11] = 0x001fu;
    for (int row = 0; row < 8; ++row)
        ppu->vram[16 + row] = 0x00ffu;
    for (int entry = 0; entry < 0x400; ++entry)
        ppu->vram[0x2000 + entry] = (uint16_t)(1u | (1u << 10));

    PpuBeginDrawing(ppu, (uint8_t *)pixels,
                    kPpuXPixels * sizeof(uint32_t), 0u);
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
    CHECK(pixels[0] == 0x00ff0000u);
    CHECK(pixels[kPpuXPixels - 1] == 0x00ff0000u);

    /* Output binding observes direct host-side CGRAM edits, while hardware
     * register writes keep an already-built derived RGB entry coherent. */
    ppu->cgram[0x11] = 0x7c00u;
    PpuBeginDrawing(ppu, (uint8_t *)pixels,
                    kPpuXPixels * sizeof(uint32_t), 0u);
    ppu_runLine(ppu, 2);
    CHECK(pixels[kPpuXPixels] == 0x000000ffu);
    ppu_write(ppu, 0x21, 0x11u);
    ppu_write(ppu, 0x22, 0x1fu);
    ppu_write(ppu, 0x22, 0x00u);
    ppu_runLine(ppu, 2);
    CHECK(pixels[kPpuXPixels] == 0x00ff0000u);

    /* Add-and-half operates on the unsaturated component sum.  Saturating
     * first would incorrectly turn (31 + 31) / 2 into 15. */
    ppu->cgadsub = 0x41u;
    ppu->fixedColor = 0x001fu;
    ppu_runLine(ppu, 2);
    CHECK(pixels[kPpuXPixels] == 0x00ff0000u);

    ppu->inidisp |= 0x80u;
    ppu_runLine(ppu, 2);
    CHECK(pixels[kPpuXPixels] == 0u);
}

static int fixture_bpp(int mode, int layer) {
    static const uint8_t depths[8][4] = {
        {2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0},
        {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {8, 0, 0, 0}
    };
    return depths[mode][layer];
}

static void setup_native_fast_fixture(Ppu *ppu, uint8_t mode,
                                      uint8_t big_tiles,
                                      bool bg3_priority) {
    static const uint16_t map_bases[4] = {
        0x4000u, 0x5000u, 0x6000u, 0x7000u
    };
    uint8_t active_layers = 0u;
    ppu_reset(ppu);
    ppu->inidisp = 0x0fu;
    ppu->bgmode = (uint8_t)(mode | big_tiles | (bg3_priority ? 8u : 0u));
    ppu->bgTileAdr = 0x3210u;
    for (int layer = 0; layer < 4; ++layer)
        if (fixture_bpp(mode, layer) != 0)
            active_layers |= (uint8_t)(1u << layer);
    ppu->screenEnabled[0] = active_layers | 0x10u;
    ppu->screenEnabled[1] = active_layers | 0x10u;
    ppu->screenWindowed[0] = (active_layers & 0x05u) | 0x10u;
    ppu->screenWindowed[1] = active_layers & 0x0au;
    ppu->windowsel =
        ((uint32_t)(kWindow1Enabled | kWindow2Inversed |
                    kWindow2Enabled) << 0) |
        ((uint32_t)(kWindow1Inversed | kWindow1Enabled) << 4) |
        ((uint32_t)kWindow2Enabled << 8) |
        ((uint32_t)(kWindow1Enabled | kWindow2Enabled) << 12) |
        ((uint32_t)kWindow1Enabled << 16) |
        ((uint32_t)(kWindow1Enabled | kWindow2Enabled) << 20);
    ppu->window1left = 37u;
    ppu->window1right = 146u;
    ppu->window2left = 104u;
    ppu->window2right = 211u;
    ppu->wbgobjlog = 0x1249u;
    ppu->hScroll[0] = 5u;
    ppu->hScroll[1] = 1019u;
    ppu->hScroll[2] = 13u;
    ppu->hScroll[3] = 31u;
    ppu->vScroll[0] = 9u;
    ppu->vScroll[1] = 1017u;
    ppu->vScroll[2] = 23u;
    ppu->vScroll[3] = 1009u;
    ppu->cgwsel = 0x92u;
    ppu->cgadsub = 0x5fu;
    ppu->fixedColor = 0x294au;
    if (mode == 7u) {
        ppu->setini |= 0x40u;
        ppu->screenEnabled[0] |= 0x02u;
        ppu->screenEnabled[1] |= 0x02u;
        ppu->m7matrix[0] = 0x0140;
        ppu->m7matrix[1] = -0x0030;
        ppu->m7matrix[2] = 0x0028;
        ppu->m7matrix[3] = 0x0120;
        ppu->m7matrix[4] = 128;
        ppu->m7matrix[5] = 112;
        if (big_tiles == 0u) {
            ppu->m7matrix[6] = -300;
            ppu->m7matrix[7] = 950;
        } else {
            ppu->m7sel = big_tiles == 0x80u ? 0x80u : 0xc3u;
            ppu->m7matrix[6] = 850;
            ppu->m7matrix[7] = 850;
        }
    }
    for (int index = 0; index < kPpuCgramEntries; ++index)
        ppu->cgram[index] = (uint16_t)(((index * 13) & 31) |
            (((index * 7) & 31) << 5) | (((index * 3) & 31) << 10));
    for (int layer = 0; layer < 4; ++layer) {
        int bpp = fixture_bpp(mode, layer);
        int tile_base = layer * 0x1000;
        ppu->bgXsc[layer] = (uint8_t)((map_bases[layer] >> 8) | 3u);
        if (bpp == 0) continue;
        for (int tile = 0; tile < 256; ++tile) {
            int address = tile_base + tile *
                (bpp == 2 ? 8 : bpp == 4 ? 16 : 32);
            for (int row = 0; row < 8; ++row) {
                for (int plane = 0; plane < bpp / 2; ++plane)
                    ppu->vram[(address + row + plane * 8) & 0x7fff] =
                        (uint16_t)(
                            ((tile * (29 + plane * 7) + row * 17 +
                              layer * 71) & 0xff) |
                            (((tile * 11 + row * (43 - plane * 3) +
                               layer * 19 + plane * 61) & 0xff) << 8));
            }
        }
        for (int entry = 0; entry < 0x1000; ++entry) {
            unsigned tile = (unsigned)(entry * 17 + layer * 41) & 0x7fu;
            unsigned palette = (unsigned)(entry + layer * 3) & 7u;
            unsigned attributes = ((unsigned)entry >> 2) & 7u;
            ppu->vram[map_bases[layer] + entry] = (uint16_t)(
                tile | (palette << 10) | (attributes << 13));
        }
    }
    for (int slot = 0; slot < 128; ++slot) {
        ppu->oam[slot * 2] = 0xe000u;
        ppu->oam[slot * 2 + 1] = 0u;
    }
    for (int slot = 0; slot < 4; ++slot) {
        int x = 29 + slot * 53;
        int y = 3 + slot * 9;
        ppu->oam[slot * 2] = (uint16_t)(x | (y << 8));
        ppu->oam[slot * 2 + 1] = (uint16_t)(
            (11 + slot * 23) | ((slot & 7) << 9) |
            ((slot & 3) << 12) | ((slot & 1) ? 0x4000u : 0u) |
            ((slot & 2) ? 0x8000u : 0u));
    }
    /* Exercise cyclic OAM priority order across the 64-bit mask boundary. */
    ppu->oamaddl = 94u;
    ppu->oamaddh = 0x80u;
}

static void compare_native_fast_render(uint8_t mode, uint8_t big_tiles,
                                       bool bg3_priority, bool main_only) {
    enum { kRows = 48 };
    size_t pixel_count = (size_t)kPpuXPixels * kRows;
    uint32_t *fast_pixels = calloc(pixel_count, sizeof(*fast_pixels));
    uint32_t *reference_pixels = calloc(pixel_count, sizeof(*reference_pixels));
    uint32_t *fast_authentic = calloc(pixel_count, sizeof(*fast_authentic));
    uint32_t *reference_authentic = calloc(pixel_count,
                                            sizeof(*reference_authentic));
    Ppu *fast = ppu_init();
    Ppu *reference = ppu_init();
    CHECK(fast_pixels != NULL && reference_pixels != NULL &&
          fast_authentic != NULL && reference_authentic != NULL &&
          fast != NULL && reference != NULL);
    if (fast_pixels == NULL || reference_pixels == NULL ||
        fast_authentic == NULL || reference_authentic == NULL ||
        fast == NULL || reference == NULL) goto cleanup;
    setup_native_fast_fixture(fast, mode, big_tiles, bg3_priority);
    setup_native_fast_fixture(reference, mode, big_tiles, bg3_priority);
    if (main_only) {
        fast->cgwsel &= (uint8_t)~2u;
        reference->cgwsel &= (uint8_t)~2u;
    }
    PpuBeginDrawing(fast, (uint8_t *)fast_pixels,
                    kPpuXPixels * sizeof(uint32_t), 0u);
    PpuBeginDrawing(reference, (uint8_t *)reference_pixels,
                    kPpuXPixels * sizeof(uint32_t),
                    kPpuRenderFlags_ReferencePixelRenderer);
    CHECK(PpuBindAuthenticSurface(fast, (uint8_t *)fast_authentic,
                                  kPpuXPixels * sizeof(uint32_t)));
    CHECK(PpuBindAuthenticSurface(reference, (uint8_t *)reference_authentic,
                                  kPpuXPixels * sizeof(uint32_t)));
    ppu_runLine(fast, 0);
    ppu_runLine(reference, 0);
    for (int line = 1; line <= kRows; ++line) {
        if (line == 18) {
            PpuSetObjExactPosition(fast, 0u, 29, 25);
            PpuSetObjExactPosition(reference, 0u, 29, 25);
        } else if (line == 32) {
            PpuClearObjExactPositions(fast);
            PpuClearObjExactPositions(reference);
        } else if (line == 35) {
            ppu_write(fast, 0x01, 0xe0u);
            ppu_write(reference, 0x01, 0xe0u);
        } else if (line == 40) {
            /* Move slot zero through the hardware OAM data port after the
             * fast path has already consumed earlier scanline masks. */
            ppu_write(fast, 0x02, 0u);
            ppu_write(fast, 0x03, 0u);
            ppu_write(fast, 0x04, 29u);
            ppu_write(fast, 0x04, 45u);
            ppu_write(reference, 0x02, 0u);
            ppu_write(reference, 0x03, 0u);
            ppu_write(reference, 0x04, 29u);
            ppu_write(reference, 0x04, 45u);
        }
        ppu_runLine(fast, line);
        ppu_runLine(reference, line);
    }
    CHECK(memcmp(fast_pixels, reference_pixels,
                 pixel_count * sizeof(*fast_pixels)) == 0);
    CHECK(memcmp(fast_authentic, reference_authentic,
                 pixel_count * sizeof(*fast_authentic)) == 0);
    CHECK(memcmp(&fast->bgBuffers[0], &reference->bgBuffers[0],
                 sizeof(fast->bgBuffers[0])) == 0);
cleanup:
    ppu_free(fast);
    ppu_free(reference);
    free(fast_pixels);
    free(reference_pixels);
    free(fast_authentic);
    free(reference_authentic);
}

static void test_native_fast_path_parity(void) {
    for (uint8_t mode = 0u; mode <= 7u; ++mode) {
        compare_native_fast_render(mode, 0u, false, true);
        compare_native_fast_render(mode, 0xf0u, mode == 1u, false);
    }
    compare_native_fast_render(7u, 0x80u, false, false);
}

int main(void) {
    Ppu *ppu = ppu_init();
    CHECK(ppu != NULL);
    if (ppu != NULL) {
        test_register_memory_ports(ppu);
        test_basic_bg_scanout(ppu);
        test_native_fast_path_parity();
        ppu_free(ppu);
    }
    if (failures != 0) {
        fprintf(stderr, "runtime-next PPU tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime-next PPU tests: pass");
    return 0;
}
