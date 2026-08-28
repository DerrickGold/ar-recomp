#include "snes/ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

typedef struct PpuMemoryState {
    SaveLoadInfo base;
    uint8_t bytes[70000];
    size_t offset;
} PpuMemoryState;

static void transfer_state(SaveLoadInfo *base, void *data, size_t size) {
    PpuMemoryState *state = (PpuMemoryState *)base;
    if (state->offset + size > sizeof(state->bytes)) {
        base->failed = true;
        return;
    }
    if (base->saving)
        memcpy(state->bytes + state->offset, data, size);
    else
        memcpy(data, state->bytes + state->offset, size);
    state->offset += size;
}

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

    ppu_latchCounters(ppu, 0x0123u, 0x0101u);
    CHECK(ppu_read(ppu, 0x37) == 0u);
    CHECK(ppu_read(ppu, 0x3c) == 0x23u);
    CHECK(ppu_read(ppu, 0x3c) == 1u);
    CHECK(ppu_read(ppu, 0x3d) == 1u);
    CHECK(ppu_read(ppu, 0x3d) == 1u);
    CHECK((ppu_read(ppu, 0x3f) & 0x40u) != 0u);
    CHECK(!ppu->countersLatched);
}

static void test_portable_saveload(Ppu *ppu) {
    static PpuMemoryState state;
    memset(&state, 0, sizeof(state));
    state.base.func = transfer_state;
    state.base.saving = true;
    state.base.portable = true;
    ppu_reset(ppu);
    ppu->inidisp = 0x8du;
    ppu->vramPointer = 0x3456u;
    ppu->cgramSecondWrite = true;
    ppu->m7startX = -123456;
    ppu->cgram[17] = 0x4567u;
    ppu->vram[0x2345] = 0x89abu;
    ppu_saveload(ppu, &state.base);
    CHECK(!state.base.failed && state.offset < sizeof(state.bytes));
    ppu_reset(ppu);
    state.offset = 0u;
    state.base.saving = false;
    ppu_saveload(ppu, &state.base);
    CHECK(!state.base.failed);
    CHECK(ppu->inidisp == 0x8du && ppu->vramPointer == 0x3456u);
    CHECK(ppu->cgramSecondWrite && ppu->m7startX == -123456);
    CHECK(ppu->cgram[17] == 0x4567u && ppu->vram[0x2345] == 0x89abu);
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

static uint32_t fixture_color_rgb(uint16_t color) {
    unsigned red = color & 31u;
    unsigned green = (color >> 5) & 31u;
    unsigned blue = (color >> 10) & 31u;
    return ((red * 255u / 31u) << 16) |
           ((green * 255u / 31u) << 8) |
           (blue * 255u / 31u);
}

static void fill_solid_4bpp_tile(Ppu *ppu, int tile, unsigned pixel) {
    for (int row = 0; row < 8; ++row) {
        ppu->vram[tile * 16 + row] = (uint16_t)(
            ((pixel & 1u) ? 0x00ffu : 0u) |
            ((pixel & 2u) ? 0xff00u : 0u));
        ppu->vram[tile * 16 + row + 8] = (uint16_t)(
            ((pixel & 4u) ? 0x00ffu : 0u) |
            ((pixel & 8u) ? 0xff00u : 0u));
    }
}

static void test_tilemap_screen_wrapping(Ppu *ppu) {
    static const uint16_t colors[4] = {
        0x001fu, 0x03e0u, 0x7c00u, 0x7fffu
    };
    uint32_t pixels[kPpuXPixels];
    for (unsigned size = 0; size < 4u; ++size) {
        for (unsigned quadrant = 0; quadrant < 4u; ++quadrant) {
            ppu_reset(ppu);
            memset(pixels, 0, sizeof(pixels));
            ppu->inidisp = 0x0fu;
            ppu->bgmode = 1u;
            ppu->screenEnabled[0] = 1u;
            ppu->bgXsc[0] = (uint8_t)(0x60u | size);
            ppu->hScroll[0] = (quadrant & 1u) ? 256u : 0u;
            ppu->vScroll[0] = (quadrant & 2u) ? 255u : 0u;
            for (unsigned tile = 1; tile <= 4u; ++tile) {
                fill_solid_4bpp_tile(ppu, (int)tile, tile);
                ppu->cgram[tile] = colors[tile - 1u];
            }
            ppu->vram[0x6000] = 1u;
            ppu->vram[0x6400] = 2u;
            ppu->vram[0x6800] = 3u;
            ppu->vram[0x6c00] = 4u;
            PpuBeginDrawing(ppu, (uint8_t *)pixels,
                            kPpuXPixels * sizeof(uint32_t), 0u);
            ppu_runLine(ppu, 0);
            ppu_runLine(ppu, 1);
            unsigned offset = 0u;
            if ((quadrant & 1u) != 0u && (size & 1u) != 0u)
                offset += 1u;
            if ((quadrant & 2u) != 0u && (size & 2u) != 0u)
                offset += (size & 1u) != 0u ? 2u : 1u;
            CHECK(pixels[0] == fixture_color_rgb(colors[offset]));
        }
    }
}

typedef struct VirtualPaddingFixture {
    unsigned lookups;
    uint16_t entry;
} VirtualPaddingFixture;

static bool virtual_padding_lookup(const void *context, int32_t tile_x,
                                   int32_t tile_y, uint16_t *entry) {
    VirtualPaddingFixture *fixture = (VirtualPaddingFixture *)context;
    (void)tile_x;
    (void)tile_y;
    fixture->lookups++;
    *entry = fixture->entry;
    return true;
}

static void test_virtual_provider_does_not_own_padding(Ppu *ppu) {
    enum { kExtra = 8, kWidth = kPpuXPixels + kExtra * 2 };
    uint32_t pixels[kWidth];
    for (unsigned mirror = 0; mirror < 2u; ++mirror) {
        VirtualPaddingFixture fixture = {0u, 2u};
        PpuVirtualTilemapBinding binding = {
            .lookup = virtual_padding_lookup,
            .context = &fixture,
        };
        ppu_reset(ppu);
        memset(pixels, 0, sizeof(pixels));
        ppu->inidisp = 0x0fu;
        ppu->bgmode = 1u;
        ppu->screenEnabled[0] = 1u;
        ppu->bgXsc[0] = 0x20u;
        ppu->cgram[1] = 0x001fu;
        ppu->cgram[2] = 0x7c00u;
        fill_solid_4bpp_tile(ppu, 1, 1u);
        fill_solid_4bpp_tile(ppu, 2, 2u);
        for (int entry = 0; entry < 0x400; ++entry)
            ppu->vram[0x2000 + entry] = 1u;
        PpuSetExtraSpace(ppu, kExtra);
        if (mirror)
            PpuSetWidescreenLayerMirror(ppu, 1u);
        else
            PpuSetWidescreenLayerRepeat(ppu, 1u);
        CHECK(PpuSetVirtualTilemap(ppu, 0u, &binding));
        PpuBeginDrawing(ppu, (uint8_t *)pixels,
                        kWidth * sizeof(uint32_t), 0u);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        CHECK(pixels[0] == fixture_color_rgb(ppu->cgram[1]));
        CHECK(pixels[kExtra] == fixture_color_rgb(ppu->cgram[1]));
        CHECK(pixels[kWidth - 1] == fixture_color_rgb(ppu->cgram[1]));
        CHECK(fixture.lookups == 0u);
    }
}

static void test_virtual_provider_lookup_is_tile_shaped(Ppu *ppu) {
    uint32_t pixels[kPpuXPixels];
    VirtualPaddingFixture fixture = {0u, 1u};
    PpuVirtualTilemapBinding binding = {
        .lookup = virtual_padding_lookup,
        .context = &fixture,
        .flags = kPpuVirtualTilemapFlag_IncludeAuthentic,
    };
    ppu_reset(ppu);
    memset(pixels, 0, sizeof(pixels));
    ppu->inidisp = 0x0fu;
    ppu->bgmode = 1u;
    ppu->screenEnabled[0] = 1u;
    ppu->cgram[1] = 0x001fu;
    fill_solid_4bpp_tile(ppu, 1, 1u);
    CHECK(PpuSetVirtualTilemap(ppu, 0u, &binding));
    PpuBeginDrawing(ppu, (uint8_t *)pixels,
                    sizeof(pixels), 0u);
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
    CHECK(pixels[0] == fixture_color_rgb(ppu->cgram[1]));
    CHECK(pixels[kPpuXPixels - 1] == fixture_color_rgb(ppu->cgram[1]));
    CHECK(fixture.lookups == kPpuXPixels / 8u);
}

typedef struct VirtualVerticalFixture {
    unsigned lookups;
    int32_t first_tile_y;
    int32_t last_tile_y;
} VirtualVerticalFixture;

static bool virtual_vertical_lookup(const void *context, int32_t tile_x,
                                    int32_t tile_y, uint16_t *entry) {
    VirtualVerticalFixture *fixture = (VirtualVerticalFixture *)context;
    (void)tile_x;
    if (fixture->lookups == 0u) fixture->first_tile_y = tile_y;
    fixture->last_tile_y = tile_y;
    fixture->lookups++;
    *entry = 1u;
    return true;
}

static void test_virtual_provider_vertical_margin_continues_world(Ppu *ppu) {
    enum { kExtra = 8, kRows = kPpuYPixels + kExtra * 2 };
    static uint32_t pixels[kPpuXPixels * kRows];
    VirtualVerticalFixture fixture = {0};
    PpuVirtualTilemapBinding binding = {
        .lookup = virtual_vertical_lookup,
        .context = &fixture,
        .flags = kPpuVirtualTilemapFlag_IncludeAuthentic,
    };
    ppu_reset(ppu);
    memset(pixels, 0, sizeof(pixels));
    ppu->inidisp = 0x0fu;
    ppu->bgmode = 1u;
    ppu->screenEnabled[0] = 1u;
    ppu->cgram[1] = 0x001fu;
    fill_solid_4bpp_tile(ppu, 1, 1u);
    PpuSetExtraVerticalSpace(ppu, kExtra, kExtra);
    CHECK(PpuSetVirtualTilemap(ppu, 0u, &binding));
    PpuBeginDrawing(ppu, (uint8_t *)pixels,
                    kPpuXPixels * sizeof(uint32_t), 0u);
    ppu_runLine(ppu, 0);

    /* The eighth row above the authentic viewport has sample Y -7, which is
     * virtual tile row -1.  Holding authentic Y zero here stretches the first
     * tile row across the whole diorama apron. */
    ppu_runMarginLine(ppu, 1 - kExtra);
    CHECK(fixture.lookups != 0u);
    CHECK(fixture.first_tile_y == -1);
    CHECK(fixture.last_tile_y == -1);
    CHECK(pixels[0] == fixture_color_rgb(ppu->cgram[1]));

    fixture.lookups = 0u;
    /* The first row below the 224-line viewport samples Y 225, in tile row
     * 28, rather than repeating authentic sample Y 223 / tile row 27. */
    ppu_runMarginLine(ppu, kPpuYPixels + 1);
    CHECK(fixture.lookups != 0u);
    CHECK(fixture.first_tile_y == 28);
    CHECK(fixture.last_tile_y == 28);
}

static void test_hud_split_does_not_widen_bg3(Ppu *ppu) {
    enum { kExtra = 8, kWidth = kPpuXPixels + kExtra * 2 };
    uint32_t pixels[kWidth * 2];
    ppu_reset(ppu);
    memset(pixels, 0, sizeof(pixels));
    ppu->inidisp = 0x0fu;
    ppu->bgmode = 1u;
    ppu->screenEnabled[0] = 1u << 2;
    ppu->bgXsc[2] = 0x20u;
    ppu->cgram[1] = 0x001fu;
    for (int row = 0; row < 8; ++row)
        ppu->vram[8 + row] = 0x00ffu;
    for (int entry = 0; entry < 0x400; ++entry)
        ppu->vram[0x2000 + entry] = 1u | 0x2000u;
    PpuSetExtraSpace(ppu, kExtra);
    PpuSetWidescreenHudSplit(ppu, 40u, 88u, 168u, 40u, 28u);
    PpuBeginDrawing(ppu, (uint8_t *)pixels,
                    kWidth * sizeof(uint32_t), 0u);
    ppu_runLine(ppu, 0);
    ppu_runLine(ppu, 1);
    CHECK(pixels[0] == 0u);
    CHECK(pixels[kExtra] == fixture_color_rgb(ppu->cgram[1]));
    CHECK(pixels[kWidth - 1] == 0u);

    /* Some action maps use BG3 for level content below their HUD.  That
     * explicit band threshold, rather than the mere presence of a HUD split,
     * is what grants BG3 margin ownership. */
    PpuSetWidescreenBg3Widen(ppu, 1u);
    ppu_runLine(ppu, 2);
    CHECK(pixels[kWidth] == fixture_color_rgb(ppu->cgram[1]));
    CHECK(pixels[kWidth * 2 - 1] == fixture_color_rgb(ppu->cgram[1]));
}

static void test_mode7_hardware_origin(Ppu *ppu) {
    uint32_t pixels[kPpuXPixels];
    for (unsigned reference = 0; reference < 2u; ++reference) {
        ppu_reset(ppu);
        memset(pixels, 0, sizeof(pixels));
        ppu->inidisp = 0x0fu;
        ppu->bgmode = 7u;
        ppu->screenEnabled[0] = 1u;
        ppu->m7matrix[0] = 0x100;
        ppu->m7matrix[3] = 0x100;
        ppu->m7matrix[4] = 100;
        ppu->m7matrix[6] = 100;
        ppu->cgram[1] = 0x001fu;
        ppu->vram[12] = 1u;
        ppu->vram[64 + 8 + 4] = 0x0100u;
        PpuBeginDrawing(ppu, (uint8_t *)pixels,
                        kPpuXPixels * sizeof(uint32_t),
                        reference ? kPpuRenderFlags_ReferencePixelRenderer
                                  : 0u);
        ppu_runLine(ppu, 0);
        ppu_runLine(ppu, 1);
        CHECK(pixels[0] == 0x00ff0000u);
    }
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
                                       bool bg3_priority, bool main_only,
                                       bool unwindowed_subscreen,
                                       uint8_t mosaic_size,
                                       uint8_t mosaic_layers) {
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
    if (mosaic_size > 1u) {
        fast->mosaic = reference->mosaic = (uint8_t)(
            ((mosaic_size - 1u) << 4) | (mosaic_layers & 0x0fu));
    }
    if (main_only) {
        fast->cgwsel &= (uint8_t)~2u;
        reference->cgwsel &= (uint8_t)~2u;
    } else if (unwindowed_subscreen) {
        fast->screenWindowed[0] = fast->screenWindowed[1] = 0u;
        reference->screenWindowed[0] = reference->screenWindowed[1] = 0u;
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
        compare_native_fast_render(
            mode, 0u, false, true, false, 1u, 0u);
        compare_native_fast_render(
            mode, 0xf0u, mode == 1u, false, false, 1u, 0u);
        if (mode < 7u)
            compare_native_fast_render(
                mode, 0u, mode == 1u, false, true, 1u, 0u);
    }
    compare_native_fast_render(
        7u, 0x80u, false, false, false, 1u, 0u);
    /* Mosaic remains local to the selected tiled BGs across depth, tile-size,
     * window, main/subscreen, and group-phase combinations. */
    for (uint8_t mode = 0u; mode < 7u; ++mode) {
        compare_native_fast_render(
            mode, 0u, mode == 1u, true, false, 2u, 0x05u);
        compare_native_fast_render(
            mode, 0xf0u, mode == 1u, false, false, 5u, 0x0au);
        compare_native_fast_render(
            mode, 0u, mode == 1u, false, true, 16u, 0x0fu);
    }
    /* Mode 7 mosaic deliberately remains on the reference affine path. */
    compare_native_fast_render(
        7u, 0u, false, false, false, 5u, 0x03u);
}

static void compare_native_capture_path(bool deferred, uint8_t mode,
                                        uint8_t mosaic_size) {
    enum { kRows = 48, kPlanes = 4 };
    const size_t pixel_count = (size_t)kPpuXPixels * kRows;
    Ppu *fast = ppu_init();
    Ppu *reference = ppu_init();
    uint32_t *fast_pixels = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *reference_pixels = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *fast_authentic = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *reference_authentic = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *fast_range = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *reference_range = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *fast_overlay[kPpuOverlaySource_Count][kPlanes] = {{0}};
    uint32_t *reference_overlay[kPpuOverlaySource_Count][kPlanes] = {{0}};
    CHECK(fast != NULL && reference != NULL && fast_pixels != NULL &&
          reference_pixels != NULL && fast_authentic != NULL &&
          reference_authentic != NULL && fast_range != NULL &&
          reference_range != NULL);
    if (fast == NULL || reference == NULL || fast_pixels == NULL ||
        reference_pixels == NULL || fast_authentic == NULL ||
        reference_authentic == NULL || fast_range == NULL ||
        reference_range == NULL) goto cleanup;
    setup_native_fast_fixture(fast, mode, 0u, mode == 1u);
    setup_native_fast_fixture(reference, mode, 0u, mode == 1u);
    if (mosaic_size > 1u) {
        fast->mosaic = reference->mosaic =
            (uint8_t)(((mosaic_size - 1u) << 4) | 0x03u);
    }
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        for (int plane = 0; plane < kPlanes; ++plane) {
            fast_overlay[source][plane] =
                calloc(pixel_count, sizeof(uint32_t));
            reference_overlay[source][plane] =
                calloc(pixel_count, sizeof(uint32_t));
            CHECK(fast_overlay[source][plane] != NULL &&
                  reference_overlay[source][plane] != NULL);
            if (fast_overlay[source][plane] == NULL ||
                reference_overlay[source][plane] == NULL) goto cleanup;
        }
        CHECK(PpuBindOverlaySurface(
            fast, (PpuOverlaySource)source,
            (uint8_t *)fast_overlay[source][0],
            kPpuXPixels * sizeof(uint32_t)));
        CHECK(PpuBindOverlaySurface(
            reference, (PpuOverlaySource)source,
            (uint8_t *)reference_overlay[source][0],
            kPpuXPixels * sizeof(uint32_t)));
        for (int band = 1;
             band < (source == kPpuOverlaySource_Obj ? kPlanes : 3);
             ++band) {
            CHECK(PpuBindOverlayPrioSurface(
                fast, (PpuOverlaySource)source, (uint8_t)band,
                (uint8_t *)fast_overlay[source][band]));
            CHECK(PpuBindOverlayPrioSurface(
                reference, (PpuOverlaySource)source, (uint8_t)band,
                (uint8_t *)reference_overlay[source][band]));
        }
    }
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        int x = source == kPpuOverlaySource_Bg3 ? 19 : 0;
        int width = source == kPpuOverlaySource_Bg3
            ? 181 : kPpuXPixels;
        uint8_t flags = kPpuOverlayFlag_RemoveFromGame;
        if (deferred) {
            if (source == kPpuOverlaySource_Bg2 ||
                source == kPpuOverlaySource_Obj)
                flags |= kPpuOverlayFlag_MarkFullAddSubscreen;
            else if (source == kPpuOverlaySource_Bg3)
                flags |= kPpuOverlayFlag_MarkOwningScreenWinner;
            else if (source == kPpuOverlaySource_Bg1)
                flags |= kPpuOverlayFlag_MarkMainScreenWinner;
        } else if (source == kPpuOverlaySource_Bg1) {
            flags |= kPpuOverlayFlag_MarkBgHalfAdd;
        } else if (source == kPpuOverlaySource_Bg2) {
            flags |= kPpuOverlayFlag_ApplyBgFixedColorSubtract;
        } else if (source == kPpuOverlaySource_Obj) {
            flags |= kPpuOverlayFlag_MarkObjColorMath;
        }
        CHECK(PpuSetOverlayCapture(
            fast, (PpuOverlaySource)source,
            x, 0, width, kRows, flags));
        CHECK(PpuSetOverlayCapture(
            reference, (PpuOverlaySource)source,
            x, 0, width, kRows, flags));
    }
    CHECK(PpuSetOverlayOamRange(fast, 0u, 128u));
    CHECK(PpuSetOverlayOamRange(reference, 0u, 128u));
    if (deferred) {
        CHECK(PpuSetOverlayRelocatedOamRange(fast, 0u, 2u));
        CHECK(PpuSetOverlayRelocatedOamRange(reference, 0u, 2u));
    }
    CHECK(PpuSetObjRangeCapture(
        fast, 0u, 2u, 0, 0, kPpuXPixels, kRows,
        (uint8_t *)fast_range, kPpuXPixels * sizeof(uint32_t)));
    CHECK(PpuSetObjRangeCapture(
        reference, 0u, 2u, 0, 0, kPpuXPixels, kRows,
        (uint8_t *)reference_range, kPpuXPixels * sizeof(uint32_t)));
    PpuBeginDrawing(fast, (uint8_t *)fast_pixels,
                    kPpuXPixels * sizeof(uint32_t), 0u);
    PpuBeginDrawing(reference, (uint8_t *)reference_pixels,
                    kPpuXPixels * sizeof(uint32_t),
                    kPpuRenderFlags_ReferencePixelRenderer);
    CHECK(PpuBindAuthenticSurface(
        fast, (uint8_t *)fast_authentic,
        kPpuXPixels * sizeof(uint32_t)));
    CHECK(PpuBindAuthenticSurface(
        reference, (uint8_t *)reference_authentic,
        kPpuXPixels * sizeof(uint32_t)));
    ppu_runLine(fast, 0);
    ppu_runLine(reference, 0);
    for (int line = 1; line <= kRows; ++line) {
        ppu_runLine(fast, line);
        ppu_runLine(reference, line);
    }
    CHECK(memcmp(fast_pixels, reference_pixels,
                 pixel_count * sizeof(uint32_t)) == 0);
    CHECK(memcmp(fast_authentic, reference_authentic,
                 pixel_count * sizeof(uint32_t)) == 0);
    CHECK(memcmp(fast_range, reference_range,
                 pixel_count * sizeof(uint32_t)) == 0);
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        if (fast->overlayRenderContentMask[source] !=
            reference->overlayRenderContentMask[source]) {
            fprintf(stderr,
                "capture mask mismatch deferred=%d source=%d fast=%02x "
                "ref=%02x\n", deferred, source,
                fast->overlayRenderContentMask[source],
                reference->overlayRenderContentMask[source]);
            CHECK(false);
        }
        for (int plane = 0; plane < kPlanes; ++plane) {
            if (memcmp(fast_overlay[source][plane],
                       reference_overlay[source][plane],
                       pixel_count * sizeof(uint32_t)) != 0) {
                for (size_t index = 0; index < pixel_count; ++index) {
                    if (fast_overlay[source][plane][index] !=
                        reference_overlay[source][plane][index]) {
                        fprintf(stderr,
                            "capture mismatch deferred=%d source=%d "
                            "plane=%d x=%zu y=%zu fast=%08x ref=%08x\n",
                            deferred, source, plane,
                            index % kPpuXPixels, index / kPpuXPixels,
                            fast_overlay[source][plane][index],
                            reference_overlay[source][plane][index]);
                        break;
                    }
                }
                CHECK(false);
            }
        }
    }
