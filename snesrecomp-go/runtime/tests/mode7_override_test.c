#include "snes/ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

enum { kRows = 4, kWidth = 272, kScaleMax = 4 };
static uint32_t native[kWidth * kPpuBufHeight], authentic[kWidth * kPpuBufHeight];
static uint32_t overlay[kWidth * kRows * kScaleMax * kScaleMax + 2];
static uint32_t art[16];

static uint32_t sample(unsigned scale, unsigned x, unsigned y);

static void setup(Ppu *ppu, unsigned scale, unsigned reference) {
    CHECK(PpuBindMode7OverlaySurface(ppu, NULL, 0u, 0u, 0u));
    ppu_reset(ppu);
    memset(native, 0xa5, sizeof(native));
    memset(overlay, 0xa5, sizeof(overlay));
    ppu->inidisp = 15u;
    ppu->bgmode = 7u;
    ppu->screenEnabled[0] = 1u;
    ppu->m7matrix[0] = ppu->m7matrix[3] = 256;
    ppu->cgram[0] = 0x7c00u; // blue backdrop
    ppu->cgram[1] = 0x03e0u; // green original BG1
    // Every canvas tile uses character 1; its pixels are green.
    for (int i = 0; i < 0x4000; ++i) ppu->vram[i] = 1u;
    for (int i = 64; i < 128; ++i) ppu->vram[i] |= 0x0100u;
    PpuBeginDrawing(ppu, (uint8_t *)native, kWidth * 4u,
        reference ? kPpuRenderFlags_ReferencePixelRenderer : 0u);
    CHECK(PpuBindAuthenticSurface(ppu, (uint8_t *)authentic, kWidth * 4u));
    CHECK(PpuBindMode7OverlaySurface(ppu, (uint8_t *)(overlay + 1),
        kWidth * scale * 4u, (uint8_t)scale, kRows * scale));
    for (unsigned i = 0; i < 16; ++i) art[i] = 0xffff0000u;
    CHECK(PpuSetMode7Override(ppu, art, 4, 4, 10, 1, 12, 3, 0u));
    ppu_runLine(ppu, 0);
}

static void test_transform(Ppu *ppu, unsigned reference) {
    setup(ppu, 2, reference);
    // Four distinct texels within each native pixel: not an upscaled ROM.
    for (unsigned i = 0; i < 16; ++i) art[i] = 0xff000020u + i;
    ppu_runLine(ppu, 1);
    CHECK(sample(2, 20, 0) == art[0]);
    CHECK(sample(2, 21, 0) == art[1]);
    CHECK(sample(2, 20, 1) == art[4]);
    CHECK(sample(2, 23, 1) == art[7]);
    CHECK(sample(2, 19, 0) == 0u);
    CHECK(sample(2, 24, 0) == 0u);
    CHECK(authentic[8 + 10] == 0x0000ff00u);
    // Emulate HDMA changing the matrix/scroll between scanlines.
    ppu->m7matrix[6] = -10;
    ppu_runLine(ppu, 2);
    CHECK(sample(2, 20, 0) == art[0]);
    CHECK(sample(2, 20, 2) == 0u);
    CHECK(sample(2, 40, 2) == art[8]);

    setup(ppu, 2, reference);
    for (unsigned i = 0; i < 16; ++i) art[i] = 0xff000020u + i;
    ppu->m7sel = 1u; // X flip reverses subpixels too.
    ppu_runLine(ppu, 1);
    CHECK(sample(2, 244*2, 0) == art[3]);
    CHECK(sample(2, 244*2+1, 0) == art[2]);
    CHECK(sample(2, 245*2+1, 0) == art[0]);
    ppu->m7sel = 2u; // Y flip at scanline 1 maps to canvas row 254.
    CHECK(PpuSetMode7Override(ppu, art, 4, 4, 10, 253, 12, 255, 0));
    ppu_runLine(ppu, 1);
    CHECK(sample(2, 20, 0) == art[12]);
    CHECK(sample(2, 20, 1) == art[8]);

    setup(ppu, 1, reference);
    for (unsigned i = 0; i < 16; ++i) art[i] = 0xff000020u + i;
    // 90-degree rotation around a translated centre; independent expectations.
    ppu->m7matrix[0] = ppu->m7matrix[3] = 0;
    ppu->m7matrix[1] = 256;
    ppu->m7matrix[2] = -256;
    ppu->m7matrix[4] = ppu->m7matrix[6] = 10;
    ppu->m7matrix[5] = ppu->m7matrix[7] = 12;
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 9, 0) == art[15]);
    CHECK(sample(1, 10, 0) == art[7]);
    CHECK(sample(1, 11, 0) == 0xff0000ffu); // remove native edge footprint

    setup(ppu, 4, reference);
    // A scaled edge intersects only the last half of one native pixel.
    ppu->m7matrix[0] = 512;
    CHECK(PpuSetMode7Override(ppu, art, 4, 4, 11, 1, 13, 3, 0));
    ppu_runLine(ppu, 1);
    CHECK(sample(4, 5*4+1, 0) == 0u);
    CHECK(sample(4, 5*4+2, 0) == 0xffff0000u);
    CHECK(sample(4, 6*4+1, 0) == 0xffff0000u);
    // Native fetch at x=6 was inside the replaced rect. Clear its coarse
    // footprint outside the HD edge rather than leaking green ROM pixels.
    CHECK(sample(4, 6*4+2, 0) == 0xff0000ffu);

    setup(ppu, 2, reference);
    ppu->mosaic = 0x11u; // 2x2 mosaic samples the same texel in each block.
    CHECK(PpuSetMode7Override(ppu, art, 4, 4, 10, 0, 12, 2, 0));
    art[0] = 0xff123456u;
    ppu_runLine(ppu, 1);
    CHECK(sample(2, 20, 0) == art[0]);
    CHECK(sample(2, 23, 1) == art[0]);
}

