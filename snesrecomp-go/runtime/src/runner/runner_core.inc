/* Unity-build domain fragment. Included only by runner.c. */
static SrResult get_component(SrRunnerHandle *runner,
                              SrComponentKind component,
                              const SrComponentHandle **out_component) {
    Snes *snes = runner_from_handle(runner);
    const void *resolved = NULL;
    if (out_component == NULL) return SR_RESULT_INVALID_ARGUMENT;
    *out_component = NULL;
    if (snes == NULL) return SR_RESULT_INVALID_ARGUMENT;
    switch (component) {
        case SR_COMPONENT_RUNNER: resolved = snes; break;
        case SR_COMPONENT_CPU:
            resolved = s_cpu_state_runner == snes && s_cpu_component != NULL
                ? s_cpu_component : snes->cpu;
            break;
        case SR_COMPONENT_PPU: resolved = snes->ppu; break;
        case SR_COMPONENT_APU: resolved = snes->apu; break;
        case SR_COMPONENT_DSP:
            resolved = snes->apu != NULL ? snes->apu->dsp : NULL;
            break;
        case SR_COMPONENT_SPC:
            resolved = snes->apu != NULL ? snes->apu->spc : NULL;
            break;
        case SR_COMPONENT_DMA: resolved = snes->dma; break;
        case SR_COMPONENT_CARTRIDGE: resolved = snes->cart; break;
        default:
            return SR_RESULT_UNSUPPORTED;
    }
    *out_component = component_handle(resolved);
    return resolved != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

static SrResult query_generations(
        SrRunnerHandle *runner, SrGenerationSnapshot *out_generations) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_generations == NULL ||
        out_generations->struct_size < SR_GENERATION_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    out_generations->struct_size = SR_GENERATION_SNAPSHOT_V2_SIZE;
    out_generations->reserved = 0u;
    out_generations->lifetime_generation = snes->abiLifetimeGeneration;
    out_generations->tick_generation = snes->abiTickGeneration;
    out_generations->reset_generation = snes->abiResetGeneration;
    out_generations->load_generation = snes->abiLoadGeneration;
    out_generations->mutation_generation = snes->abiMutationGeneration;
    return SR_RESULT_OK;
}