cleanup:
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        for (int plane = 0; plane < kPlanes; ++plane) {
            free(fast_overlay[source][plane]);
            free(reference_overlay[source][plane]);
        }
    }
    ppu_free(fast);
    ppu_free(reference);
    free(fast_pixels);
    free(reference_pixels);
    free(fast_authentic);
    free(reference_authentic);
    free(fast_range);
    free(reference_range);
}

static void test_native_capture_path_parity(void) {
    compare_native_capture_path(false, 1u, 1u);
    compare_native_capture_path(true, 1u, 1u);
    compare_native_capture_path(false, 7u, 1u);
    compare_native_capture_path(false, 1u, 5u);
    compare_native_capture_path(false, 3u, 16u);
}

static void test_unbound_capture_fails_open(void) {
    enum { kRows = 32 };
    const size_t pixel_count = (size_t)kPpuXPixels * kRows;
    Ppu *baseline = ppu_init();
    Ppu *fast = ppu_init();
    Ppu *reference = ppu_init();
    uint32_t *baseline_pixels = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *fast_pixels = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *reference_pixels = calloc(pixel_count, sizeof(uint32_t));
    CHECK(baseline != NULL && fast != NULL && reference != NULL &&
          baseline_pixels != NULL && fast_pixels != NULL &&
          reference_pixels != NULL);
    if (baseline == NULL || fast == NULL || reference == NULL ||
        baseline_pixels == NULL || fast_pixels == NULL ||
        reference_pixels == NULL) goto cleanup;
    setup_native_fast_fixture(baseline, 1u, 0u, true);
    setup_native_fast_fixture(fast, 1u, 0u, true);
    setup_native_fast_fixture(reference, 1u, 0u, true);
    for (int source = 0; source < kPpuOverlaySource_Count; ++source) {
        CHECK(PpuSetOverlayCapture(
            fast, (PpuOverlaySource)source, 0, 0,
            kPpuXPixels, kRows, kPpuOverlayFlag_RemoveFromGame));
        CHECK(PpuSetOverlayCapture(
            reference, (PpuOverlaySource)source, 0, 0,
            kPpuXPixels, kRows, kPpuOverlayFlag_RemoveFromGame));
    }
    CHECK(PpuSetOverlayOamRange(fast, 0u, 128u));
    CHECK(PpuSetOverlayOamRange(reference, 0u, 128u));
    PpuBeginDrawing(baseline, (uint8_t *)baseline_pixels,
                    kPpuXPixels * sizeof(uint32_t), 0u);
    PpuBeginDrawing(fast, (uint8_t *)fast_pixels,
                    kPpuXPixels * sizeof(uint32_t), 0u);
    PpuBeginDrawing(reference, (uint8_t *)reference_pixels,
                    kPpuXPixels * sizeof(uint32_t),
                    kPpuRenderFlags_ReferencePixelRenderer);
    ppu_runLine(baseline, 0);
    ppu_runLine(fast, 0);
    ppu_runLine(reference, 0);
    for (int line = 1; line <= kRows; ++line) {
        ppu_runLine(baseline, line);
        ppu_runLine(fast, line);
        ppu_runLine(reference, line);
    }
    CHECK(memcmp(baseline_pixels, fast_pixels,
                 pixel_count * sizeof(uint32_t)) == 0);
    CHECK(memcmp(baseline_pixels, reference_pixels,
                 pixel_count * sizeof(uint32_t)) == 0);
cleanup:
    ppu_free(baseline);
    ppu_free(fast);
    ppu_free(reference);
    free(baseline_pixels);
    free(fast_pixels);
    free(reference_pixels);
}

