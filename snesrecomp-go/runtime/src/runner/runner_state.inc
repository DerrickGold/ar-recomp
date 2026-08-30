/* Unity-build domain fragment. Included only by runner.c. */
static uint32_t game_timing_state(const Snes *snes) {
    return ((uint32_t)snes->forceNmi << 0) |
        ((uint32_t)snes->nmiAvail << 1) |
        ((uint32_t)snes->nmiEnabled << 2) |
        ((uint32_t)snes->inNmi << 3);
}

uint32_t sr_runner_transition_game_timing(
        Snes *snes, SrGameTimingOperation operation, uint32_t flags) {
    uint32_t transition_flags = 0u;
    if (snes == NULL) return 0u;

    if (operation == SR_GAME_TIMING_BEGIN_FRAME_SLICE) {
        snes_beginVblank(snes);
        snes->forceNmi = true;
        snes->nmiAvail = true;
        if (sr_runner_event_enabled(SR_EVENT_MASK_FRAME)) {
            sr_runner_emit_frame_boundary(
                snes,
                SR_EVENT_FRAME_BEGIN | SR_EVENT_FRAME_VBLANK |
                    SR_EVENT_FRAME_GAME_SLICE,
                "game-slice-begin");
        }
    } else {
        const bool enter_nmi =
            (flags & SR_GAME_TIMING_DISPATCH_NMI_IF_ENABLED) != 0u &&
            snes->nmiEnabled;
        snes->forceNmi = false;
        snes->inNmi = snes->inNmi || enter_nmi;
        transition_flags = enter_nmi ?
            SR_GAME_TIMING_TRANSITION_NMI_ENTERED : 0u;
        if (sr_runner_event_enabled(SR_EVENT_MASK_FRAME)) {
            sr_runner_emit_frame_boundary(
                snes, SR_EVENT_FRAME_END | SR_EVENT_FRAME_GAME_SLICE,
                "game-slice-end");
        }
        if (enter_nmi &&
            sr_runner_event_enabled(SR_EVENT_MASK_INTERRUPT)) {
            sr_runner_emit_interrupt(
                snes, SR_INTERRUPT_NMI,
                SR_EVENT_INTERRUPT_ENTER |
                    SR_EVENT_INTERRUPT_TRANSITION,
                0u, 0u, SR_INTERRUPT_SCANLINE_UNKNOWN,
                "nmi-transition");
        }
    }
    return transition_flags;
}

static SrResult control_game_timing(
        SrRunnerHandle *runner,
        const SrGameTimingRequest *request,
        SrGameTimingResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    uint32_t transition_flags;
    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_GAME_TIMING_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_GAME_TIMING_RESULT_V2_SIZE ||
        (request->flags & ~SR_GAME_TIMING_REQUEST_FLAGS_SUPPORTED) != 0u ||
        request->reserved != 0u ||
        (request->operation != SR_GAME_TIMING_BEGIN_FRAME_SLICE &&
         request->operation != SR_GAME_TIMING_COMPLETE_FRAME_SLICE) ||
        (request->operation == SR_GAME_TIMING_BEGIN_FRAME_SLICE &&
         request->flags != 0u))
        return SR_RESULT_INVALID_ARGUMENT;

    transition_flags = sr_runner_transition_game_timing(
        snes, request->operation, request->flags);

    memset(out_result, 0, SR_GAME_TIMING_RESULT_V2_SIZE);
    out_result->struct_size = SR_GAME_TIMING_RESULT_V2_SIZE;
    out_result->state_flags = game_timing_state(snes);
    out_result->transition_flags = transition_flags;
    return SR_RESULT_OK;
}

static SrResult query_input_state(
        SrRunnerHandle *runner, SrInputStateSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_INPUT_STATE_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_INPUT_STATE_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_INPUT_STATE_SNAPSHOT_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    out_state->frame_counter = snes->abiFrameCounter;
    out_state->packed_buttons[0] = snes->input1_currentState;
    out_state->packed_buttons[1] = snes->input2_currentState;
    out_state->auto_joypad[0] = SwapInputBits(snes->input1_currentState);
    out_state->auto_joypad[1] = SwapInputBits(snes->input2_currentState);
    return SR_RESULT_OK;
}

static SrResult query_cpu_math_state(SrRunnerHandle *runner,
                                     SrCpuMathState *out_state);
static SrResult restore_cpu_math_state(SrRunnerHandle *runner,
                                       const SrCpuMathState *state);

