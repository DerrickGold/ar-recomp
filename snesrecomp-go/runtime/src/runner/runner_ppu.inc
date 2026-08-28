/* Unity-build domain fragment. Included only by runner.c. */
void sr_runner_bind_ppu_owner(Snes *snes, Ppu *ppu, bool enabled) {
    if (!enabled && s_ppu_owner_runner != snes) return;
    s_ppu_owner_runner = enabled ? snes : NULL;
    s_owned_ppu = enabled ? ppu : NULL;
}

static uint8_t ppu_background_bpp(uint8_t mode, unsigned layer,
                                  uint32_t flags) {
    static const uint8_t bpp[7][4] = {
        {2u, 2u, 2u, 2u}, {4u, 4u, 2u, 0u}, {4u, 4u, 0u, 0u},
        {8u, 4u, 0u, 0u}, {8u, 2u, 0u, 0u}, {4u, 2u, 0u, 0u},
        {4u, 0u, 0u, 0u},
    };
    if (layer >= 4u) return 0u;
    if (mode < 7u) return bpp[mode][layer];
    if (mode == 7u && layer == 0u) return 8u;
    if (mode == 7u && layer == 1u &&
        (flags & SR_PPU_STATE_MODE7_EXT_BG) != 0u) return 8u;
    return 0u;
}

static SrResult query_ppu_state(SrRunnerHandle *runner,
                                SrPpuStateSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    unsigned layer;
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_PPU_STATE_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_PPU_STATE_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_PPU_STATE_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    ppu = snes->ppu;
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    out_state->flags =
        (PPU_forcedBlank(ppu) ? SR_PPU_STATE_FORCED_BLANK : 0u) |
        (PPU_bg3priority(ppu) ? SR_PPU_STATE_BG3_PRIORITY : 0u) |
        (PPU_interlace(ppu) ? SR_PPU_STATE_INTERLACE : 0u) |
        (PPU_objInterlace(ppu) ? SR_PPU_STATE_OBJ_INTERLACE : 0u) |
        (PPU_overscan(ppu) ? SR_PPU_STATE_OVERSCAN : 0u) |
        (PPU_pseudoHires(ppu) ? SR_PPU_STATE_PSEUDO_HIRES : 0u) |
        (PPU_m7extBg(ppu) ? SR_PPU_STATE_MODE7_EXT_BG : 0u);
    out_state->display_control = ppu->inidisp;
    out_state->object_select = ppu->obsel;
    out_state->bg_mode_control = ppu->bgmode;
    out_state->mosaic_control = ppu->mosaic;
    out_state->bg_mode = (uint8_t)PPU_mode(ppu);
    out_state->brightness = (uint8_t)PPU_brightness(ppu);
    out_state->main_screen = ppu->screenEnabled[0];
    out_state->sub_screen = ppu->screenEnabled[1];
    out_state->main_windowed = ppu->screenWindowed[0];
    out_state->sub_windowed = ppu->screenWindowed[1];
    out_state->object_size_select = (uint8_t)PPU_objSize(ppu);
    out_state->margin_left = ppu->extraLeftCur;
    out_state->margin_right = ppu->extraRightCur;
    out_state->margin_top = ppu->extraTopCur;
    out_state->margin_bottom = ppu->extraBottomCur;
    out_state->object_tile_base_1_word = (uint32_t)PPU_objTileAdr1(ppu);
    out_state->object_tile_base_2_word = (uint32_t)PPU_objTileAdr2(ppu);
    out_state->object_small_size_pixels =
        kPpuSpriteSizes[out_state->object_size_select][0];
    out_state->object_large_size_pixels =
        kPpuSpriteSizes[out_state->object_size_select][1];
    out_state->renderer_flags =
        PpuAuthenticSurfaceBound(ppu)
            ? SR_PPU_RENDERER_AUTHENTIC_SURFACE_BOUND : 0u;
    for (layer = 0u; layer < 4u; ++layer) {
        SrPpuBackgroundState *background = &out_state->backgrounds[layer];
        background->h_scroll = ppu->hScroll[layer];
        background->v_scroll = ppu->vScroll[layer];
        background->tilemap_base_word =
            (uint16_t)PPU_bgTilemapAdr(ppu, layer);
        background->tile_base_word = (uint16_t)PPU_bgTileAdr(ppu, layer);
        background->tilemap_width_tiles =
            PPU_bgTilemapWider(ppu, layer) ? 64u : 32u;
        background->tilemap_height_tiles =
            PPU_bgTilemapHigher(ppu, layer) ? 64u : 32u;
        background->tile_size_pixels = PPU_bigTiles(ppu, layer) ? 16u : 8u;
        background->bits_per_pixel = ppu_background_bpp(
            out_state->bg_mode, layer, out_state->flags);
    }
    out_state->window_select = ppu->windowsel;
    out_state->window_logic = ppu->wbgobjlog;
    out_state->color_math_control = ppu->cgwsel;
    out_state->color_math_designation = ppu->cgadsub;
    for (layer = 0u; layer < 4u; ++layer)
        out_state->background_tilemap_control[layer] = ppu->bgXsc[layer];
    out_state->background_tile_base_control = ppu->bgTileAdr;
    out_state->mode7_select = ppu->m7sel;
    memcpy(out_state->mode7_matrix, ppu->m7matrix,
           sizeof(out_state->mode7_matrix));
    out_state->fixed_color = ppu->fixedColor;
    out_state->window1_left = ppu->window1left;
    out_state->window1_right = ppu->window1right;
    out_state->window2_left = ppu->window2left;
    out_state->window2_right = ppu->window2right;
    out_state->setini_control = ppu->setini;
    out_state->layer_clamp_mask = ppu->wsLayerClamp;
    out_state->layer_mirror_mask = ppu->wsLayerMirror;
    out_state->layer_repeat_mask = ppu->wsLayerRepeat;
    out_state->layer_normal_scroll_mask = ppu->wsLayerNormalScroll;
    out_state->oam_address_low = ppu->oamaddl;
    out_state->oam_address_high = ppu->oamaddh;
    out_state->object_priority_rotation = PPU_objPriority(ppu) ? 1u : 0u;
    return SR_RESULT_OK;
}

static SrResult query_dma_state(SrRunnerHandle *runner,
                                SrDmaStateSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    Dma *dma;
    unsigned channel;
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_DMA_STATE_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_DMA_STATE_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_DMA_STATE_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    dma = snes->dma;
    if (dma == NULL) return SR_RESULT_UNAVAILABLE;
    out_state->flags = dma->dmaBusy ? SR_DMA_STATE_BUSY : 0u;
    out_state->timer = dma->dmaTimer;
    out_state->channel_count = SR_DMA_CHANNEL_COUNT;
    _Static_assert(SR_DMA_CHANNEL_COUNT == kDmaChannelCount,
                   "public ABI DMA channel count must match the runner");
    for (channel = 0u; channel < SR_DMA_CHANNEL_COUNT; ++channel) {
        const DmaChannel *source = &dma->channel[channel];
        SrDmaChannelState *target = &out_state->channels[channel];
        target->flags =
            (source->dmaActive ? SR_DMA_CHANNEL_DMA_ACTIVE : 0u) |
            (source->hdmaActive ? SR_DMA_CHANNEL_HDMA_ACTIVE : 0u) |
            (source->fixed ? SR_DMA_CHANNEL_FIXED_A_BUS : 0u) |
            (source->decrement ? SR_DMA_CHANNEL_DECREMENT_A_BUS : 0u) |
            (source->indirect ? SR_DMA_CHANNEL_INDIRECT : 0u) |
            (source->fromB ? SR_DMA_CHANNEL_FROM_B_BUS : 0u) |
            (source->doTransfer ? SR_DMA_CHANNEL_TRANSFER_PENDING : 0u) |
            (source->terminated ? SR_DMA_CHANNEL_TERMINATED : 0u);
        target->a_address = source->aAdr;
        target->transfer_size = source->size;
        target->table_address = source->tableAdr;
        target->a_bank = source->aBank;
        target->b_address = source->bAdr;
        target->indirect_bank = source->indBank;
        target->mode = source->mode;
        target->repeat_count = source->repCount;
    }
    return SR_RESULT_OK;
}