typedef struct VirtualParityFixture {
    uint32_t salt;
    unsigned span_lookups;
    unsigned reverse_span_lookups;
    unsigned gap_span_lookups;
    uint16_t span_entries[kPpuSurfaceWidth / 8 + 2];
} VirtualParityFixture;

static bool virtual_parity_lookup(const void *context, int32_t tile_x,
                                  int32_t tile_y, uint16_t *entry) {
    const VirtualParityFixture *fixture = context;
    uint32_t value = (uint32_t)tile_x * 17u +
        (uint32_t)tile_y * 29u + fixture->salt;
    if ((value % 19u) == 0u) return false;
    *entry = (uint16_t)((value & 0x7fu) |
        (((value >> 3) & 7u) << 10) |
        (((value >> 7) & 7u) << 13));
    return true;
}

static size_t virtual_parity_span_lookup(
        const void *context, int32_t tile_x, int32_t tile_y,
        int32_t tile_step, size_t capacity, const uint16_t **entries,
        ptrdiff_t *entry_step) {
    VirtualParityFixture *fixture = (VirtualParityFixture *)context;
    if (capacity > sizeof(fixture->span_entries) /
            sizeof(fixture->span_entries[0])) return 0u;
    fixture->span_lookups++;
    if (tile_step < 0) fixture->reverse_span_lookups++;
    size_t count = 0u;
    bool first_present = false;
    while (count < capacity) {
        const uint32_t x = (uint32_t)((int64_t)tile_x +
            (int64_t)tile_step * (int64_t)count);
        const uint32_t value = x * 17u +
            (uint32_t)tile_y * 29u + fixture->salt;
        const bool present = (value % 19u) != 0u;
        if (count == 0u) first_present = present;
        else if (present != first_present) break;
        if (present) {
            fixture->span_entries[count] = (uint16_t)((value & 0x7fu) |
                (((value >> 3) & 7u) << 10) |
                (((value >> 7) & 7u) << 13));
        }
        ++count;
    }
    if (!first_present) fixture->gap_span_lookups++;
    *entries = first_present ? fixture->span_entries : NULL;
    *entry_step = first_present ? 1 : 0;
    return count;
}