static void test_visibility(Ppu *ppu, unsigned reference) {
    setup(ppu, 1, reference);
    // Texture alpha reveals the underlying scene, never the replaced green BG1.
    for (unsigned i = 0; i < 16; ++i) art[i] = 0x80ff0000u;
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xff80007fu);
    memset(art, 0, sizeof(art));
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xff0000ffu);
    for (unsigned i = 0; i < 16; ++i) art[i] = 0xffff0000u;
    ppu->inidisp = 7;
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xff770000u);
    ppu->inidisp = 0x8f;
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0u);
    ppu->inidisp = 15;
    ppu->screenEnabled[0] = 0;
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0u);
    ppu->screenEnabled[0] = 1;
    ppu->screenWindowed[0] = 1;
    ppu->windowsel = kWindow1Enabled;
    ppu->window1left = ppu->window1right = 10;
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0u);
    CHECK(sample(1, 11, 0) == 0xffff0000u);

    setup(ppu, 1, reference);
    ppu->screenEnabled[0] |= 0x10;
    ppu->obsel = 2; // OBJ characters at word $4000, outside Mode-7 data.
    for (unsigned i = 0; i < 128; ++i) ppu->oam[i*2] = 100u << 8;
    for (unsigned i = 0; i < 8; ++i) {
        ppu->vram[0x4000+i] = 0xff;
        ppu->vram[0x4008+i] = 0;
    }
    ppu->oam[0] = 10;
    ppu->oam[1] = 3u << 12;
    ppu->cgram[0x81] = 0x03ff;
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xffffff00u);
    ppu->oam[1] = 0; // Lower priority than BG1, so the HD red wins.
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xffff0000u);

    setup(ppu, 1, reference);
    ppu->setini = 0x40; // EXTBG high priority covers replacement BG1.
    ppu->screenEnabled[0] = 3;
    ppu->cgram[0x81] = 0x03e0;
    for (unsigned i = 64; i < 128; ++i) ppu->vram[i] |= 0x8000;
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xff00ff00u);
    for (unsigned i = 64; i < 128; ++i) ppu->vram[i] &= ~0x8000u;
    ppu_runLine(ppu, 1); // EXTBG low priority is below replacement BG1.
    CHECK(sample(1, 10, 0) == 0xffff0000u);

    setup(ppu, 1, reference);
    ppu->fixedColor = 0x001f;
    ppu->cgadsub = 0x81; // BG1 - red fixed colour.
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xff000000u);
    ppu->cgadsub = 0x41; // Add then halve before saturation, not after.
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xffff0000u);
    ppu->cgwsel = 0xc0; // Clip main to black.
    ppu->cgadsub = 0;
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xff000000u);
    // Subscreen BG1 can contribute to backdrop colour math, too.
    ppu->cgwsel = 2;
    ppu->cgadsub = 0x20;
    ppu->screenEnabled[0] = 0;
    ppu->screenEnabled[1] = 1;
    ppu_runLine(ppu, 1);
    CHECK(sample(1, 10, 0) == 0xffff00ffu);
}