static SrResult resolve_ppu_background_coordinate(
        SrRunnerHandle *runner,
        const SrPpuBackgroundCoordinateRequest *request,
        SrPpuBackgroundCoordinateResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    PpuWidescreenLayerPolicy policy;
    int source_x = 0;
    int sample_y = 0;
    bool mapped;
    bool mosaic = false;
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_BACKGROUND_COORDINATE_REQUEST_V2_SIZE ||
        out_result->struct_size <
            SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE ||
        request->flags != 0u || request->layer >= 4u ||
        request->screen_x < -(int32_t)SR_PPU_HORIZONTAL_MARGIN_MAX ||
        request->screen_x >= (int32_t)(SR_PPU_NATIVE_WIDTH +
                                       SR_PPU_HORIZONTAL_MARGIN_MAX) ||
        request->screen_y < -(int32_t)SR_PPU_VERTICAL_MARGIN_MAX ||
        request->screen_y >= (int32_t)(SR_PPU_NATIVE_HEIGHT +
                                       SR_PPU_VERTICAL_MARGIN_MAX))
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE);
    out_result->struct_size =
        SR_PPU_BACKGROUND_COORDINATE_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (snes->ppu == NULL) return SR_RESULT_UNAVAILABLE;
    mapped = PpuResolveBackgroundCoordinate(
        snes->ppu, (uint8_t)request->layer, request->screen_x,
        request->screen_y, &source_x, &sample_y, &policy, &mosaic);
    out_result->flags =
        (mapped ? SR_PPU_BACKGROUND_COORDINATE_MAPPED : 0u) |
        (policy.band_override
             ? SR_PPU_BACKGROUND_COORDINATE_BAND_OVERRIDE : 0u) |
        (mosaic ? SR_PPU_BACKGROUND_COORDINATE_MOSAIC : 0u);
    out_result->source_x = source_x;
    out_result->sample_y = sample_y;
    out_result->fill = (SrPpuBackgroundFill)policy.fill;
    out_result->motion = (SrPpuBackgroundMotion)policy.motion;
    return SR_RESULT_OK;
}

static uint8_t ppu_frame_color_component(uint16_t value,
                                         uint8_t brightness) {
    uint32_t expanded = ((uint32_t)value << 3) | ((uint32_t)value >> 2);
    return (uint8_t)((expanded * brightness) / 15u);
}

static uint32_t ppu_frame_color_argb(const Ppu *ppu, uint16_t color) {
    uint8_t brightness = (uint8_t)PPU_brightness(ppu);
    return UINT32_C(0xff000000) |
        ((uint32_t)ppu_frame_color_component(color & 0x1fu, brightness) << 16) |
        ((uint32_t)ppu_frame_color_component((color >> 5) & 0x1fu,
                                             brightness) << 8) |
        ppu_frame_color_component((color >> 10) & 0x1fu, brightness);
}

static uint32_t ppu_overlay_content_mask(const Ppu *ppu, unsigned source) {
    uint32_t mask = 0u;
    unsigned band;
    if (ppu->overlayRenderBuffer[source] != NULL &&
        (ppu->overlayRenderContentMask[source] & 1u) != 0u)
        mask |= 1u;
    for (band = 1u; band <= 3u; ++band) {
        if (ppu->overlayRenderBands[source][band - 1u] != NULL &&
            (ppu->overlayRenderContentMask[source] & (1u << band)) != 0u)
            mask |= 1u << band;
    }
    return mask;
}

static uint32_t ppu_overlay_fill_argb(
        const Ppu *ppu, const PpuOverlayCapture *capture) {
    if (capture->transparentFillMode == kPpuOverlayTransparentFill_Black)
        return UINT32_C(0xff000000);
    if (capture->transparentFillMode == kPpuOverlayTransparentFill_Cgram)
        return ppu_frame_color_argb(
            ppu, ppu->cgram[capture->transparentFillCgram]);
    return 0u;
}

static SrResult query_ppu_frame_state(SrRunnerHandle *runner,
                                      SrPpuFrameSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    unsigned source;
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_PPU_FRAME_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_PPU_FRAME_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_PPU_FRAME_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    ppu = snes->ppu;
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    _Static_assert(SR_PPU_OVERLAY_SOURCE_COUNT == kPpuOverlaySource_Count,
                   "ABI overlay count must match the PPU");
    _Static_assert(SR_PPU_OVERLAY_REMOVE_FROM_GAME ==
                       kPpuOverlayFlag_RemoveFromGame &&
                   SR_PPU_OVERLAY_MARK_OBJ_COLOR_MATH ==
                       kPpuOverlayFlag_MarkObjColorMath &&
                   SR_PPU_OVERLAY_MARK_BG_HALF_ADD ==
                       kPpuOverlayFlag_MarkBgHalfAdd &&
                   SR_PPU_OVERLAY_APPLY_BG_FIXED_COLOR_SUBTRACT ==
                       kPpuOverlayFlag_ApplyBgFixedColorSubtract &&
                   SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN ==
                       kPpuOverlayFlag_MarkFullAddSubscreen &&
                   SR_PPU_OVERLAY_MARK_MAIN_SCREEN_WINNER ==
                       kPpuOverlayFlag_MarkMainScreenWinner &&
                   SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER ==
                       kPpuOverlayFlag_MarkOwningScreenWinner,
                   "ABI overlay flags must match the PPU");
    out_state->display_control = ppu->inidisp;
    out_state->bg_mode = (uint8_t)PPU_mode(ppu);
    out_state->hud_split_height = ppu->wsHudSplitHeight;
    out_state->hud_left_end = ppu->wsHudLeftEnd;
    out_state->hud_right_start = ppu->wsHudRightStart;
    out_state->hud_player_row_y = ppu->wsHudPlayerRowY;
    out_state->hud_left_only_y = ppu->wsHudLeftOnlyY;
    out_state->margin_budget = ppu->extraLeftRight;
    out_state->mode7_override_active = ppu->m7Override.rgba != NULL ? 1u : 0u;
    out_state->overlay_count = SR_PPU_OVERLAY_SOURCE_COUNT;
    for (source = 0u; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
        const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
        SrPpuOverlayState *overlay = &out_state->overlays[source];
        overlay->x0 = capture->x0;
        overlay->x1 = capture->x1;
        overlay->y0 = capture->y0;
        overlay->y1 = capture->y1;
        overlay->flags = capture->flags;
        overlay->content_band_mask = ppu_overlay_content_mask(ppu, source);
        overlay->transparent_fill_argb =
            ppu_overlay_fill_argb(ppu, capture);
        overlay->transparent_fill_configured =
            capture->transparentFillConfigured != 0u ? 1u : 0u;
        overlay->transparent_fill_mode = capture->transparentFillMode;
        overlay->transparent_fill_cgram = capture->transparentFillCgram;
        overlay->oam_first = capture->oamFirst;
        overlay->oam_count = capture->oamCount;
    }
    return SR_RESULT_OK;
}

static void ppu_surface_view_init(SrPpuSurfaceView *view,
                                  const uint8_t *data, uint32_t pitch,
                                  uint32_t height, int32_t origin_x,
                                  int32_t origin_y, uint32_t scale,
                                  bool has_content) {
    if (view == NULL || data == NULL || pitch == 0u ||
        pitch % sizeof(uint32_t) != 0u || height == 0u) return;
    view->flags = SR_PPU_SURFACE_BOUND |
        (has_content ? SR_PPU_SURFACE_HAS_CONTENT : 0u);
    view->pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32;
    view->data = data;
    view->pitch_bytes = pitch;
    view->byte_size = (uint64_t)pitch * height;
    view->width_pixels = pitch / (uint32_t)sizeof(uint32_t);
    view->height_pixels = height;
    view->origin_x = origin_x;
    view->origin_y = origin_y;
    view->scale = scale;
}

static uint32_t ppu_overlay_surface_height(
        const PpuOverlayCapture *capture) {
    int height;
    if (capture == NULL || capture->x1 <= capture->x0 ||
        capture->y1 <= capture->y0) return 0u;
    height = capture->y0 < 0 ? capture->y1 - capture->y0 : capture->y1;
    if (height <= 0) return 0u;
    if (height > kPpuBufHeight) height = kPpuBufHeight;
    return (uint32_t)height;
}