static bool virtual_parity_band_lookup(const void *context, int32_t tile_x,
                                       int32_t tile_y, uint16_t entry,
                                       uint8_t *band) {
    (void)context;
    *band = (uint8_t)(((uint32_t)tile_x +
        (uint32_t)tile_y * 3u + entry) % 3u);
    return true;
}

typedef struct VirtualFastFixture {
    uint32_t salt;
    unsigned lookups;
    unsigned span_lookups;
    unsigned span_tiles;
    unsigned band_lookups;
    uint16_t span_entries[kPpuSurfaceWidth / 8 + 2];
} VirtualFastFixture;

static bool virtual_fast_lookup(const void *context, int32_t tile_x,
                                int32_t tile_y, uint16_t *entry) {
    VirtualFastFixture *fixture = (VirtualFastFixture *)context;
    uint32_t value = (uint32_t)tile_x * 17u +
        (uint32_t)tile_y * 29u + fixture->salt;
    fixture->lookups++;
    *entry = (uint16_t)((value & 0x7fu) |
        (((value >> 3) & 7u) << 10) |
        (((value >> 7) & 7u) << 13));
    return true;
}

static size_t virtual_fast_span_lookup(
        const void *context, int32_t tile_x, int32_t tile_y,
        int32_t tile_step, size_t capacity, const uint16_t **entries,
        ptrdiff_t *entry_step) {
    VirtualFastFixture *fixture = (VirtualFastFixture *)context;
    if (capacity > sizeof(fixture->span_entries) /
            sizeof(fixture->span_entries[0])) return 0u;
    fixture->span_lookups++;
    fixture->span_tiles += (unsigned)capacity;
    for (size_t i = 0; i < capacity; ++i) {
        uint32_t value =
            (uint32_t)(tile_x + (int32_t)i * tile_step) * 17u +
            (uint32_t)tile_y * 29u + fixture->salt;
        fixture->span_entries[i] = (uint16_t)((value & 0x7fu) |
            (((value >> 3) & 7u) << 10) |
            (((value >> 7) & 7u) << 13));
    }
    *entries = fixture->span_entries;
    *entry_step = 1;
    return capacity;
}

