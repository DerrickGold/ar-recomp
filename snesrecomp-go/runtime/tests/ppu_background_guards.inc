static bool guard_far_band(const void *context, int32_t x, int32_t y,
        uint16_t entry, uint8_t *band) {
    (void)context; (void)x; (void)y; (void)entry; *band = 0; return 1;
}
static uint32_t guard_overlap_edit(void *context, int32_t x, int32_t y,
        SrPpuCaptureTile *tile) {
    (void)context; (void)x; (void)y;
    *tile = (SrPpuCaptureTile){.band = 2};
    for (unsigned row = 0; row < 8; ++row) {
        tile->black_rows[row] = 0x80;
        tile->transparent_rows[row] = 0x40;
    }
    return 1;
}
static void test_background_guards(void) {
    enum { W = 256, H = 224, ROWS = 16 };
    static uint32_t main_pixels[2][W * H], bands[2][3][W * H];
    static uint32_t saved_main[W * H], saved_bands[3][W * H];
    static uint32_t guarded[W * ROWS + 2];
    static uint32_t independent[3][W * ROWS + 2];
    unsigned overlapping_lanes = 0;
    Ppu *p[2] = {ppu_init(), ppu_init()};
    CHECK(p[0] && p[1]);
    if (!p[0] || !p[1]) goto cleanup;
    /* An ordinary primary capture at the identical world rows is the oracle.
     * Exercise live CHR phases, fades, native high/low, far edits, masks,
     * blank/character replacements and source-owned palette transforms. */
    for (unsigned variant = 0; variant < 20; ++variant) {
        const unsigned phase = variant / 4, edit = variant % 4;
        CaptureTileFixture fixture[2] = {{0}, {0}};
        memset(main_pixels, 0xa5, sizeof(main_pixels));
        memset(bands, 0xa5, sizeof(bands));
        for (unsigned n = 0; n < 2; ++n) {
            ppu_reset(p[n]);
            p[n]->inidisp = (uint8_t)(15 - phase);
            p[n]->bgmode = 1; p[n]->screenEnabled[0] = 1;
            p[n]->fixedColor = 0x0842;
            for (unsigned c = 0; c < 128; ++c)
                p[n]->cgram[c] = (uint16_t)((c * 257 + phase * 71) & 0x7fff);
            for (unsigned tile = 1; tile <= 4; ++tile)
                for (unsigned row = 0; row < 16; ++row)
                    p[n]->vram[tile * 16 + row] = (uint16_t)(0x1357 * (tile + row + phase));
            const PpuVirtualTilemapBinding binding = {.lookup = background_view_lookup,
                .band_lookup = variant >= 16 ? guard_far_band : NULL,
                .camera_x = 128, .camera_y = n ? 72 : 200,
                .flags = kPpuVirtualTilemapFlag_IncludeAuthentic};
            CHECK(PpuSetVirtualTilemap(p[n], 0, &binding));
            CHECK(PpuBindOverlaySurfaceSized(p[n], 0, (uint8_t *)bands[n][0], W * 4, H));
            CHECK(PpuBindOverlayPrioSurface(p[n], 0, 1, (uint8_t *)bands[n][1]));
            CHECK(PpuBindOverlayPrioSurface(p[n], 0, 2, (uint8_t *)bands[n][2]));
            CHECK(PpuSetOverlayCapture(p[n], 0, 0, 0, W, H,
                kPpuOverlayFlag_RemoveFromGame | (phase == 1 ? kPpuOverlayFlag_MarkBgHalfAdd :
                    phase == 2 ? kPpuOverlayFlag_ApplyBgFixedColorSubtract : 0)));
            if (variant & 1)
                CHECK(PpuSetOverlayTransparentFill(p[n], 0, kPpuOverlayTransparentFill_Cgram, 3));
            fixture[n] = (CaptureTileFixture){.masks = edit != 0,
                .complete = edit == 2, .character = edit == 2, .sparse_masks = edit == 3};
            if (edit) p[n]->captureTiles[0] = (SrPpuCaptureTileBinding){
                .lookup = capture_tile_lookup, .user_data = &fixture[n]};
            if (variant >= 16) p[n]->captureTiles[0] = (SrPpuCaptureTileBinding){
                .lookup = guard_overlap_edit};
            CHECK(PpuBeginDrawingSized(p[n], (uint8_t *)main_pixels[n], W * 4, H, 0));
            ppu_runLine(p[n], 0);
        }
        for (int y = 0; y < ROWS; ++y) ppu_runLine(p[1], y + 1);
        memcpy(saved_main, main_pixels[0], sizeof(saved_main));
        memcpy(saved_bands, bands[0], sizeof(saved_bands));
        uint8_t saved_content[sizeof(p[0]->overlayRenderContentMask)];
        memcpy(saved_content, p[0]->overlayRenderContentMask, sizeof(saved_content));
        memset(guarded, 0xa5, sizeof(guarded));
        SrPpuBackgroundGuardRequest guard = {.struct_size = sizeof(guard), .layer = 0,
            .screen_x0 = 0, .screen_y0 = -128, .width = W, .height = ROWS,
            .source_width = 1024, .source_height = 512,
            .pixels = guarded + 1, .pitch_bytes = W * 4, .pixel_byte_size = W * ROWS * 4};
        CHECK(PpuRenderBackgroundGuard(p[0], &guard));
        SrPpuBackgroundGuardBands separate = {.struct_size = sizeof(separate)};
        memset(independent, 0xa5, sizeof(independent));
        for (unsigned b = 0; b < 3; ++b)
            separate.bands[b] = (SrPpuBackgroundGuardBandOutput){.pixels = independent[b] + 1,
                .pitch_bytes = W * 4, .pixel_byte_size = W * ROWS * 4};
        /* Independent outputs must preserve every native/edited lane, not
         * recover priority from the already-reduced legacy union. */
        CHECK(PpuRenderBackgroundGuardBands(p[0], &guard, &separate));
        bool same = true;
        for (unsigned y = 0; y < ROWS; ++y) for (unsigned x = 0; x < W; ++x) {
            const unsigned at = y * W + x;
            uint32_t expected = bands[1][2][at];
            if (bands[1][0][at] >> 24) expected = bands[1][0][at];
            if (bands[1][1][at] >> 24) expected = bands[1][1][at];
            if (guarded[1 + at] != expected) {
                if (same) fprintf(stderr, "guard parity variant=%u x=%u y=%u expected=%08x got=%08x\n",
                    variant, x, y, expected, guarded[1 + at]);
                same = false;
            }
            for (unsigned b = 0; b < 3; ++b)
                CHECK(independent[b][1 + at] == bands[1][b][at]);
            if ((bands[1][0][at] >> 24) && (bands[1][1][at] >> 24) &&
                (bands[1][2][at] >> 24)) ++overlapping_lanes;
        }
        CHECK(same);
        CHECK(guarded[0] == 0xa5a5a5a5 && guarded[W * ROWS + 1] == 0xa5a5a5a5);
        for (unsigned b = 0; b < 3; ++b)
            CHECK(independent[b][0] == 0xa5a5a5a5 && independent[b][W * ROWS + 1] == 0xa5a5a5a5);
        SrPpuBackgroundGuardRequest bands_only = guard;
        bands_only.pixels = NULL; bands_only.pitch_bytes = bands_only.pixel_byte_size = 0;
        CHECK(PpuRenderBackgroundGuardBands(p[0], &bands_only, &separate));
        for (unsigned b = 0; b < 3; ++b)
            CHECK(!memcmp(independent[b] + 1, bands[1][b], W * ROWS * 4));
        CHECK(!memcmp(saved_main, main_pixels[0], sizeof(saved_main)));
        CHECK(!memcmp(saved_bands, bands[0], sizeof(saved_bands)));
        CHECK(!memcmp(saved_content, p[0]->overlayRenderContentMask, sizeof(saved_content)));
        /* Finite clipping overrides even opaque transparent-fill contracts. */
        guard.source_x0 = 140; guard.source_width = 200;
        guard.source_y0 = 77; guard.source_height = 6;
        CHECK(PpuRenderBackgroundGuardBands(p[0], &guard, &separate));
        for (unsigned y = 0; y < ROWS; ++y) for (unsigned x = 0; x < W; ++x)
            if (x < 12 || x >= 212 || y < 4 || y >= 10)
                for (unsigned b = 0; b < 3; ++b) {
                    CHECK(guarded[1 + y * W + x] == 0);
                    CHECK(independent[b][1 + y * W + x] == 0);
                }
        separate.flags = SR_PPU_BACKGROUND_GUARD_BANDS_REQUIRE_ANCHOR_SCROLL;
        CHECK(PpuRenderBackgroundGuardBands(p[0], &guard, &separate));
        for (unsigned axis = 0; axis < 2; ++axis) {
            if (axis) p[0]->vScroll[0] = 1; else p[0]->hScroll[0] = 1;
            CHECK(!PpuRenderBackgroundGuardBands(p[0], &guard, &separate));
            for (unsigned b = 0; b < 3; ++b) for (unsigned at = 0; at < W * ROWS; ++at)
                CHECK(!independent[b][at + 1]);
            separate.flags = 0;
            CHECK(PpuRenderBackgroundGuardBands(p[0], &guard, &separate));
            p[0]->hScroll[0] = p[0]->vScroll[0] = 0;
            separate.flags = SR_PPU_BACKGROUND_GUARD_BANDS_REQUIRE_ANCHOR_SCROLL;
        }
        p[0]->inidisp |= 0x80;
        CHECK(PpuRenderBackgroundGuardBands(p[0], &guard, &separate));
        for (unsigned at = 0; at < W * ROWS; ++at) CHECK(!guarded[at + 1]);
        for (unsigned b = 0; b < 3; ++b) for (unsigned at = 0; at < W * ROWS; ++at)
            CHECK(!independent[b][at + 1]);
        p[0]->inidisp &= 15; p[0]->bgmode = 7;
        CHECK(!PpuRenderBackgroundGuard(p[0], &guard));
    }
    CHECK(overlapping_lanes > 0);
    /* Every output lane must be disjoint from every bound capture category,
     * not just from the main framebuffer or another guard. No pixels are
     * read or changed by this validation matrix. */
    CHECK(PpuBindAuthenticSurfaceSized(p[0], (uint8_t *)saved_main, W * 4, H));
    CHECK(PpuSetObjRangeCapture(p[0], 0, 128, 0, 0, W, ROWS,
        (uint8_t *)saved_bands[0], W * 4));
    CHECK(PpuSetObjWinnerCapture(p[0], 0, 128, 0, 0, W, ROWS,
        (uint8_t *)saved_bands[1], W * 4));
    const void *bound[] = {main_pixels[0], saved_main, saved_bands[0], saved_bands[1],
        bands[0][0], bands[0][1], bands[0][2]};
    SrPpuBackgroundGuardRequest storage = {.struct_size = sizeof(storage), .layer = 0,
        .width = W, .height = ROWS, .pixels = guarded + 1,
        .pitch_bytes = W * 4, .pixel_byte_size = W * ROWS * 4};
    SrPpuBackgroundGuardBands outputs = {.struct_size = sizeof(outputs)};
    for (unsigned b = 0; b < 3; ++b)
        outputs.bands[b] = (SrPpuBackgroundGuardBandOutput){independent[b] + 1, W * 4, W * ROWS * 4};
    CHECK(PpuBackgroundGuardStorageValid(p[0], &storage, &outputs));
    for (unsigned lane = 0; lane < 4; ++lane) for (unsigned target = 0; target < sizeof(bound) / sizeof(*bound); ++target) {
        SrPpuBackgroundGuardRequest bad = storage;
        SrPpuBackgroundGuardBands bad_outputs = outputs;
        if (lane) bad_outputs.bands[lane - 1].pixels = (void *)bound[target];
        else bad.pixels = (void *)bound[target];
        CHECK(!PpuBackgroundGuardStorageValid(p[0], &bad, &bad_outputs));
    }
cleanup:
    ppu_free(p[0]); ppu_free(p[1]);
}