static SrResult query_ppu_surfaces(
        SrRunnerHandle *runner, SrPpuSurfaceSnapshot *out_surfaces) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    uint32_t rendered_height;
    unsigned source, band;
    if (snes == NULL || out_surfaces == NULL ||
        out_surfaces->struct_size < SR_PPU_SURFACE_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_surfaces, 0, SR_PPU_SURFACE_SNAPSHOT_V2_SIZE);
    out_surfaces->struct_size = SR_PPU_SURFACE_SNAPSHOT_V2_SIZE;
    out_surfaces->lifetime_generation = snes->abiLifetimeGeneration;
    out_surfaces->overlay_count = SR_PPU_OVERLAY_SOURCE_COUNT;
    out_surfaces->band_count = SR_PPU_SURFACE_BAND_COUNT;
    ppu = snes->ppu;
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    out_surfaces->binding_generation = ppu->surfaceBindingGeneration;
    rendered_height = (uint32_t)PpuRenderedHeight(ppu);
    ppu_surface_view_init(
        &out_surfaces->main, ppu->renderBuffer, ppu->renderPitch,
        rendered_height,
        PpuSurfaceApron(ppu, ppu->renderPitch) + ppu->extraLeftRight,
        PpuVerticalOrigin(ppu), 1u, true);
    ppu_surface_view_init(
        &out_surfaces->authentic, ppu->authenticRenderBuffer,
        ppu->authenticRenderPitch, rendered_height,
        PpuSurfaceApron(ppu, ppu->authenticRenderPitch) +
            ppu->extraLeftRight,
        PpuVerticalOrigin(ppu), 1u, true);
    for (source = 0u; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
        const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
        uint32_t height = ppu->overlayRenderHeight[source] != 0u
            ? ppu->overlayRenderHeight[source]
            : ppu_overlay_surface_height(capture);
        int32_t origin_y = capture->y0 < 0 ? -capture->y0 : 0;
        ppu_surface_view_init(
            &out_surfaces->overlays[source][0],
            ppu->overlayRenderBuffer[source],
            ppu->overlayRenderPitch[source], height,
            PpuSurfaceApron(ppu, ppu->overlayRenderPitch[source]) +
                ppu->extraLeftRight,
            origin_y, 1u,
            (ppu->overlayRenderContentMask[source] & 1u) != 0u);
        for (band = 1u; band < SR_PPU_SURFACE_BAND_COUNT; ++band)
            ppu_surface_view_init(
                &out_surfaces->overlays[source][band],
                ppu->overlayRenderBands[source][band - 1u],
                ppu->overlayRenderPitch[source], height,
                PpuSurfaceApron(ppu, ppu->overlayRenderPitch[source]) +
                    ppu->extraLeftRight,
                origin_y, 1u,
                (ppu->overlayRenderContentMask[source] & (1u << band)) != 0u);
    }
    if (ppu->m7OverlayBuffer != NULL && ppu->m7OverlayScale != 0u) {
        uint32_t scale = ppu->m7OverlayScale;
        int32_t width = (int32_t)(ppu->m7OverlayPitch / sizeof(uint32_t));
        int32_t span = (kPpuXPixels + 2 * ppu->extraLeftRight) *
            (int32_t)scale;
        int32_t apron = (width - span) / 2;
        if (apron < 0) apron = 0;
        ppu_surface_view_init(
            &out_surfaces->mode7, ppu->m7OverlayBuffer,
            ppu->m7OverlayPitch, rendered_height * scale,
            apron + ppu->extraLeftRight * (int32_t)scale,
            PpuVerticalOrigin(ppu) * (int32_t)scale, scale,
            ppu->m7Override.rgba != NULL);
    }
    return SR_RESULT_OK;
}

static uint32_t ppu_surface_snapshot_is_valid(
        SrRunnerHandle *runner, const SrPpuSurfaceSnapshot *surfaces) {
    Snes *snes = runner_from_handle(runner);
    return snes != NULL && snes->ppu != NULL && surfaces != NULL &&
        surfaces->struct_size >= SR_PPU_SURFACE_SNAPSHOT_V2_SIZE &&
        surfaces->lifetime_generation == snes->abiLifetimeGeneration &&
        surfaces->binding_generation ==
            snes->ppu->surfaceBindingGeneration;
}

static Ppu *ppu_output_control_target(Snes *snes) {
    if (snes == NULL || snes->ppu == NULL || s_ppu_owner_runner != snes ||
        s_owned_ppu != snes->ppu) return NULL;
    return snes->ppu;
}

static void ppu_writable_surface_view_init(
        SrPpuWritableSurfaceView *view, uint8_t *data, uint32_t pitch,
        uint32_t height, int32_t origin_x, int32_t origin_y,
        bool has_content) {
    if (view == NULL || data == NULL || pitch == 0u || height == 0u ||
        pitch % sizeof(uint32_t) != 0u) return;
    view->flags = SR_PPU_SURFACE_BOUND |
        (has_content ? SR_PPU_SURFACE_HAS_CONTENT : 0u);
    view->pixel_format = SR_PPU_PIXEL_FORMAT_ARGB8888_U32;
    view->data = data;
    view->byte_size = (uint64_t)pitch * height;
    view->pitch_bytes = pitch;
    view->width_pixels = pitch / (uint32_t)sizeof(uint32_t);
    view->height_pixels = height;
    view->origin_x = origin_x;
    view->origin_y = origin_y;
    view->scale = 1u;
}

static SrResult visit_ppu_frame_transaction(
        SrRunnerHandle *runner,
        const SrPpuFrameTransactionRequest *request) {
    Snes *snes = runner_from_handle(runner);
    SrPpuFrameTransactionContext context;
    Ppu *ppu;
    SrResult result;
    unsigned source;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_FRAME_TRANSACTION_REQUEST_V2_SIZE ||
        request->flags != 0u || request->callback == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;

    memset(&context, 0, sizeof(context));
    context.struct_size = SR_PPU_FRAME_TRANSACTION_CONTEXT_V2_SIZE;
    context.lifetime_generation = snes->abiLifetimeGeneration;
    context.state.struct_size = SR_PPU_STATE_SNAPSHOT_V2_SIZE;
    context.frame.struct_size = SR_PPU_FRAME_SNAPSHOT_V2_SIZE;
    context.vram.struct_size = SR_BORROWED_U16_SPAN_V2_SIZE;
    context.cgram.struct_size = SR_BORROWED_U16_SPAN_V2_SIZE;
    context.oam.struct_size = SR_BORROWED_U16_SPAN_V2_SIZE;
    context.high_oam.struct_size = SR_BORROWED_SPAN_V2_SIZE;
    result = query_ppu_state(runner, &context.state);
    if (result != SR_RESULT_OK) return result;
    result = query_ppu_frame_state(runner, &context.frame);
    if (result != SR_RESULT_OK) return result;
    result = borrow_u16_memory(runner, SR_MEMORY_VRAM, &context.vram);
    if (result != SR_RESULT_OK) return result;
    result = borrow_u16_memory(runner, SR_MEMORY_CGRAM, &context.cgram);
    if (result != SR_RESULT_OK) return result;
    result = borrow_u16_memory(runner, SR_MEMORY_OAM, &context.oam);
    if (result != SR_RESULT_OK) return result;
    result = borrow_memory(runner, SR_MEMORY_HIGH_OAM, &context.high_oam);
    if (result != SR_RESULT_OK) return result;
    ppu_writable_surface_view_init(
        &context.main, ppu->renderBuffer, ppu->renderPitch,
        (uint32_t)PpuRenderedHeight(ppu),
        PpuSurfaceApron(ppu, ppu->renderPitch) + ppu->extraLeftRight,
        PpuVerticalOrigin(ppu), true);
    ppu_writable_surface_view_init(
        &context.authentic, ppu->authenticRenderBuffer,
        ppu->authenticRenderPitch, (uint32_t)PpuRenderedHeight(ppu),
        PpuSurfaceApron(ppu, ppu->authenticRenderPitch) +
            ppu->extraLeftRight,
        PpuVerticalOrigin(ppu), true);
    for (source = 0u; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
        const PpuOverlayCapture *capture = &ppu->overlayCaptures[source];
        uint32_t height = ppu->overlayRenderHeight[source] != 0u
            ? ppu->overlayRenderHeight[source]
            : ppu_overlay_surface_height(capture);
        int32_t origin_y = capture->y0 < 0 ? -capture->y0 : 0;
        ppu_writable_surface_view_init(
            &context.overlays[source], ppu->overlayRenderBuffer[source],
            ppu->overlayRenderPitch[source], height,
            PpuSurfaceApron(ppu, ppu->overlayRenderPitch[source]) +
                ppu->extraLeftRight,
            origin_y,
            (ppu->overlayRenderContentMask[source] & 1u) != 0u);
    }
    return request->callback(request->user_data, runner, &context);
}

static SrResult reset_ppu_frame_state(
        SrRunnerHandle *runner, const SrPpuFrameResetRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_FRAME_RESET_REQUEST_V2_SIZE ||
        request->flags != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    PpuClearOverlayCaptures(ppu);
    return SR_RESULT_OK;
}

