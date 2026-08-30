/* Unity-build domain fragment. Included only by runner.c after runner_core.inc. */
typedef struct DigestSaveLoad {
    SaveLoadInfo base;
    Sha256Context *sha;
} DigestSaveLoad;

static void digest_saveload(SaveLoadInfo *info, void *data,
                            size_t data_size) {
    DigestSaveLoad *digest = (DigestSaveLoad *)(void *)info;
    if (digest == NULL || digest->sha == NULL ||
        (data == NULL && data_size != 0u)) {
        if (info != NULL) info->failed = true;
        return;
    }
    sha256_update(digest->sha, (const uint8_t *)data, data_size);
}

static void digest_u16(Sha256Context *sha, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    sha256_update(sha, bytes, sizeof(bytes));
}

static void digest_u32(Sha256Context *sha, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    sha256_update(sha, bytes, sizeof(bytes));
}

static void digest_u64(Sha256Context *sha, uint64_t value) {
    uint8_t bytes[8];
    unsigned index;
    for (index = 0u; index < 8u; ++index)
        bytes[index] = (uint8_t)(value >> (index * 8u));
    sha256_update(sha, bytes, sizeof(bytes));
}

static SrResult semantic_digest(Snes *snes, uint8_t digest[32]) {
    static const uint8_t domain[] = "snesrecomp-semantic-state-v1";
    SrCpuStateSnapshot cpu = {
        .struct_size = sizeof(cpu),
    };
    Sha256Context sha;
    DigestSaveLoad saveload;
    SrResult result;
    if (sr_runner_audio_query_forbidden()) return SR_RESULT_BUSY;
    if (snes->cpu == NULL || snes->apu == NULL || snes->dma == NULL ||
        snes->ppu == NULL || snes->cart == NULL || snes->ram == NULL)
        return SR_RESULT_UNAVAILABLE;
    RtlApuLock();
    result = query_cpu_state(sr_runner_handle(snes), &cpu);
    if (result != SR_RESULT_OK) {
        RtlApuUnlock();
        return result;
    }
    sha256_init(&sha);
    sha256_update(&sha, domain, sizeof(domain) - 1u);
    digest_u32(&sha, SR_DETERMINISM_SEMANTIC_SCHEMA_VERSION);
    digest_u64(&sha, snes->abiFrameCounter);
    digest_u32(&sha, cpu.flags);
    digest_u64(&sha, cpu.frame_counter);
    digest_u32(&sha, cpu.execution_pc24);
    digest_u16(&sha, cpu.a);
    digest_u16(&sha, cpu.x);
    digest_u16(&sha, cpu.y);
    digest_u16(&sha, cpu.s);
    digest_u16(&sha, cpu.d);
    digest_u32(&sha, (uint32_t)cpu.db | ((uint32_t)cpu.pb << 8) |
                        ((uint32_t)cpu.p << 16));
    digest_u16(&sha, snes->input1_currentState);
    digest_u16(&sha, snes->input2_currentState);
    saveload.base.func = digest_saveload;
    saveload.base.saving = true;
    saveload.base.portable = true;
    saveload.base.semantic = true;
    saveload.base.failed = false;
    saveload.sha = &sha;
    snes_saveload(snes, &saveload.base);
    RtlApuUnlock();
    if (saveload.base.failed) return SR_RESULT_UNAVAILABLE;
    sha256_final(&sha, digest);
    return SR_RESULT_OK;
}