static void test_bounds_and_lifetime(Ppu *ppu, unsigned reference) {
    setup(ppu, 2, reference);
    // Negative coordinates must not become enormous texture indices.
    ppu->m7matrix[6] = -1014;
    ppu_runLine(ppu, 1);
    CHECK(sample(2, 0, 0) == 0u);
    CHECK(PpuSetMode7Override(ppu, art, 4, 4, 10, 1, 12, 3, 1));
    ppu_runLine(ppu, 1);
    CHECK(sample(2, 0, 0) == 0xffff0000u);
    // Exact right/bottom half-open bounds and a non-square source.
    ppu->m7matrix[6] = 0;
    CHECK(PpuSetMode7Override(ppu, art, 1, 2, 10, 1, 12, 3, 0));
    art[0] = 0xff112233;
    art[1] = 0xff445566;
    ppu_runLine(ppu, 1);
    ppu_runLine(ppu, 2);
    ppu_runLine(ppu, 3);
    CHECK(sample(2, 20, 0) == art[0]);
    CHECK(sample(2, 20, 2) == art[1]);
    CHECK(sample(2, 24, 0) == 0u);
    CHECK(sample(2, 20, 4) == 0u);
    PpuClearOverlayCaptures(ppu);
    ppu_runLine(ppu, 1);
    CHECK(sample(2, 20, 0) == 0u); // clear stale pixels after disable
    CHECK(PpuSetMode7Override(ppu, art, 1, 2, 10, 1, 12, 3, 0));
    ppu->bgmode = 1;
    ppu_runLine(ppu, 2);
    CHECK(sample(2, 20, 2) == 0u); // per-line mode change

    setup(ppu, 4, reference);
    CHECK(PpuBindMode7OverlaySurface(ppu, (uint8_t *)(overlay + 1),
        kWidth*4*4, 4, 1)); // one subpixel row of real capacity
    ppu_runLine(ppu, 1);
    ppu_runLine(ppu, 2);
    CHECK(sample(4, 40, 0) == 0xffff0000u);
    CHECK(overlay[1+kWidth*4] == 0xa5a5a5a5u);
    CHECK(overlay[0] == 0xa5a5a5a5u);
    CHECK(PpuBindMode7OverlaySurface(ppu, NULL, 0, 0, 0));
    CHECK(!PpuSetMode7Override(ppu, art, 4, 4, 10, 1, 12, 3, 0));
    ppu_runLine(ppu, 1);
    CHECK(native[8+10] == 0x0000ff00u); // unbound destination fails open

    setup(ppu, 2, reference);
    PpuSetExtraSpace(ppu, 8);
    ppu->m7matrix[6] = 18;
    ppu_runLine(ppu, 1);
    CHECK(overlay[1] == 0xffff0000u); // left margin, not an apron overrun
    CHECK(overlay[0] == 0xa5a5a5a5u);
}

static uint32_t sample(unsigned scale, unsigned x, unsigned y) {
    // The 272-pixel pitch has an 8-pixel apron around the native 256 pixels.
    return overlay[1 + y * kWidth * scale + 8 * scale + x];
}

int main(void) {
    Ppu *ppu = ppu_init();
    if (!ppu) return 1;
    for (unsigned reference = 0; reference < 2; ++reference) {
        for (unsigned scale = 1; scale <= 4; ++scale) {
            setup(ppu, scale, reference);
            ppu_runLine(ppu, 1);
            CHECK(sample(scale, 10 * scale, 0) == 0xffff0000u);
            CHECK(sample(scale, 12 * scale, 0) == 0u);
            CHECK(native[8 + 10] == 0x0000ff00u);
            CHECK(overlay[0] == 0xa5a5a5a5u);
            CHECK(overlay[1 + kWidth*kRows*scale*scale] == 0xa5a5a5a5u);
        }
        test_transform(ppu, reference);
        test_visibility(ppu, reference);
        test_bounds_and_lifetime(ppu, reference);
    }
    ppu_free(ppu);
    if (failures) fprintf(stderr, "Mode-7 override: %u failures\n", failures);
    else puts("Mode-7 override: pass");
    return failures ? 1 : 0;
}