static bool virtual_fast_band_lookup(const void *context, int32_t tile_x,
                                     int32_t tile_y, uint16_t entry,
                                     uint8_t *band) {
    VirtualFastFixture *fixture = (VirtualFastFixture *)context;
    fixture->band_lookups++;
    *band = (uint8_t)(((uint32_t)tile_x +
        (uint32_t)tile_y * 3u + entry) % 3u);
    return true;
}

static void test_native_virtual_fast_path_parity(void) {
    enum { kRows = 48 };
    const size_t pixel_count = (size_t)kPpuXPixels * kRows;
    for (int camera_x = -24; camera_x <= -21; camera_x += 3) {
        Ppu *fast = ppu_init();
        Ppu *reference = ppu_init();
        uint32_t *fast_pixels = calloc(pixel_count, sizeof(uint32_t));
        uint32_t *reference_pixels = calloc(pixel_count, sizeof(uint32_t));
        VirtualFastFixture fast_fixture = {.salt = 0x51a7u};
        VirtualFastFixture reference_fixture = {.salt = 0x51a7u};
        PpuVirtualTilemapBinding fast_binding = {
            .lookup = virtual_fast_lookup,
            .lookup_span = virtual_fast_span_lookup,
            .band_lookup = virtual_fast_band_lookup,
            .context = &fast_fixture,
            .camera_x = camera_x,
            .camera_y = 11,
            .hscroll_anchor = 5u,
            .vscroll_anchor = 9u,
            .flags = kPpuVirtualTilemapFlag_IncludeAuthentic,
        };
        PpuVirtualTilemapBinding reference_binding = fast_binding;
        reference_binding.context = &reference_fixture;
        reference_binding.lookup_span = NULL;
        CHECK(fast != NULL && reference != NULL && fast_pixels != NULL &&
              reference_pixels != NULL);
        if (fast == NULL || reference == NULL || fast_pixels == NULL ||
            reference_pixels == NULL) goto cleanup;
        setup_native_fast_fixture(fast, 1u, 0u, true);
        setup_native_fast_fixture(reference, 1u, 0u, true);
        fast->screenWindowed[0] = fast->screenWindowed[1] = 0u;
        reference->screenWindowed[0] = reference->screenWindowed[1] = 0u;
        CHECK(PpuSetVirtualTilemap(fast, 0u, &fast_binding));
        CHECK(PpuSetVirtualTilemap(reference, 0u, &reference_binding));
        PpuBeginDrawing(fast, (uint8_t *)fast_pixels,
                        kPpuXPixels * sizeof(uint32_t), 0u);
        PpuBeginDrawing(reference, (uint8_t *)reference_pixels,
                        kPpuXPixels * sizeof(uint32_t),
                        kPpuRenderFlags_ReferencePixelRenderer);
        ppu_runLine(fast, 0);
        ppu_runLine(reference, 0);
        for (int line = 1; line <= kRows; ++line) {
            ppu_runLine(fast, line);
            ppu_runLine(reference, line);
        }
        CHECK(memcmp(fast_pixels, reference_pixels,
                     pixel_count * sizeof(uint32_t)) == 0);
        CHECK(fast_fixture.lookups == 0u);
        CHECK(fast_fixture.span_lookups != 0u);
        CHECK(fast_fixture.span_tiles > fast_fixture.span_lookups);
        CHECK(fast_fixture.band_lookups == 0u);
        CHECK(reference_fixture.lookups != 0u);
        CHECK(reference_fixture.band_lookups != 0u);
cleanup:
        ppu_free(fast);
        ppu_free(reference);
        free(fast_pixels);
        free(reference_pixels);
    }
}