void sr_runner_capture_presentation_digest(Snes *snes) {
    static const uint8_t domain[] = "snesrecomp-presentation-v1";
    Ppu *ppu;
    Sha256Context sha;
    uint32_t width;
    uint32_t height;
    int start_x;
    uint32_t row;
    if (snes == NULL) return;
    snes->abiScanoutAvailable = false;
    ppu = snes->ppu;
    if (ppu == NULL || ppu->renderBuffer == NULL || ppu->renderPitch == 0u)
        return;
    width = SR_PPU_NATIVE_WIDTH + ppu->extraLeftCur + ppu->extraRightCur;
    height = SR_PPU_NATIVE_HEIGHT + ppu->extraTopCur + ppu->extraBottomCur;
    start_x = PpuSurfaceApron(ppu, ppu->renderPitch) +
        ppu->extraLeftRight - ppu->extraLeftCur;
    if (start_x < 0 || width > ppu->renderPitch / sizeof(uint32_t) ||
        (uint64_t)(uint32_t)start_x + width >
            ppu->renderPitch / sizeof(uint32_t) ||
        height > ppu->renderHeight)
        return;
    sha256_init(&sha);
    sha256_update(&sha, domain, sizeof(domain) - 1u);
    digest_u32(&sha, SR_DETERMINISM_PRESENTATION_SCHEMA_VERSION);
    digest_u64(&sha, snes->abiFrameCounter);
    digest_u32(&sha, SR_DETERMINISM_PIXEL_XRGB8888_LE);
    digest_u32(&sha, width);
    digest_u32(&sha, height);
    digest_u16(&sha, ppu->extraLeftCur);
    digest_u16(&sha, ppu->extraRightCur);
    digest_u16(&sha, ppu->extraTopCur);
    digest_u16(&sha, ppu->extraBottomCur);
    for (row = 0u; row < height; ++row) {
        const uint8_t *source = ppu->renderBuffer +
            (size_t)row * ppu->renderPitch +
            (size_t)start_x * sizeof(uint32_t);
        uint32_t column;
        for (column = 0u; column < width; ++column) {
            uint32_t pixel;
            memcpy(&pixel, source + (size_t)column * sizeof(pixel),
                   sizeof(pixel));
            digest_u32(&sha, pixel);
        }
    }
    sha256_final(&sha, snes->abiLastPresentationSha256);
    snes->abiLastScanoutFrame = snes->abiFrameCounter;
    snes->abiLastPresentationWidth = width;
    snes->abiLastPresentationHeight = height;
    snes->abiLastPresentationMarginLeft = ppu->extraLeftCur;
    snes->abiLastPresentationMarginRight = ppu->extraRightCur;
    snes->abiLastPresentationMarginTop = ppu->extraTopCur;
    snes->abiLastPresentationMarginBottom = ppu->extraBottomCur;
    snes->abiScanoutAvailable = true;
}

static SrResult presentation_digest(
        Snes *snes, SrDeterminismDigestResult *out_result) {
    if (!snes->abiScanoutAvailable) return SR_RESULT_UNAVAILABLE;
    memcpy(out_result->presentation_sha256,
           snes->abiLastPresentationSha256,
           sizeof(out_result->presentation_sha256));
    out_result->valid_flags |= SR_DETERMINISM_DIGEST_PRESENTATION;
    out_result->presentation_frame_counter = snes->abiLastScanoutFrame;
    out_result->presentation_schema_version =
        SR_DETERMINISM_PRESENTATION_SCHEMA_VERSION;
    out_result->presentation_width_pixels =
        snes->abiLastPresentationWidth;
    out_result->presentation_height_pixels =
        snes->abiLastPresentationHeight;
    out_result->presentation_margin_left_pixels =
        snes->abiLastPresentationMarginLeft;
    out_result->presentation_margin_right_pixels =
        snes->abiLastPresentationMarginRight;
    out_result->presentation_margin_top_pixels =
        snes->abiLastPresentationMarginTop;
    out_result->presentation_margin_bottom_pixels =
        snes->abiLastPresentationMarginBottom;
    out_result->presentation_pixel_format =
        SR_DETERMINISM_PIXEL_XRGB8888_LE;
    return SR_RESULT_OK;
}

static SrResult query_determinism_digest(
        SrRunnerHandle *runner, const SrDeterminismDigestRequest *request,
        SrDeterminismDigestResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    SrResult semantic_result = SR_RESULT_OK;
    SrResult presentation_result = SR_RESULT_OK;
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_DETERMINISM_DIGEST_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_DETERMINISM_DIGEST_RESULT_V2_SIZE ||
        request->flags == 0u ||
        (request->flags & ~SR_DETERMINISM_DIGEST_FLAGS_SUPPORTED) != 0u ||
        request->reserved != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_DETERMINISM_DIGEST_RESULT_V2_SIZE);
    out_result->struct_size = SR_DETERMINISM_DIGEST_RESULT_V2_SIZE;
    out_result->lifetime_generation = snes->abiLifetimeGeneration;
    out_result->frame_counter = snes->abiFrameCounter;
    if (request->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    if ((request->flags & SR_DETERMINISM_DIGEST_SEMANTIC) != 0u) {
        semantic_result = semantic_digest(snes, out_result->semantic_sha256);
        if (semantic_result == SR_RESULT_OK) {
            out_result->valid_flags |= SR_DETERMINISM_DIGEST_SEMANTIC;
            out_result->semantic_schema_version =
                SR_DETERMINISM_SEMANTIC_SCHEMA_VERSION;
        }
    }
    if ((request->flags & SR_DETERMINISM_DIGEST_PRESENTATION) != 0u)
        presentation_result = presentation_digest(snes, out_result);
    if (semantic_result == SR_RESULT_BUSY ||
        presentation_result == SR_RESULT_BUSY)
        return SR_RESULT_BUSY;
    if (semantic_result != SR_RESULT_OK ||
        presentation_result != SR_RESULT_OK)
        return SR_RESULT_UNAVAILABLE;
    return SR_RESULT_OK;
}