static const SnesRunnerApi k_runner_api = {
    SR_RUNNER_ABI_VERSION,
    (uint32_t)sizeof(SnesRunnerApi),
    SR_RUNNER_CAP_COMPONENT_HANDLES |
        SR_RUNNER_CAP_GENERATION_COUNTERS |
        SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
        SR_RUNNER_CAP_CPU_STATE |
        SR_RUNNER_CAP_PPU_STATE |
        SR_RUNNER_CAP_BORROWED_U16_SPANS |
        SR_RUNNER_CAP_PPU_FRAME_STATE |
        SR_RUNNER_CAP_PPU_OBJ_RASTER |
        SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
        SR_RUNNER_CAP_EXECUTION_STATE |
        SR_RUNNER_CAP_EVENT_OBSERVERS |
        SR_RUNNER_CAP_SAFE_POINT_MUTATIONS |
        SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE |
        SR_RUNNER_CAP_PPU_OUTPUT_CONTROL |
        SR_RUNNER_CAP_PPU_CAPTURE_CONTROL |
        SR_RUNNER_CAP_CPU_MATH_STATE |
        SR_RUNNER_CAP_AUDIO_TRACE_OBSERVERS |
        SR_RUNNER_CAP_SPC_CONTROL |
        SR_RUNNER_CAP_AUDIO_MIX_CONTROL |
        SR_RUNNER_CAP_PPU_FRAME_TRANSACTIONS |
        SR_RUNNER_CAP_PPU_VRAM_PATCH |
        SR_RUNNER_CAP_PPU_OBJ_METADATA |
        SR_RUNNER_CAP_DMA_STATE |
        SR_RUNNER_CAP_PPU_BACKGROUND_POLICY |
        SR_RUNNER_CAP_PPU_SCANOUT |
        SR_RUNNER_CAP_GAME_TIMING_CONTROL |
        SR_RUNNER_CAP_INPUT_STATE |
        SR_RUNNER_CAP_PPU_FRAME_POLICY |
        SR_RUNNER_CAP_PPU_FRAME_RESET |
        SR_RUNNER_CAP_PPU_OBJ_CAPTURE |
        SR_RUNNER_CAP_APU_STATE_SNAPSHOT,
    get_component,
    query_generations,
    borrow_memory,
    borrow_is_valid,
    query_cpu_state,
    query_ppu_state,
    borrow_u16_memory,
    borrow_u16_is_valid,
    query_ppu_frame_state,
    rasterize_ppu_obj_range,
    resolve_ppu_obj_range,
    rasterize_ppu_obj_parts,
    query_ppu_surfaces,
    ppu_surface_snapshot_is_valid,
    query_execution_state,
    subscribe_events,
    unsubscribe_events,
    queue_mutation,
    query_mutation,
    resolve_ppu_background_coordinate,
    bind_ppu_output_surface,
    configure_ppu_horizontal_margin,
    claim_ppu_overlay_capture,
    claim_ppu_mode7_override,
    query_cpu_math_state,
    restore_cpu_math_state,
    sr_runner_subscribe_audio_trace,
    sr_runner_unsubscribe_audio_trace,
    sr_runner_compare_exchange_spc_pc,
    sr_runner_configure_audio_mix,
    visit_ppu_frame_transaction,
    compare_exchange_ppu_overlay_captures,
    compare_exchange_ppu_vram_words,
    update_ppu_obj_metadata,
    query_dma_state,
    update_ppu_layer_extents,
    replace_ppu_virtual_tilemaps,
    update_ppu_authentic_camera,
    run_ppu_scanout,
    control_game_timing,
    query_input_state,
    apply_ppu_frame_policy,
    reset_ppu_frame_state,
    configure_ppu_obj_capture,
    sr_runner_query_apu_state,
};

/* Keep synchronized with the source boundary in runner.cmake. */
static const SrRunnerDescriptor k_runner = {
    .abi_version = SR_RUNNER_ABI_VERSION,
    .struct_size = (uint32_t)sizeof(SrRunnerDescriptor),
    .capabilities = SR_RUNNER_CAP_COMPONENT_HANDLES |
        SR_RUNNER_CAP_GENERATION_COUNTERS |
        SR_RUNNER_CAP_BORROWED_BYTE_SPANS |
        SR_RUNNER_CAP_CPU_STATE |
        SR_RUNNER_CAP_PPU_STATE |
        SR_RUNNER_CAP_BORROWED_U16_SPANS |
        SR_RUNNER_CAP_PPU_FRAME_STATE |
        SR_RUNNER_CAP_PPU_OBJ_RASTER |
        SR_RUNNER_CAP_PPU_SURFACE_VIEWS |
        SR_RUNNER_CAP_EXECUTION_STATE |
        SR_RUNNER_CAP_EVENT_OBSERVERS |
        SR_RUNNER_CAP_SAFE_POINT_MUTATIONS |
        SR_RUNNER_CAP_PPU_BACKGROUND_COORDINATE |
        SR_RUNNER_CAP_PPU_OUTPUT_CONTROL |
        SR_RUNNER_CAP_PPU_CAPTURE_CONTROL |
        SR_RUNNER_CAP_CPU_MATH_STATE |
        SR_RUNNER_CAP_AUDIO_TRACE_OBSERVERS |
        SR_RUNNER_CAP_SPC_CONTROL |
        SR_RUNNER_CAP_AUDIO_MIX_CONTROL |
        SR_RUNNER_CAP_PPU_FRAME_TRANSACTIONS |
        SR_RUNNER_CAP_PPU_VRAM_PATCH |
        SR_RUNNER_CAP_PPU_OBJ_METADATA |
        SR_RUNNER_CAP_DMA_STATE |
        SR_RUNNER_CAP_PPU_BACKGROUND_POLICY |
        SR_RUNNER_CAP_PPU_SCANOUT |
        SR_RUNNER_CAP_GAME_TIMING_CONTROL |
        SR_RUNNER_CAP_INPUT_STATE |
        SR_RUNNER_CAP_PPU_FRAME_POLICY |
        SR_RUNNER_CAP_PPU_FRAME_RESET |
        SR_RUNNER_CAP_PPU_OBJ_CAPTURE |
        SR_RUNNER_CAP_APU_STATE_SNAPSHOT,
    .implementation = "portable",
};