static SrResult configure_ppu_obj_capture(
        SrRunnerHandle *runner, const SrPpuObjCaptureRequest *request) {
    Snes *snes = runner_from_handle(runner);
    const uint32_t supported = SR_PPU_OBJ_CAPTURE_RANGE |
        SR_PPU_OBJ_CAPTURE_RELOCATED;
    Ppu *ppu;
    bool range_clear = false;
    bool relocated_clear = false;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_OBJ_CAPTURE_REQUEST_V2_SIZE ||
        request->flags == 0u || (request->flags & ~supported) != 0u ||
        request->reserved[0] != 0u || request->reserved[1] != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;

    if ((request->flags & SR_PPU_OBJ_CAPTURE_RANGE) != 0u) {
        range_clear = request->range_count == 0u;
        if (range_clear) {
            if (request->range_first != 0u || request->range_x != 0 ||
                request->range_y != 0 || request->range_width != 0u ||
                request->range_height != 0u ||
                request->range_pixels != NULL ||
                request->range_pixel_byte_size != 0u ||
                request->range_pitch_bytes != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
        } else {
            uint64_t required;
            if (request->range_first >= 128u ||
                request->range_count > 128u - request->range_first ||
                request->range_width == 0u ||
                request->range_height == 0u || request->range_y < 0 ||
                request->range_y + (uint64_t)request->range_height >
                    SR_PPU_NATIVE_HEIGHT ||
                request->range_x < INT16_MIN ||
                request->range_x + (int64_t)request->range_width > INT16_MAX ||
                request->range_pixels == NULL ||
                request->range_pitch_bytes == 0u ||
                request->range_pitch_bytes > UINT32_MAX ||
                request->range_pitch_bytes % sizeof(uint32_t) != 0u ||
                request->range_pitch_bytes / sizeof(uint32_t) <
                    SR_PPU_NATIVE_WIDTH ||
                request->range_pitch_bytes / sizeof(uint32_t) >
                    SR_PPU_SURFACE_MAX_WIDTH)
                return SR_RESULT_INVALID_ARGUMENT;
            required = request->range_pitch_bytes *
                ((uint64_t)request->range_y + request->range_height);
            if (request->range_pixel_byte_size < required)
                return SR_RESULT_INVALID_ARGUMENT;
        }
    }
    if ((request->flags & SR_PPU_OBJ_CAPTURE_RELOCATED) != 0u) {
        const PpuOverlayCapture *capture =
            &ppu->overlayCaptures[kPpuOverlaySource_Obj];
        relocated_clear = request->relocated_count == 0u;
        if (relocated_clear) {
            if (request->relocated_first != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
        } else if (request->relocated_first >= 128u ||
                   request->relocated_count >
                       128u - request->relocated_first ||
                   (capture->flags &
                    kPpuOverlayFlag_MarkFullAddSubscreen) == 0u ||
                   capture->oamCount == 0u ||
                   request->relocated_first < capture->oamFirst ||
                   request->relocated_first + request->relocated_count >
                       capture->oamFirst + capture->oamCount) {
            return SR_RESULT_INVALID_ARGUMENT;
        }
    }

    if ((request->flags & SR_PPU_OBJ_CAPTURE_RANGE) != 0u) {
        if (range_clear) {
            memset(&ppu->objRangeCapture, 0, sizeof(ppu->objRangeCapture));
        } else if (!PpuSetObjRangeCapture(
                       ppu, (uint8_t)request->range_first,
                       (uint8_t)request->range_count, request->range_x,
                       request->range_y, (int)request->range_width,
                       (int)request->range_height, request->range_pixels,
                       (size_t)request->range_pitch_bytes)) {
            return SR_RESULT_INVALID_ARGUMENT;
        }
    }
    if ((request->flags & SR_PPU_OBJ_CAPTURE_RELOCATED) != 0u) {
        if (relocated_clear) {
            ppu->overlayObjRelocatedFirst = 0u;
            ppu->overlayObjRelocatedCount = 0u;
        } else if (!PpuSetOverlayRelocatedOamRange(
                       ppu, (uint8_t)request->relocated_first,
                       (uint8_t)request->relocated_count)) {
            return SR_RESULT_INVALID_ARGUMENT;
        }
    }
    return SR_RESULT_OK;
}

static bool ppu_capture_state_valid(
        const SrPpuOverlayCaptureState *state, unsigned source) {
    bool empty;
    if (state == NULL || source >= SR_PPU_OVERLAY_SOURCE_COUNT)
        return false;
    empty = state->x0 == 0 && state->x1 == 0 &&
        state->y0 == 0 && state->y1 == 0;
    if (
        (state->flags & ~SR_PPU_OVERLAY_FLAGS_SUPPORTED) != 0u ||
        state->transparent_fill_configured > 1u ||
        state->transparent_fill_mode > SR_PPU_TRANSPARENT_FILL_CGRAM ||
        state->reserved8[0] != 0u || state->reserved8[1] != 0u ||
        state->reserved8[2] != 0u)
        return false;
    if (!empty &&
        (state->x1 <= state->x0 || state->y1 <= state->y0 ||
         state->x0 < -(int32_t)SR_PPU_HORIZONTAL_MARGIN_MAX ||
         state->x1 > (int32_t)(SR_PPU_NATIVE_WIDTH +
                               SR_PPU_HORIZONTAL_MARGIN_MAX) ||
         state->y0 < -(int32_t)SR_PPU_VERTICAL_MARGIN_MAX ||
         state->y1 > (int32_t)(SR_PPU_NATIVE_HEIGHT +
                               SR_PPU_VERTICAL_MARGIN_MAX)))
        return false;
    if (state->oam_count == 0u)
        return state->oam_first == 0u;
    return !empty && source == SR_PPU_OVERLAY_OBJ &&
        state->oam_first < 128u && state->oam_count <=
            128u - state->oam_first;
}

static void ppu_capture_state_from_internal(
        SrPpuOverlayCaptureState *state,
        const PpuOverlayCapture *capture) {
    memset(state, 0, sizeof(*state));
    state->x0 = capture->x0;
    state->x1 = capture->x1;
    state->y0 = capture->y0;
    state->y1 = capture->y1;
    state->flags = capture->flags;
    state->transparent_fill_configured =
        capture->transparentFillConfigured != 0u ? 1u : 0u;
    state->transparent_fill_mode = capture->transparentFillMode;
    state->transparent_fill_cgram = capture->transparentFillCgram;
    state->oam_first = capture->oamFirst;
    state->oam_count = capture->oamCount;
}

static bool ppu_capture_state_equal(
        const SrPpuOverlayCaptureState *left,
        const SrPpuOverlayCaptureState *right) {
    return left->x0 == right->x0 && left->x1 == right->x1 &&
        left->y0 == right->y0 && left->y1 == right->y1 &&
        left->flags == right->flags &&
        left->transparent_fill_configured ==
            right->transparent_fill_configured &&
        left->transparent_fill_mode == right->transparent_fill_mode &&
        left->transparent_fill_cgram == right->transparent_fill_cgram &&
        left->oam_first == right->oam_first &&
        left->oam_count == right->oam_count;
}

static void ppu_capture_state_apply(
        PpuOverlayCapture *capture,
        const SrPpuOverlayCaptureState *state) {
    capture->x0 = state->x0;
    capture->x1 = state->x1;
    capture->y0 = state->y0;
    capture->y1 = state->y1;
    capture->flags = (uint8_t)state->flags;
    capture->transparentFillConfigured =
        state->transparent_fill_configured;
    capture->transparentFillMode = state->transparent_fill_mode;
    capture->transparentFillCgram = state->transparent_fill_cgram;
    capture->oamFirst = state->oam_first;
    capture->oamCount = state->oam_count;
}

static SrResult compare_exchange_ppu_overlay_captures(
        SrRunnerHandle *runner,
        const SrPpuOverlayCaptureExchangeRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    const uint32_t valid_sources =
        (UINT32_C(1) << SR_PPU_OVERLAY_SOURCE_COUNT) - 1u;
    unsigned source;
    if (snes == NULL || request == NULL ||
        request->struct_size <
            SR_PPU_OVERLAY_CAPTURE_EXCHANGE_REQUEST_V2_SIZE ||
        request->flags != 0u || request->source_mask == 0u ||
        (request->source_mask & ~valid_sources) != 0u ||
        request->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    _Static_assert(SR_PPU_TRANSPARENT_FILL_NONE ==
                       kPpuOverlayTransparentFill_None &&
                   SR_PPU_TRANSPARENT_FILL_BLACK ==
                       kPpuOverlayTransparentFill_Black &&
                   SR_PPU_TRANSPARENT_FILL_CGRAM ==
                       kPpuOverlayTransparentFill_Cgram,
                   "ABI transparent-fill values must match the PPU");
    for (source = 0u; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
        SrPpuOverlayCaptureState current;
        if ((request->source_mask & (UINT32_C(1) << source)) == 0u)
            continue;
        if (!ppu_capture_state_valid(&request->expected[source], source) ||
            !ppu_capture_state_valid(&request->replacement[source], source))
            return SR_RESULT_INVALID_ARGUMENT;
        ppu_capture_state_from_internal(
            &current, &ppu->overlayCaptures[source]);
        if (!ppu_capture_state_equal(&current, &request->expected[source]))
            return SR_RESULT_BUSY;
    }
    for (source = 0u; source < SR_PPU_OVERLAY_SOURCE_COUNT; ++source) {
        if ((request->source_mask & (UINT32_C(1) << source)) != 0u)
            ppu_capture_state_apply(
                &ppu->overlayCaptures[source],
                &request->replacement[source]);
    }
    return SR_RESULT_OK;
}

static SrResult compare_exchange_ppu_vram_words(
        SrRunnerHandle *runner,
        const SrPpuVramPatchRequest *request) {
    Snes *snes = runner_from_handle(runner);
    uint8_t seen[SR_PPU_VRAM_WORD_COUNT / 8u];
    Ppu *ppu;
    uint32_t index;
    uint32_t previous_address = 0u;
    bool addresses_sorted;
    bool changed = false;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_VRAM_PATCH_REQUEST_V2_SIZE ||
        (request->flags & ~SR_PPU_VRAM_PATCH_ADDRESSES_SORTED) != 0u ||
        request->patches == NULL ||
        request->patch_count == 0u ||
        request->patch_count > SR_PPU_VRAM_PATCH_MAX_WORDS ||
        request->reserved != 0u ||
        ((uintptr_t)request->patches %
         _Alignof(SrPpuVramWordPatch)) != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;

    addresses_sorted =
        (request->flags & SR_PPU_VRAM_PATCH_ADDRESSES_SORTED) != 0u;
    if (!addresses_sorted) memset(seen, 0, sizeof(seen));
    for (index = 0u; index < request->patch_count; ++index) {
        const SrPpuVramWordPatch *patch = &request->patches[index];
        uint32_t address = patch->word_address;
        uint32_t byte = address >> 3;
        uint8_t bit = (uint8_t)(1u << (address & 7u));
        if (address >= SR_PPU_VRAM_WORD_COUNT || patch->reserved != 0u)
            return SR_RESULT_INVALID_ARGUMENT;
        if (addresses_sorted) {
            if (index != 0u && address <= previous_address)
                return SR_RESULT_INVALID_ARGUMENT;
            previous_address = address;
        } else {
            if ((seen[byte] & bit) != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            seen[byte] |= bit;
        }
        if (ppu->vram[address] != patch->expected)
            return SR_RESULT_BUSY;
        changed |= patch->expected != patch->replacement;
    }
    if (!changed) return SR_RESULT_OK;

    sr_runner_note_mutation(snes);
    for (index = 0u; index < request->patch_count; ++index) {
        const SrPpuVramWordPatch *patch = &request->patches[index];
        uint32_t byte_address = (uint32_t)patch->word_address * 2u;
        if (patch->expected == patch->replacement) continue;
        ppu->vram[patch->word_address] = patch->replacement;
        if (sr_runner_event_enabled(SR_EVENT_MASK_MEMORY_WRITE)) {
            sr_runner_emit_memory_write(
                snes, SR_MEMORY_VRAM, byte_address,
                (uint8_t)patch->expected,
                (uint8_t)patch->replacement, 1u);
            sr_runner_emit_memory_write(
                snes, SR_MEMORY_VRAM, byte_address + 1u,
                (uint8_t)(patch->expected >> 8),
                (uint8_t)(patch->replacement >> 8), 1u);
        }
    }
    return SR_RESULT_OK;
}

static SrResult update_ppu_obj_metadata(
        SrRunnerHandle *runner,
        const SrPpuObjMetadataRequest *request) {
    Snes *snes = runner_from_handle(runner);
    uint8_t seen[SR_PPU_OBJ_POSITION_UPDATE_MAX / 8u] = {0};
    const uint32_t supported_flags =
        SR_PPU_OBJ_METADATA_CLEAR_POSITIONS |
        SR_PPU_OBJ_METADATA_CLEAR_CAMERA_RELATIVE;
    Ppu *ppu;
    uint32_t index;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_OBJ_METADATA_REQUEST_V2_SIZE ||
        (request->flags & ~supported_flags) != 0u ||
        request->update_count > SR_PPU_OBJ_POSITION_UPDATE_MAX ||
        (request->update_count == 0u && request->flags == 0u) ||
        (request->update_count != 0u && request->updates == NULL) ||
        (request->update_count == 0u && request->updates != NULL) ||
        request->reserved != 0u ||
        (request->updates != NULL &&
         (uintptr_t)request->updates %
             _Alignof(SrPpuObjPositionUpdate) != 0u))
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;

    /* Validate the entire batch before clears: malformed input cannot erase
     * a prior producer's metadata or partially publish a replacement. */
    for (index = 0u; index < request->update_count; ++index) {
        const SrPpuObjPositionUpdate *update = &request->updates[index];
        uint32_t byte;
        uint8_t bit;
        if (update->slot >= SR_PPU_OBJ_POSITION_UPDATE_MAX ||
            (update->flags & ~SR_PPU_OBJ_POSITION_CAMERA_RELATIVE) != 0u ||
            update->reserved != 0u)
            return SR_RESULT_INVALID_ARGUMENT;
        byte = update->slot >> 3;
        bit = (uint8_t)(1u << (update->slot & 7u));
        if ((seen[byte] & bit) != 0u)
            return SR_RESULT_INVALID_ARGUMENT;
        seen[byte] |= bit;
    }

    if ((request->flags & SR_PPU_OBJ_METADATA_CLEAR_POSITIONS) != 0u)
        PpuClearObjExactPositions(ppu);
    if ((request->flags &
         SR_PPU_OBJ_METADATA_CLEAR_CAMERA_RELATIVE) != 0u)
        PpuClearObjCameraRelative(ppu);
    for (index = 0u; index < request->update_count; ++index) {
        const SrPpuObjPositionUpdate *update = &request->updates[index];
        PpuSetObjExactPosition(ppu, update->slot, update->x, update->y);
        PpuSetObjCameraRelative(
            ppu, update->slot,
            (update->flags & SR_PPU_OBJ_POSITION_CAMERA_RELATIVE) != 0u);
    }
    return SR_RESULT_OK;
}

static SrResult update_ppu_layer_extents(
        SrRunnerHandle *runner,
        const SrPpuLayerExtentRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    uint32_t index;
    uint8_t default_layers = 0u;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_LAYER_EXTENT_REQUEST_V2_SIZE ||
        request->flags != 0u || request->updates == NULL ||
        request->update_count == 0u ||
        request->update_count > SR_PPU_LAYER_EXTENT_UPDATE_MAX ||
        request->reserved != 0u ||
        (uintptr_t)request->updates %
            _Alignof(SrPpuLayerExtentUpdate) != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;

    for (index = 0u; index < request->update_count; ++index) {
        const SrPpuLayerExtentUpdate *update = &request->updates[index];
        if (update->layer >= 4u ||
            update->left > SR_PPU_LAYER_EXTENT_AVAILABLE ||
            update->right > SR_PPU_LAYER_EXTENT_AVAILABLE ||
            update->top > SR_PPU_LAYER_EXTENT_AVAILABLE ||
            update->bottom > SR_PPU_LAYER_EXTENT_AVAILABLE)
            return SR_RESULT_INVALID_ARGUMENT;
        if (update->kind == SR_PPU_LAYER_EXTENT_DEFAULT) {
            uint8_t bit = (uint8_t)(1u << update->layer);
            if (update->y0 != 0u || update->y1 != 0u ||
                (default_layers & bit) != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            default_layers |= bit;
        } else if (update->kind ==
                       SR_PPU_LAYER_EXTENT_HORIZONTAL_BAND) {
            if (update->y0 >= update->y1 ||
                update->y1 > SR_PPU_NATIVE_HEIGHT ||
                update->top != 0u || update->bottom != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
        } else {
            return SR_RESULT_INVALID_ARGUMENT;
        }
    }
    for (index = 0u; index < request->update_count; ++index) {
        const SrPpuLayerExtentUpdate *update = &request->updates[index];
        if (update->kind == SR_PPU_LAYER_EXTENT_DEFAULT) {
            PpuSetWidescreenLayerExtent(
                ppu, (uint8_t)update->layer,
                (uint16_t)update->left, (uint16_t)update->right,
                (uint16_t)update->top, (uint16_t)update->bottom);
        } else {
            PpuSetWidescreenLayerExtentBand(
                ppu, (uint8_t)update->layer,
                (uint8_t)update->y0, (uint8_t)update->y1,
                (uint16_t)update->left, (uint16_t)update->right);
        }
    }
    return SR_RESULT_OK;
}

static bool ppu_abi_virtual_lookup(const void *context, int32_t tile_x,
                                   int32_t tile_y, uint16_t *entry) {
    const SrPpuVirtualTilemapBinding *binding = context;
    return binding != NULL && binding->lookup != NULL && entry != NULL &&
        binding->lookup(binding->user_data, tile_x, tile_y, entry) != 0u;
}

static size_t ppu_abi_virtual_span_lookup(
        const void *context, int32_t tile_x, int32_t tile_y,
        int32_t tile_step, size_t capacity, const uint16_t **entries,
        ptrdiff_t *word_stride) {
    const SrPpuVirtualTilemapBinding *binding = context;
    uint32_t abi_capacity;
    uint32_t count;
    int64_t abi_stride = 0;
    if (entries != NULL) *entries = NULL;
    if (word_stride != NULL) *word_stride = 0;
    if (binding == NULL || binding->lookup_span == NULL || entries == NULL ||
        word_stride == NULL || capacity == 0u)
        return 0u;
    abi_capacity = capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)capacity;
    count = binding->lookup_span(
        binding->user_data, tile_x, tile_y, tile_step, abi_capacity,
        entries, &abi_stride);
    if (count > abi_capacity ||
        (int64_t)(ptrdiff_t)abi_stride != abi_stride) {
        *entries = NULL;
        return 0u;
    }
    *word_stride = (ptrdiff_t)abi_stride;
    return count;
}

static bool ppu_abi_virtual_band_lookup(
        const void *context, int32_t tile_x, int32_t tile_y,
        uint16_t entry, uint8_t *band) {
    const SrPpuVirtualTilemapBinding *binding = context;
    return binding != NULL && binding->band_lookup != NULL && band != NULL &&
        binding->band_lookup(
            binding->user_data, tile_x, tile_y, entry, band) != 0u;
}

static SrResult replace_ppu_virtual_tilemaps(
        SrRunnerHandle *runner,
        const SrPpuVirtualTilemapRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    uint32_t layer;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_VIRTUAL_TILEMAP_REQUEST_V2_SIZE ||
        request->flags != 0u || request->layer_mask > 0x03u ||
        request->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    for (layer = 0u; layer < 2u; ++layer) {
        const SrPpuVirtualTilemapBinding *binding =
            &request->bindings[layer];
        if ((request->layer_mask & (1u << layer)) == 0u) continue;
        if (binding->lookup == NULL || binding->hscroll_anchor > 0x3ffu ||
            binding->vscroll_anchor > 0x3ffu ||
            (binding->flags &
             ~SR_PPU_VIRTUAL_TILEMAP_INCLUDE_AUTHENTIC) != 0u ||
            binding->reserved != 0u)
            return SR_RESULT_INVALID_ARGUMENT;
    }

    PpuClearVirtualTilemaps(ppu);
    memset(ppu->abiVirtualTilemap, 0, sizeof(ppu->abiVirtualTilemap));
    for (layer = 0u; layer < 2u; ++layer) {
        const SrPpuVirtualTilemapBinding *source;
        PpuVirtualTilemapBinding binding;
        if ((request->layer_mask & (1u << layer)) == 0u) continue;
        source = &request->bindings[layer];
        ppu->abiVirtualTilemap[layer] = *source;
        binding = (PpuVirtualTilemapBinding) {
            .lookup = ppu_abi_virtual_lookup,
            .lookup_span = source->lookup_span != NULL
                ? ppu_abi_virtual_span_lookup : NULL,
            .band_lookup = source->band_lookup != NULL
                ? ppu_abi_virtual_band_lookup : NULL,
            .context = &ppu->abiVirtualTilemap[layer],
            .camera_x = source->camera_x,
            .camera_y = source->camera_y,
            .hscroll_anchor = (uint16_t)source->hscroll_anchor,
            .vscroll_anchor = (uint16_t)source->vscroll_anchor,
            .flags = (uint8_t)source->flags,
        };
        if (!PpuSetVirtualTilemap(ppu, (uint8_t)layer, &binding)) {
            PpuClearVirtualTilemaps(ppu);
            memset(ppu->abiVirtualTilemap, 0,
                   sizeof(ppu->abiVirtualTilemap));
            return SR_RESULT_INVALID_ARGUMENT;
        }
    }
    return SR_RESULT_OK;
}

static SrResult update_ppu_authentic_camera(
        SrRunnerHandle *runner,
        const SrPpuAuthenticCameraRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    const uint32_t layer_mask = request != NULL ? request->layer_mask : 0u;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_AUTHENTIC_CAMERA_REQUEST_V2_SIZE ||
        (request->flags & ~SR_PPU_AUTHENTIC_CAMERA_CLEAR) != 0u ||
        layer_mask > SR_PPU_AUTHENTIC_CAMERA_ALL ||
        (request->flags == 0u && layer_mask == 0u) ||
        (layer_mask == 0u && (request->row_count != 0u ||
                             request->bg1_hscroll != NULL ||
                             request->bg2_hscroll != NULL ||
                             request->object_offset_x != 0)) ||
        (layer_mask != 0u && request->row_count != SR_PPU_NATIVE_HEIGHT) ||
        ((layer_mask & SR_PPU_AUTHENTIC_CAMERA_BG1) != 0u &&
         request->bg1_hscroll == NULL) ||
        ((layer_mask & SR_PPU_AUTHENTIC_CAMERA_BG1) == 0u &&
         request->bg1_hscroll != NULL) ||
        ((layer_mask & SR_PPU_AUTHENTIC_CAMERA_BG2) != 0u &&
         request->bg2_hscroll == NULL) ||
        ((layer_mask & SR_PPU_AUTHENTIC_CAMERA_BG2) == 0u &&
         request->bg2_hscroll != NULL) ||
        (request->bg1_hscroll != NULL &&
         (uintptr_t)request->bg1_hscroll % _Alignof(uint16_t) != 0u) ||
        (request->bg2_hscroll != NULL &&
         (uintptr_t)request->bg2_hscroll % _Alignof(uint16_t) != 0u) ||
        request->object_offset_x < INT16_MIN ||
        request->object_offset_x > INT16_MAX || request->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    if ((request->flags & SR_PPU_AUTHENTIC_CAMERA_CLEAR) != 0u)
        PpuClearAuthenticCameraFrame(ppu);
    if (layer_mask != 0u && !PpuSetAuthenticCameraFrame(
            ppu, (uint8_t)layer_mask, request->bg1_hscroll,
            request->bg2_hscroll, request->object_offset_x))
        return SR_RESULT_INVALID_ARGUMENT;
    return SR_RESULT_OK;
}

static SrResult run_ppu_scanout(
        SrRunnerHandle *runner,
        const SrPpuScanoutRequest *request,
        SrPpuScanoutResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_SCANOUT_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_PPU_SCANOUT_RESULT_V2_SIZE ||
        request->flags != 0u || request->hdma_suppress_mask > 0xffu ||
        request->reserved != 0u || request->irq_callback == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    memset(out_result, 0, SR_PPU_SCANOUT_RESULT_V2_SIZE);
    out_result->struct_size = SR_PPU_SCANOUT_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (s_ppu_scanout_runner != snes || s_ppu_scanout_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_ppu_scanout_provider(snes, request, out_result);
}

static SrResult validate_ppu_output_capacity(
        const SrPpuOutputBindingRequest *request, uint64_t minimum_width,
        uint64_t maximum_width, uint64_t maximum_height) {
    uint64_t width;
    if (request->pixels == NULL) {
        return request->pixel_byte_size == 0u && request->pitch_bytes == 0u &&
                       request->height_pixels == 0u
            ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
    }
    if (((uintptr_t)request->pixels % _Alignof(uint32_t)) != 0u ||
        request->pitch_bytes == 0u ||
        request->pitch_bytes % sizeof(uint32_t) != 0u ||
        request->pitch_bytes > (uint64_t)SIZE_MAX ||
        request->height_pixels == 0u ||
        request->height_pixels > maximum_height)
        return SR_RESULT_INVALID_ARGUMENT;
    width = request->pitch_bytes / sizeof(uint32_t);
    if (width < minimum_width || width > maximum_width ||
        request->pitch_bytes > UINT64_MAX / request->height_pixels ||
        request->pixel_byte_size <
            request->pitch_bytes * request->height_pixels)
        return SR_RESULT_INVALID_ARGUMENT;
    return SR_RESULT_OK;
}

static SrResult bind_ppu_output_surface(
        SrRunnerHandle *runner, const SrPpuOutputBindingRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    SrResult capacity_result;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_OUTPUT_BINDING_REQUEST_V2_SIZE ||
        request->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    switch (request->kind) {
        case SR_PPU_OUTPUT_MAIN: {
            uint32_t render_flags = 0u;
            if ((request->flags &
                 ~SR_PPU_OUTPUT_REFERENCE_PIXEL_RENDERER) != 0u ||
                request->source != 0u || request->band != 0u ||
                request->scale != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, SR_PPU_NATIVE_WIDTH, SR_PPU_SURFACE_MAX_WIDTH,
                SR_PPU_SURFACE_MAX_HEIGHT);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            if ((request->flags &
                 SR_PPU_OUTPUT_REFERENCE_PIXEL_RENDERER) != 0u)
                render_flags |= kPpuRenderFlags_ReferencePixelRenderer;
            if (request->pixels != NULL)
                snes->diagnosticMainSurfaceBound = true;
            PpuBeginDrawing(ppu, request->pixels,
                            (size_t)request->pitch_bytes, render_flags);
            return SR_RESULT_OK;
        }
        case SR_PPU_OUTPUT_AUTHENTIC:
            if (request->flags != 0u || request->source != 0u ||
                request->band != 0u || request->scale != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, SR_PPU_NATIVE_WIDTH, SR_PPU_SURFACE_MAX_WIDTH,
                SR_PPU_SURFACE_MAX_HEIGHT);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            return PpuBindAuthenticSurface(
                       ppu, request->pixels, (size_t)request->pitch_bytes)
                ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
        case SR_PPU_OUTPUT_OVERLAY:
            if (request->flags != 0u ||
                request->source >= SR_PPU_OVERLAY_SOURCE_COUNT ||
                request->band != 0u || request->scale != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, SR_PPU_NATIVE_WIDTH, SR_PPU_SURFACE_MAX_WIDTH,
                SR_PPU_SURFACE_MAX_HEIGHT);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            return PpuBindOverlaySurfaceSized(
                       ppu, (PpuOverlaySource)request->source,
                       request->pixels, (size_t)request->pitch_bytes,
                       request->height_pixels)
                ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
        case SR_PPU_OUTPUT_OVERLAY_PRIORITY:
            if (request->flags != 0u ||
                request->source >= SR_PPU_OVERLAY_SOURCE_COUNT ||
                request->band == 0u ||
                request->band >= SR_PPU_SURFACE_BAND_COUNT ||
                request->scale != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, SR_PPU_NATIVE_WIDTH, SR_PPU_SURFACE_MAX_WIDTH,
                SR_PPU_SURFACE_MAX_HEIGHT);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            if (request->pixels != NULL &&
                request->pitch_bytes !=
                    ppu->overlayRenderPitch[request->source])
                return SR_RESULT_INVALID_ARGUMENT;
            return PpuBindOverlayPrioSurface(
                       ppu, (PpuOverlaySource)request->source,
                       (int)request->band, request->pixels)
                ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
        case SR_PPU_OUTPUT_MODE7:
            if (request->flags != 0u || request->source != 0u ||
                request->band != 0u ||
                (request->pixels == NULL ? request->scale != 0u
                                         : (request->scale < 1u ||
                                            request->scale > 4u)))
                return SR_RESULT_INVALID_ARGUMENT;
            capacity_result = validate_ppu_output_capacity(
                request, (uint64_t)SR_PPU_NATIVE_WIDTH * request->scale,
                (uint64_t)SR_PPU_SURFACE_MAX_WIDTH * request->scale,
                (uint64_t)SR_PPU_SURFACE_MAX_HEIGHT * request->scale);
            if (capacity_result != SR_RESULT_OK) return capacity_result;
            return PpuBindMode7OverlaySurface(
                       ppu, request->pixels, (size_t)request->pitch_bytes,
                       (uint8_t)request->scale)
                ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
        case SR_PPU_OUTPUT_CLEAR_OVERLAY_SOURCES:
            if (request->flags != 0u || request->source != 0u ||
                request->band != 0u || request->scale != 0u ||
                request->pixels != NULL || request->pixel_byte_size != 0u ||
                request->pitch_bytes != 0u || request->height_pixels != 0u)
                return SR_RESULT_INVALID_ARGUMENT;
            PpuClearOverlayBindings(ppu);
            return SR_RESULT_OK;
        default:
            return SR_RESULT_UNSUPPORTED;
    }
}

static SrResult configure_ppu_horizontal_margin(
        SrRunnerHandle *runner,
        const SrPpuHorizontalMarginRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    uint8_t old_budget, old_left, old_right;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_HORIZONTAL_MARGIN_REQUEST_V2_SIZE ||
        request->flags != 0u || request->budget_pixels >
            SR_PPU_HORIZONTAL_MARGIN_MAX ||
        request->reserved[0] != 0u || request->reserved[1] != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    old_budget = ppu->extraLeftRight;
    old_left = ppu->extraLeftCur;
    old_right = ppu->extraRightCur;
    switch (request->mode) {
        case SR_PPU_HORIZONTAL_MARGIN_AVAILABLE:
            PpuSetExtraSpace(ppu, (uint8_t)request->budget_pixels);
            break;
        case SR_PPU_HORIZONTAL_MARGIN_CENTERED:
            PpuSetExtraSpaceCentered(ppu, (uint8_t)request->budget_pixels);
            break;
        default:
            return SR_RESULT_UNSUPPORTED;
    }
    if (ppu->extraLeftRight != old_budget ||
        ppu->extraLeftCur != old_left || ppu->extraRightCur != old_right)
        PpuNoteSurfaceViewChange(ppu);
    return SR_RESULT_OK;
}

SrResult sr_runner_apply_ppu_frame_policy(
        Snes *snes, const SrPpuFramePolicy *policy) {
    Ppu *ppu;
    uint32_t index;
    if (snes == NULL || policy == NULL ||
        policy->struct_size < SR_PPU_FRAME_POLICY_V2_SIZE ||
        (policy->flags &
         ~(SR_PPU_FRAME_POLICY_PAD_CAPTURED_TO_BUDGET |
           SR_PPU_FRAME_POLICY_FINALIZE)) != 0u ||
        policy->margin_budget_pixels > SR_PPU_HORIZONTAL_MARGIN_MAX ||
        policy->margin_left_pixels > policy->margin_budget_pixels ||
        policy->margin_right_pixels > policy->margin_budget_pixels ||
        policy->margin_top_pixels > SR_PPU_VERTICAL_MARGIN_MAX ||
        policy->margin_bottom_pixels > SR_PPU_VERTICAL_MARGIN_MAX ||
        (policy->layer_clamp_mask & ~SR_PPU_FRAME_POLICY_LAYER_MASK) != 0u ||
        (policy->layer_mirror_mask & ~SR_PPU_FRAME_POLICY_LAYER_MASK) != 0u ||
        (policy->layer_repeat_mask & ~SR_PPU_FRAME_POLICY_LAYER_MASK) != 0u ||
        (policy->layer_normal_scroll_mask &
         ~SR_PPU_FRAME_POLICY_LAYER_MASK) != 0u ||
        (policy->vertical_clip_layer_mask &
         ~SR_PPU_FRAME_POLICY_LAYER_MASK) != 0u ||
        policy->band_count > SR_PPU_FRAME_POLICY_BAND_MAX ||
        (policy->band_count == 0u && policy->bands != NULL) ||
        (policy->band_count != 0u && policy->bands == NULL) ||
        (policy->bands != NULL &&
         (uintptr_t)policy->bands % _Alignof(SrPpuFramePolicyBand) != 0u) ||
        policy->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (policy->horizontal_mode == SR_PPU_HORIZONTAL_MARGIN_CENTERED) {
        if (policy->margin_left_pixels != 0u ||
            policy->margin_right_pixels != 0u)
            return SR_RESULT_INVALID_ARGUMENT;
    } else if (policy->horizontal_mode !=
                   SR_PPU_HORIZONTAL_MARGIN_AVAILABLE) {
        return SR_RESULT_UNSUPPORTED;
    }
    if (policy->hud_split_height == 0u) {
        if (policy->hud_left_end_x != 0u ||
            policy->hud_right_start_x != 0u ||
            policy->hud_player_row_y != 0u ||
            policy->hud_left_only_y != 0u)
            return SR_RESULT_INVALID_ARGUMENT;
    } else if (policy->hud_split_height > SR_PPU_NATIVE_HEIGHT ||
               policy->hud_left_end_x == 0u ||
               policy->hud_left_end_x > policy->hud_right_start_x ||
               policy->hud_right_start_x >= SR_PPU_NATIVE_WIDTH ||
               policy->hud_player_row_y > policy->hud_left_only_y ||
               policy->hud_left_only_y > policy->hud_split_height) {
        return SR_RESULT_INVALID_ARGUMENT;
    }
    for (index = 0u; index < 4u; ++index) {
        uint32_t bit = 1u << index;
        if (policy->vertical_clip_top_rows[index] >
                SR_PPU_VERTICAL_MARGIN_MAX ||
            policy->vertical_clip_bottom_rows[index] >
                SR_PPU_VERTICAL_MARGIN_MAX ||
            ((policy->vertical_clip_layer_mask & bit) == 0u &&
             (policy->vertical_clip_top_rows[index] != 0u ||
              policy->vertical_clip_bottom_rows[index] != 0u)))
            return SR_RESULT_INVALID_ARGUMENT;
    }
    for (index = 0u; index < policy->band_count; ++index) {
        const SrPpuFramePolicyBand *band = &policy->bands[index];
        if (band->layer >= 4u || band->y0 >= band->y1 ||
            band->y1 > SR_PPU_NATIVE_HEIGHT ||
            band->fill < SR_PPU_BACKGROUND_FILL_TRANSPARENT ||
            band->fill > SR_PPU_BACKGROUND_FILL_RAW_WRAP ||
            band->motion > SR_PPU_BACKGROUND_MOTION_NORMAL_SCROLL)
            return SR_RESULT_INVALID_ARGUMENT;
    }

    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    if ((policy->flags & SR_PPU_FRAME_POLICY_FINALIZE) != 0u &&
        (!snes->abiPpuFramePolicyActive ||
         snes->abiPpuFramePolicyGeneration !=
             snes->abiLifetimeGeneration ||
         policy->margin_budget_pixels != ppu->extraLeftRight))
        return SR_RESULT_BUSY;
    if ((policy->flags & SR_PPU_FRAME_POLICY_FINALIZE) != 0u) {
        /* Centered mode encodes its exact sides as zero and BEGIN already
         * established that geometry. Only AVAILABLE mode has exact sides to
         * refine after provider publication. */
        if (policy->horizontal_mode ==
                SR_PPU_HORIZONTAL_MARGIN_AVAILABLE)
            PpuSetExtraSideSpace(
                ppu, (int)policy->margin_left_pixels,
                (int)policy->margin_right_pixels, 0);
        PpuClearWidescreenLayerBands(ppu);
    } else if (policy->horizontal_mode ==
                   SR_PPU_HORIZONTAL_MARGIN_CENTERED) {
        PpuSetExtraSpaceCentered(ppu,
                                 (uint8_t)policy->margin_budget_pixels);
    } else {
        PpuSetExtraSpace(ppu, (uint8_t)policy->margin_budget_pixels);
        PpuSetExtraSideSpace(
            ppu, (int)policy->margin_left_pixels,
            (int)policy->margin_right_pixels, 0);
    }
    PpuSetWidescreenLayerClamp(
        ppu, (uint8_t)policy->layer_clamp_mask);
    PpuSetWidescreenLayerMirror(
        ppu, (uint8_t)policy->layer_mirror_mask);
    PpuSetWidescreenLayerRepeat(
        ppu, (uint8_t)policy->layer_repeat_mask);
    PpuSetWidescreenLayerNormalScroll(
        ppu, (uint8_t)policy->layer_normal_scroll_mask);
    PpuSetWidescreenPadCapturedToBudget(
        ppu, (uint8_t)((policy->flags &
            SR_PPU_FRAME_POLICY_PAD_CAPTURED_TO_BUDGET) != 0u));
    PpuSetWidescreenHudSplit(
        ppu, (uint8_t)policy->hud_split_height,
        (uint8_t)policy->hud_left_end_x,
        (uint8_t)policy->hud_right_start_x,
        (uint8_t)policy->hud_player_row_y,
        (uint8_t)policy->hud_left_only_y);
    for (index = 0u; index < policy->band_count; ++index) {
        const SrPpuFramePolicyBand *band = &policy->bands[index];
        PpuSetWidescreenLayerBand(
            ppu, (uint8_t)band->layer, (uint8_t)band->y0,
            (uint8_t)band->y1, (PpuWidescreenBandFill)band->fill,
            (PpuWidescreenMotion)band->motion);
    }
    PpuSetExtraVerticalSpace(
        ppu, (int)policy->margin_top_pixels,
        (int)policy->margin_bottom_pixels);
    for (index = 0u; index < 4u; ++index) {
        if ((policy->vertical_clip_layer_mask & (1u << index)) != 0u)
            PpuSetVerticalMarginLayerClip(
                ppu, (uint8_t)index,
                (int)policy->vertical_clip_top_rows[index],
                (int)policy->vertical_clip_bottom_rows[index]);
    }
    if ((policy->flags & SR_PPU_FRAME_POLICY_FINALIZE) != 0u) {
        snes->abiPpuFramePolicyActive = false;
    } else {
        snes->abiPpuFramePolicyGeneration = snes->abiLifetimeGeneration;
        snes->abiPpuFramePolicyActive = true;
    }
    return SR_RESULT_OK;
}

static SrResult apply_ppu_frame_policy(
        SrRunnerHandle *runner,
        const SrPpuFramePolicyRequest *request) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_FRAME_POLICY_REQUEST_V2_SIZE ||
        request->flags != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    return sr_runner_apply_ppu_frame_policy(snes, &request->policy);
}

static SrResult claim_ppu_overlay_capture(
        SrRunnerHandle *runner,
        const SrPpuOverlayCaptureRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    int64_t x1, y1;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_OVERLAY_CAPTURE_REQUEST_V2_SIZE ||
        (request->flags & ~SR_PPU_OVERLAY_FLAGS_SUPPORTED) != 0u ||
        request->source >= SR_PPU_OVERLAY_SOURCE_COUNT ||
        request->width <= 0 || request->height <= 0 ||
        request->reserved[0] != 0u || request->reserved[1] != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    x1 = (int64_t)request->x + request->width;
    y1 = (int64_t)request->y + request->height;
    if (x1 < INT32_MIN || x1 > INT32_MAX ||
        y1 < INT32_MIN || y1 > INT32_MAX)
        return SR_RESULT_INVALID_ARGUMENT;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    if (ppu->overlayCaptures[request->source].x1 >
        ppu->overlayCaptures[request->source].x0)
        return SR_RESULT_BUSY;
    return PpuSetOverlayCapture(
               ppu, (PpuOverlaySource)request->source,
               request->x, request->y, request->width, request->height,
               (uint8_t)request->flags)
        ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
}

static SrResult claim_ppu_mode7_override(
        SrRunnerHandle *runner,
        const SrPpuMode7OverrideRequest *request) {
    Snes *snes = runner_from_handle(runner);
    Ppu *ppu;
    uint64_t pixel_count;
    if (snes == NULL || request == NULL ||
        request->struct_size < SR_PPU_MODE7_OVERRIDE_REQUEST_V2_SIZE ||
        request->flags != 0u || request->pixels == NULL ||
        ((uintptr_t)request->pixels % _Alignof(uint32_t)) != 0u ||
        request->width_pixels == 0u || request->height_pixels == 0u ||
        request->width_pixels > INT32_MAX ||
        request->height_pixels > INT32_MAX ||
        request->canvas_x0 < 0 || request->canvas_y0 < 0 ||
        request->canvas_x1 <= request->canvas_x0 ||
        request->canvas_y1 <= request->canvas_y0 ||
        request->canvas_x1 > (int32_t)SR_PPU_MODE7_CANVAS_EXTENT ||
        request->canvas_y1 > (int32_t)SR_PPU_MODE7_CANVAS_EXTENT ||
        request->wrap > 1u || request->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (request->width_pixels > UINT64_MAX / request->height_pixels)
        return SR_RESULT_INVALID_ARGUMENT;
    pixel_count = (uint64_t)request->width_pixels * request->height_pixels;
    if (pixel_count > UINT64_MAX / sizeof(uint32_t) ||
        request->pixel_byte_size < pixel_count * sizeof(uint32_t))
        return SR_RESULT_INVALID_ARGUMENT;
    ppu = ppu_output_control_target(snes);
    if (ppu == NULL) return SR_RESULT_UNAVAILABLE;
    if (ppu->m7Override.rgba != NULL) return SR_RESULT_BUSY;
    return PpuSetMode7Override(
               ppu, request->pixels, (int)request->width_pixels,
               (int)request->height_pixels, request->canvas_x0,
               request->canvas_y0, request->canvas_x1, request->canvas_y1,
               (uint8_t)request->wrap)
        ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

static SrResult rasterize_ppu_obj_range(
        SrRunnerHandle *runner, const SrPpuObjRasterRequest *request,
        SrPpuObjRasterResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_OBJ_RASTER_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_PPU_OBJ_RASTER_RESULT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_PPU_OBJ_RASTER_RESULT_V2_SIZE);
    out_result->struct_size = SR_PPU_OBJ_RASTER_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (s_ppu_obj_raster_runner != snes ||
        s_ppu_obj_raster_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_ppu_obj_raster_provider(snes, request, out_result);
}

static SrResult resolve_ppu_obj_range(
        SrRunnerHandle *runner, const SrPpuObjResolveRequest *request,
        SrPpuObjResolveResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_OBJ_RESOLVE_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_PPU_OBJ_RESOLVE_RESULT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_PPU_OBJ_RESOLVE_RESULT_V2_SIZE);
    out_result->struct_size = SR_PPU_OBJ_RESOLVE_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (s_ppu_obj_resolve_runner != snes ||
        s_ppu_obj_resolve_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_ppu_obj_resolve_provider(snes, request, out_result);
}

static SrResult rasterize_ppu_obj_parts(
        SrRunnerHandle *runner,
        const SrPpuObjPartsRasterRequest *request,
        SrPpuObjRasterResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_PPU_OBJ_PARTS_RASTER_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_PPU_OBJ_RASTER_RESULT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_PPU_OBJ_RASTER_RESULT_V2_SIZE);
    out_result->struct_size = SR_PPU_OBJ_RASTER_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if (s_ppu_obj_parts_raster_runner != snes ||
        s_ppu_obj_parts_raster_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_ppu_obj_parts_raster_provider(snes, request, out_result);
}