static void compare_native_virtual_capture(
        PpuWidescreenBandFill fill, PpuWidescreenMotion motion,
        uint8_t mosaic_size) {
    enum {
        kExtraX = 16,
        kExtraY = 8,
        kWidth = kPpuXPixels + kExtraX * 2,
        kHeight = kPpuYPixels + kExtraY * 2,
        kPlanes = 3
    };
    const size_t pixel_count = (size_t)kWidth * kHeight;
    VirtualParityFixture fixture = {.salt = 0x51a7u};
    PpuVirtualTilemapBinding binding = {
        .lookup = virtual_parity_lookup,
        .lookup_span = virtual_parity_span_lookup,
        .band_lookup = virtual_parity_band_lookup,
        .context = &fixture,
        .camera_x = -23,
        .camera_y = 11,
        .hscroll_anchor = 5u,
        .vscroll_anchor = 9u,
        .flags = kPpuVirtualTilemapFlag_IncludeAuthentic,
    };
    Ppu *fast = ppu_init();
    Ppu *reference = ppu_init();
    uint32_t *fast_pixels = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *reference_pixels = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *fast_overlay[kPlanes] = {0};
    uint32_t *reference_overlay[kPlanes] = {0};
    CHECK(fast != NULL && reference != NULL && fast_pixels != NULL &&
          reference_pixels != NULL);
    if (fast == NULL || reference == NULL || fast_pixels == NULL ||
        reference_pixels == NULL) goto cleanup;
    setup_native_fast_fixture(fast, 1u, 0u, true);
    setup_native_fast_fixture(reference, 1u, 0u, true);
    if (mosaic_size > 1u) {
        fast->mosaic = reference->mosaic =
            (uint8_t)(((mosaic_size - 1u) << 4) | 0x01u);
    }
    PpuSetExtraSpace(fast, kExtraX);
    PpuSetExtraSpace(reference, kExtraX);
    PpuSetExtraVerticalSpace(fast, kExtraY, kExtraY);
    PpuSetExtraVerticalSpace(reference, kExtraY, kExtraY);
    CHECK(PpuSetVirtualTilemap(fast, 0u, &binding));
    CHECK(PpuSetVirtualTilemap(reference, 0u, &binding));
    if (fill == kPpuWidescreenBandFill_Mirror) {
        PpuSetWidescreenLayerMirror(fast, 1u);
        PpuSetWidescreenLayerMirror(reference, 1u);
    } else if (fill == kPpuWidescreenBandFill_Repeat) {
        PpuSetWidescreenLayerRepeat(fast, 1u);
        PpuSetWidescreenLayerRepeat(reference, 1u);
    } else if (fill == kPpuWidescreenBandFill_Clamp) {
        PpuSetWidescreenLayerClamp(fast, 1u);
        PpuSetWidescreenLayerClamp(reference, 1u);
    }
    if (motion == kPpuWidescreenMotion_NormalScroll) {
        PpuSetWidescreenLayerNormalScroll(fast, 1u);
        PpuSetWidescreenLayerNormalScroll(reference, 1u);
    }
    for (int plane = 0; plane < kPlanes; ++plane) {
        fast_overlay[plane] = calloc(pixel_count, sizeof(uint32_t));
        reference_overlay[plane] = calloc(pixel_count, sizeof(uint32_t));
        CHECK(fast_overlay[plane] != NULL &&
              reference_overlay[plane] != NULL);
        if (fast_overlay[plane] == NULL ||
            reference_overlay[plane] == NULL) goto cleanup;
    }
    CHECK(PpuBindOverlaySurface(
        fast, kPpuOverlaySource_Bg1, (uint8_t *)fast_overlay[0],
        kWidth * sizeof(uint32_t)));
    CHECK(PpuBindOverlaySurface(
        reference, kPpuOverlaySource_Bg1,
        (uint8_t *)reference_overlay[0], kWidth * sizeof(uint32_t)));
    for (int plane = 1; plane < kPlanes; ++plane) {
        CHECK(PpuBindOverlayPrioSurface(
            fast, kPpuOverlaySource_Bg1, plane,
            (uint8_t *)fast_overlay[plane]));
        CHECK(PpuBindOverlayPrioSurface(
            reference, kPpuOverlaySource_Bg1, plane,
            (uint8_t *)reference_overlay[plane]));
    }
    CHECK(PpuSetOverlayCapture(
        fast, kPpuOverlaySource_Bg1, -kExtraX, -kExtraY,
        kWidth, kHeight, kPpuOverlayFlag_RemoveFromGame));
    CHECK(PpuSetOverlayCapture(
        reference, kPpuOverlaySource_Bg1, -kExtraX, -kExtraY,
        kWidth, kHeight, kPpuOverlayFlag_RemoveFromGame));
    PpuBeginDrawing(fast, (uint8_t *)fast_pixels,
                    kWidth * sizeof(uint32_t), 0u);
    PpuBeginDrawing(reference, (uint8_t *)reference_pixels,
                    kWidth * sizeof(uint32_t),
                    kPpuRenderFlags_ReferencePixelRenderer);
    ppu_runLine(fast, 0);
    ppu_runLine(reference, 0);
    for (int y = -kExtraY; y < kPpuYPixels + kExtraY; ++y) {
        ppu_runMarginLine(fast, y + 1);
        ppu_runMarginLine(reference, y + 1);
    }
    CHECK(memcmp(fast_pixels, reference_pixels,
                 pixel_count * sizeof(uint32_t)) == 0);
    CHECK(fast->overlayRenderContentMask[kPpuOverlaySource_Bg1] ==
          reference->overlayRenderContentMask[kPpuOverlaySource_Bg1]);
    for (int plane = 0; plane < kPlanes; ++plane) {
        if (memcmp(fast_overlay[plane], reference_overlay[plane],
                   pixel_count * sizeof(uint32_t)) != 0) {
            for (size_t index = 0; index < pixel_count; ++index) {
                if (fast_overlay[plane][index] !=
                    reference_overlay[plane][index]) {
                    fprintf(stderr,
                        "virtual capture mismatch fill=%d motion=%d "
                        "plane=%d x=%zu y=%zu fast=%08x ref=%08x\n",
                        (int)fill, (int)motion, plane, index % kWidth,
                        index / kWidth, fast_overlay[plane][index],
                        reference_overlay[plane][index]);
                    break;
                }
            }
            CHECK(false);
        }
    }
    CHECK(memcmp(&fast->bgBuffers[0], &reference->bgBuffers[0],
                 sizeof(fast->bgBuffers[0])) == 0);
    if (mosaic_size == 1u) {
        CHECK(fixture.span_lookups != 0u);
        CHECK(fixture.gap_span_lookups != 0u);
        if (fill == kPpuWidescreenBandFill_Mirror)
            CHECK(fixture.reverse_span_lookups != 0u);
    } else {
        CHECK(fixture.span_lookups == 0u);
    }
cleanup:
    for (int plane = 0; plane < kPlanes; ++plane) {
        free(fast_overlay[plane]);
        free(reference_overlay[plane]);
    }
    ppu_free(fast);
    ppu_free(reference);
    free(fast_pixels);
    free(reference_pixels);
}