const SrRunnerDescriptor *sr_runner_descriptor(void) {
    return &k_runner;
}

const SnesRunnerApi *sr_runner_get_api(uint32_t requested_abi_version) {
    if (requested_abi_version != SR_RUNNER_ABI_VERSION) return NULL;
    return &k_runner_api;
}

SrRunnerHandle *sr_runner_handle(Snes *snes) {
    return (SrRunnerHandle *)(void *)snes;
}

void sr_runner_set_cpu_state_provider(
        Snes *snes, RtlGameCpuStateQueryFunc *provider, void *user_data,
        const void *component_handle) {
    if (provider == NULL && s_cpu_state_runner != snes) return;
    s_cpu_state_runner = provider != NULL ? snes : NULL;
    s_cpu_state_provider = provider;
    s_cpu_state_user_data = provider != NULL ? user_data : NULL;
    s_cpu_component = provider != NULL ? component_handle : NULL;
}

void sr_runner_set_execution_state_provider(
        Snes *snes, RtlGameExecutionStateQueryFunc *provider,
        void *user_data) {
    if (provider == NULL && s_execution_state_runner != snes) return;
    s_execution_state_runner = provider != NULL ? snes : NULL;
    s_execution_state_provider = provider;
    s_execution_state_user_data = provider != NULL ? user_data : NULL;
}

void sr_runner_set_ppu_obj_raster_provider(
        Snes *snes, SrRunnerPpuObjRasterProvider *provider) {
    if (provider == NULL && s_ppu_obj_raster_runner != snes) return;
    s_ppu_obj_raster_runner = provider != NULL ? snes : NULL;
    s_ppu_obj_raster_provider = provider;
}

void sr_runner_set_ppu_obj_resolve_provider(
        Snes *snes, SrRunnerPpuObjResolveProvider *provider) {
    if (provider == NULL && s_ppu_obj_resolve_runner != snes) return;
    s_ppu_obj_resolve_runner = provider != NULL ? snes : NULL;
    s_ppu_obj_resolve_provider = provider;
}

void sr_runner_set_ppu_obj_parts_raster_provider(
        Snes *snes, SrRunnerPpuObjPartsRasterProvider *provider) {
    if (provider == NULL && s_ppu_obj_parts_raster_runner != snes) return;
    s_ppu_obj_parts_raster_runner = provider != NULL ? snes : NULL;
    s_ppu_obj_parts_raster_provider = provider;
}

void sr_runner_set_ppu_scanout_provider(
        Snes *snes, SrRunnerPpuScanoutProvider *provider) {
    if (provider == NULL && s_ppu_scanout_runner != snes) return;
    s_ppu_scanout_runner = provider != NULL ? snes : NULL;
    s_ppu_scanout_provider = provider;
}

static void invalidate_lifetime(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiLifetimeGeneration;
    snes->abiPpuFramePolicyActive = false;
}

void sr_runner_note_tick(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiTickGeneration;
    invalidate_lifetime(snes);
}

void sr_runner_note_reset(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiResetGeneration;
    invalidate_lifetime(snes);
}

void sr_runner_note_load(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiLoadGeneration;
    invalidate_lifetime(snes);
}

void sr_runner_note_mutation(Snes *snes) {
    if (snes == NULL) return;
    ++snes->abiMutationGeneration;
    invalidate_lifetime(snes);
}

static SrResult query_cpu_math_state(SrRunnerHandle *runner,
                                     SrCpuMathState *out_state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || out_state == NULL ||
        out_state->struct_size < SR_CPU_MATH_STATE_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_CPU_MATH_STATE_V2_SIZE);
    out_state->struct_size = SR_CPU_MATH_STATE_V2_SIZE;
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    out_state->multiply_operand = snes->multiplyA;
    out_state->multiply_or_remainder_result = snes->multiplyResult;
    out_state->divide_dividend = snes->divideA;
    out_state->divide_quotient = snes->divideResult;
    return SR_RESULT_OK;
}

static SrResult restore_cpu_math_state(SrRunnerHandle *runner,
                                       const SrCpuMathState *state) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || state == NULL ||
        state->struct_size < SR_CPU_MATH_STATE_V2_SIZE ||
        state->flags != 0u || state->reserved8 != 0u ||
        state->reserved32 != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (state->lifetime_generation != snes->abiLifetimeGeneration)
        return SR_RESULT_STALE_VIEW;
    sr_runner_note_mutation(snes);
    snes->multiplyA = state->multiply_operand;
    snes->multiplyResult = state->multiply_or_remainder_result;
    snes->divideA = state->divide_dividend;
    snes->divideResult = state->divide_quotient;
    return SR_RESULT_OK;
}