static SrResult resolve_memory(Snes *snes, SrMemoryRegion region,
                               const uint8_t **data, uint64_t *byte_size) {
    if (snes == NULL || data == NULL || byte_size == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    switch (region) {
        case SR_MEMORY_WRAM:
            *data = snes->ram;
            *byte_size = kSnesWramSize;
            break;
        case SR_MEMORY_SRAM:
            if (snes->cart == NULL || snes->cart->ram == NULL) {
                *data = NULL;
                *byte_size = 0u;
                return SR_RESULT_UNAVAILABLE;
            }
            *data = snes->cart->ram;
            *byte_size = snes->cart->ramSize;
            break;
        case SR_MEMORY_ROM:
            if (snes->cart == NULL || snes->cart->rom == NULL) {
                *data = NULL;
                *byte_size = 0u;
                return SR_RESULT_UNAVAILABLE;
            }
            *data = snes->cart->rom;
            *byte_size = snes->cart->romSize;
            break;
        case SR_MEMORY_HIGH_OAM:
            if (snes->ppu == NULL) {
                *data = NULL;
                *byte_size = 0u;
                return SR_RESULT_UNAVAILABLE;
            }
            *data = snes->ppu->highOam;
            *byte_size = SR_PPU_HIGH_OAM_BYTE_COUNT;
            break;
        case SR_MEMORY_APU_RAM:
        case SR_MEMORY_DSP_REGISTERS:
            /* The audio thread can advance these components between main
             * runner ticks. They need an APU-lock-aware pinned snapshot, not
             * a borrowed pointer with a misleading main-thread generation. */
            *data = NULL;
            *byte_size = 0u;
            return SR_RESULT_UNSUPPORTED;
        default:
            *data = NULL;
            *byte_size = 0u;
            return SR_RESULT_UNSUPPORTED;
    }
    return *data != NULL ? SR_RESULT_OK : SR_RESULT_UNAVAILABLE;
}

static SrResult borrow_memory(SrRunnerHandle *runner, SrMemoryRegion region,
                              SrBorrowedSpan *out_span) {
    Snes *snes = runner_from_handle(runner);
    const uint8_t *data = NULL;
    uint64_t byte_size = 0u;
    SrResult result;
    if (snes == NULL || out_span == NULL ||
        out_span->struct_size < SR_BORROWED_SPAN_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    result = resolve_memory(snes, region, &data, &byte_size);
    if (result != SR_RESULT_OK) {
        memset(out_span, 0, SR_BORROWED_SPAN_V2_SIZE);
        out_span->struct_size = SR_BORROWED_SPAN_V2_SIZE;
        out_span->region = region;
        return result;
    }
    out_span->struct_size = SR_BORROWED_SPAN_V2_SIZE;
    out_span->region = region;
    out_span->data = data;
    out_span->byte_size = byte_size;
    out_span->lifetime_generation = snes->abiLifetimeGeneration;
    return SR_RESULT_OK;
}

static uint32_t borrow_is_valid(SrRunnerHandle *runner,
                                const SrBorrowedSpan *span) {
    Snes *snes = runner_from_handle(runner);
    const uint8_t *data = NULL;
    uint64_t byte_size = 0u;
    if (snes == NULL || span == NULL ||
        span->struct_size < SR_BORROWED_SPAN_V2_SIZE || span->data == NULL ||
        span->lifetime_generation != snes->abiLifetimeGeneration)
        return 0u;
    if (resolve_memory(snes, span->region, &data, &byte_size) != SR_RESULT_OK)
        return 0u;
    return span->data == data && span->byte_size == byte_size ? 1u : 0u;
}

static SrResult resolve_u16_memory(Snes *snes, SrMemoryRegion region,
                                   const uint16_t **data,
                                   uint64_t *element_count) {
    if (snes == NULL || data == NULL || element_count == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    if (snes->ppu == NULL) {
        *data = NULL;
        *element_count = 0u;
        return SR_RESULT_UNAVAILABLE;
    }
    switch (region) {
        case SR_MEMORY_VRAM:
            *data = snes->ppu->vram;
            *element_count = SR_PPU_VRAM_WORD_COUNT;
            break;
        case SR_MEMORY_CGRAM:
            *data = snes->ppu->cgram;
            *element_count = SR_PPU_CGRAM_WORD_COUNT;
            break;
        case SR_MEMORY_OAM:
            *data = snes->ppu->oam;
            *element_count = SR_PPU_OAM_WORD_COUNT;
            break;
        default:
            *data = NULL;
            *element_count = 0u;
            return SR_RESULT_UNSUPPORTED;
    }
    return SR_RESULT_OK;
}

static SrResult borrow_u16_memory(SrRunnerHandle *runner,
                                  SrMemoryRegion region,
                                  SrBorrowedU16Span *out_span) {
    Snes *snes = runner_from_handle(runner);
    const uint16_t *data = NULL;
    uint64_t element_count = 0u;
    SrResult result;
    if (snes == NULL || out_span == NULL ||
        out_span->struct_size < SR_BORROWED_U16_SPAN_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    result = resolve_u16_memory(snes, region, &data, &element_count);
    if (result != SR_RESULT_OK) {
        memset(out_span, 0, SR_BORROWED_U16_SPAN_V2_SIZE);
        out_span->struct_size = SR_BORROWED_U16_SPAN_V2_SIZE;
        out_span->region = region;
        return result;
    }
    out_span->struct_size = SR_BORROWED_U16_SPAN_V2_SIZE;
    out_span->region = region;
    out_span->data = data;
    out_span->element_count = element_count;
    out_span->lifetime_generation = snes->abiLifetimeGeneration;
    return SR_RESULT_OK;
}

static uint32_t borrow_u16_is_valid(SrRunnerHandle *runner,
                                    const SrBorrowedU16Span *span) {
    Snes *snes = runner_from_handle(runner);
    const uint16_t *data = NULL;
    uint64_t element_count = 0u;
    if (snes == NULL || span == NULL ||
        span->struct_size < SR_BORROWED_U16_SPAN_V2_SIZE ||
        span->data == NULL ||
        span->lifetime_generation != snes->abiLifetimeGeneration)
        return 0u;
    if (resolve_u16_memory(snes, span->region, &data, &element_count) !=
        SR_RESULT_OK)
        return 0u;
    return span->data == data && span->element_count == element_count ? 1u : 0u;
}

static SrResult query_cpu_state(SrRunnerHandle *runner,
                                SrCpuStateSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_CPU_STATE_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_CPU_STATE_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_CPU_STATE_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    if (s_cpu_state_runner != snes || s_cpu_state_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_cpu_state_provider(s_cpu_state_user_data, out_state);
}

static SrResult query_execution_state(
        SrRunnerHandle *runner, SrExecutionSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_EXECUTION_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_EXECUTION_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_EXECUTION_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    if (s_execution_state_runner != snes ||
        s_execution_state_provider == NULL)
        return SR_RESULT_UNAVAILABLE;
    return s_execution_state_provider(
        s_execution_state_user_data, out_state);
}