static void test_native_virtual_capture_path_parity(void) {
    compare_native_virtual_capture(
        kPpuWidescreenBandFill_RawWrap,
        kPpuWidescreenMotion_FillRelative, 1u);
    compare_native_virtual_capture(
        kPpuWidescreenBandFill_Mirror,
        kPpuWidescreenMotion_FillRelative, 1u);
    compare_native_virtual_capture(
        kPpuWidescreenBandFill_Mirror,
        kPpuWidescreenMotion_NormalScroll, 1u);
    compare_native_virtual_capture(
        kPpuWidescreenBandFill_Repeat,
        kPpuWidescreenMotion_FillRelative, 1u);
    compare_native_virtual_capture(
        kPpuWidescreenBandFill_Clamp,
        kPpuWidescreenMotion_FillRelative, 1u);
    compare_native_virtual_capture(
        kPpuWidescreenBandFill_RawWrap,
        kPpuWidescreenMotion_FillRelative, 5u);
    compare_native_virtual_capture(
        kPpuWidescreenBandFill_Mirror,
        kPpuWidescreenMotion_NormalScroll, 16u);
}

static void compare_native_vram_margin(
        uint8_t mode, uint8_t big_tiles, PpuWidescreenBandFill fill,
        PpuWidescreenMotion motion, bool finite_extents,
        uint8_t mosaic_size) {
    enum {
        kExtraX = 16,
        kExtraY = 8,
        kWidth = kPpuXPixels + kExtraX * 2,
        kHeight = kPpuYPixels + kExtraY * 2
    };
    const size_t pixel_count = (size_t)kWidth * kHeight;
    Ppu *fast = ppu_init();
    Ppu *reference = ppu_init();
    uint32_t *fast_pixels = calloc(pixel_count, sizeof(uint32_t));
    uint32_t *reference_pixels = calloc(pixel_count, sizeof(uint32_t));
    CHECK(fast != NULL && reference != NULL && fast_pixels != NULL &&
          reference_pixels != NULL);
    if (fast == NULL || reference == NULL || fast_pixels == NULL ||
        reference_pixels == NULL) goto cleanup;
    setup_native_fast_fixture(fast, mode, big_tiles, mode == 1u);
    setup_native_fast_fixture(reference, mode, big_tiles, mode == 1u);
    if (mosaic_size > 1u) {
        fast->mosaic = reference->mosaic =
            (uint8_t)(((mosaic_size - 1u) << 4) | 0x0fu);
    }
    PpuSetExtraSpace(fast, kExtraX);
    PpuSetExtraSpace(reference, kExtraX);
    PpuSetExtraVerticalSpace(fast, kExtraY, kExtraY);
    PpuSetExtraVerticalSpace(reference, kExtraY, kExtraY);
    PpuSetWidescreenBg3Widen(fast, 1u);
    PpuSetWidescreenBg3Widen(reference, 1u);
    if (fill == kPpuWidescreenBandFill_Mirror) {
        PpuSetWidescreenLayerMirror(fast, 0x0fu);
        PpuSetWidescreenLayerMirror(reference, 0x0fu);
    } else if (fill == kPpuWidescreenBandFill_Repeat) {
        PpuSetWidescreenLayerRepeat(fast, 0x0fu);
        PpuSetWidescreenLayerRepeat(reference, 0x0fu);
    } else if (fill == kPpuWidescreenBandFill_Clamp) {
        PpuSetWidescreenLayerClamp(fast, 0x0fu);
        PpuSetWidescreenLayerClamp(reference, 0x0fu);
    }
    if (motion == kPpuWidescreenMotion_NormalScroll) {
        PpuSetWidescreenLayerNormalScroll(fast, 0x0fu);
        PpuSetWidescreenLayerNormalScroll(reference, 0x0fu);
    }
    if (finite_extents) {
        for (uint8_t layer = 0u; layer < 4u; ++layer) {
            PpuSetWidescreenLayerExtent(
                fast, layer, 7u + layer, 11u + layer, 4u, 6u);
            PpuSetWidescreenLayerExtent(
                reference, layer, 7u + layer, 11u + layer, 4u, 6u);
        }
    }
    PpuBeginDrawing(fast, (uint8_t *)fast_pixels,
                    kWidth * sizeof(uint32_t), 0u);
    PpuBeginDrawing(reference, (uint8_t *)reference_pixels,
                    kWidth * sizeof(uint32_t),
                    kPpuRenderFlags_ReferencePixelRenderer);
    ppu_runLine(fast, 0);
    ppu_runLine(reference, 0);
    for (int y = -kExtraY; y < kPpuYPixels + kExtraY; ++y) {
        ppu_runMarginLine(fast, y + 1);
        ppu_runMarginLine(reference, y + 1);
    }
    if (memcmp(fast_pixels, reference_pixels,
               pixel_count * sizeof(uint32_t)) != 0) {
        for (size_t index = 0; index < pixel_count; ++index) {
            if (fast_pixels[index] != reference_pixels[index]) {
                fprintf(stderr,
                    "VRAM margin mismatch mode=%u big=%02x fill=%d "
                    "motion=%d x=%zu y=%zu fast=%08x ref=%08x\n",
                    mode, big_tiles, (int)fill, (int)motion,
                    index % kWidth, index / kWidth,
                    fast_pixels[index], reference_pixels[index]);
                break;
            }
        }
        CHECK(false);
    }
    CHECK(memcmp(&fast->bgBuffers[0], &reference->bgBuffers[0],
                 sizeof(fast->bgBuffers[0])) == 0);
cleanup:
    ppu_free(fast);
    ppu_free(reference);
    free(fast_pixels);
    free(reference_pixels);
}

static void test_native_vram_margin_path_parity(void) {
    compare_native_vram_margin(
        0u, 0u, kPpuWidescreenBandFill_RawWrap,
        kPpuWidescreenMotion_FillRelative, true, 1u);
    compare_native_vram_margin(
        1u, 0u, kPpuWidescreenBandFill_Mirror,
        kPpuWidescreenMotion_FillRelative, false, 1u);
    compare_native_vram_margin(
        1u, 0xf0u, kPpuWidescreenBandFill_Mirror,
        kPpuWidescreenMotion_NormalScroll, false, 1u);
    compare_native_vram_margin(
        3u, 0u, kPpuWidescreenBandFill_Repeat,
        kPpuWidescreenMotion_FillRelative, false, 1u);
    compare_native_vram_margin(
        4u, 0xf0u, kPpuWidescreenBandFill_Clamp,
        kPpuWidescreenMotion_FillRelative, false, 1u);
    compare_native_vram_margin(
        7u, 0u, kPpuWidescreenBandFill_RawWrap,
        kPpuWidescreenMotion_FillRelative, true, 1u);
    compare_native_vram_margin(
        7u, 0x80u, kPpuWidescreenBandFill_Mirror,
        kPpuWidescreenMotion_NormalScroll, false, 1u);
    compare_native_vram_margin(
        1u, 0xf0u, kPpuWidescreenBandFill_Mirror,
        kPpuWidescreenMotion_NormalScroll, false, 5u);
    compare_native_vram_margin(
        3u, 0u, kPpuWidescreenBandFill_Repeat,
        kPpuWidescreenMotion_FillRelative, true, 16u);
}

int main(void) {
    Ppu *ppu = ppu_init();
    CHECK(ppu != NULL);
    if (ppu != NULL) {
        test_register_memory_ports(ppu);
        test_portable_saveload(ppu);
        test_basic_bg_scanout(ppu);
        test_tilemap_screen_wrapping(ppu);
        test_virtual_provider_does_not_own_padding(ppu);
        test_virtual_provider_lookup_is_tile_shaped(ppu);
        test_virtual_provider_vertical_margin_continues_world(ppu);
        test_hud_split_does_not_widen_bg3(ppu);
        test_mode7_hardware_origin(ppu);
        test_native_fast_path_parity();
        test_native_capture_path_parity();
        test_unbound_capture_fails_open();
        test_native_virtual_fast_path_parity();
        test_native_virtual_capture_path_parity();
        test_native_vram_margin_path_parity();
        ppu_free(ppu);
    }
    if (failures != 0) {
        fprintf(stderr, "runtime PPU tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime PPU tests: pass");
    return 0;
}
