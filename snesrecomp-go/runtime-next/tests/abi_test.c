#include "runner_next.h"
#include "runner_next_internal.h"
#include "snes/ppu.h"
#include "snes/saveload.h"
#include "snes/snes.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { TEST_WRAM_SIZE = 128 * 1024 };

typedef struct SpcPlayer SpcPlayer;
SpcPlayer *g_spc_player;

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static void leave_loaded_state_unchanged(SaveLoadInfo *info, void *data,
                                         size_t size) {
    (void)info;
    (void)data;
    (void)size;
}

static uint8_t s_cpu_component;
static uint8_t s_overlay_pixel;
static uint32_t s_mode7_pixel;
static uint32_t s_main_surface[384u * 253u];
static uint32_t s_authentic_surface[384u * 253u];
static uint32_t s_overlay_surface[384u * 232u];
static uint32_t s_overlay_band_surface[384u * 232u];
static uint32_t s_mode7_surface[768u * 506u];

static SrResult query_test_cpu_state(
        Snes *snes, SrCpuStateSnapshot *out_state) {
    (void)snes;
    out_state->flags = SR_CPU_STATE_M_FLAG | SR_CPU_STATE_EMULATION |
                       SR_CPU_STATE_HOST_RETURN_VALID |
                       SR_CPU_STATE_EXECUTION_PC_VALID;
    out_state->frame_counter = 42u;
    out_state->execution_pc24 = 0x123456u;
    out_state->a = 0x1234u;
    out_state->x = 0x5678u;
    out_state->y = 0x9abcu;
    out_state->s = 0x01efu;
    out_state->d = 0x0020u;
    out_state->db = 0x7eu;
    out_state->pb = 0x03u;
    out_state->p = 0xa5u;
    return SR_RESULT_OK;
}

static int check(int condition, const char *message) {
    if (condition) return 0;
    fprintf(stderr, "runtime-next ABI failed: %s\n", message);
    return 1;
}

static void set_solid_4bpp_tile(Ppu *ppu, unsigned tile, unsigned color) {
    unsigned row;
    for (row = 0u; row < 8u; ++row) {
        uint16_t low = 0u;
        uint16_t high = 0u;
        if ((color & 1u) != 0u) low |= UINT16_C(0x00ff);
        if ((color & 2u) != 0u) low |= UINT16_C(0xff00);
        if ((color & 4u) != 0u) high |= UINT16_C(0x00ff);
        if ((color & 8u) != 0u) high |= UINT16_C(0xff00);
        ppu->vram[tile * 16u + row] = low;
        ppu->vram[tile * 16u + row + 8u] = high;
    }
}

static int check_generation(const SnesRunnerApi *api, SrRunnerHandle *runner,
                            uint64_t lifetime, uint64_t tick, uint64_t reset,
                            uint64_t load, uint64_t mutation) {
    SrGenerationSnapshot generation = {sizeof(generation), 0u, 0u, 0u,
                                       0u, 0u, 0u};
    int failed = 0;
    failed |= check(api->query_generations(runner, &generation) == SR_RESULT_OK,
                    "generation query failed");
    failed |= check(generation.struct_size == sizeof(generation),
                    "generation size changed");
    failed |= check(generation.reserved == 0u, "generation reserved is nonzero");
    failed |= check(generation.lifetime_generation == lifetime,
                    "lifetime generation mismatch");
    failed |= check(generation.tick_generation == tick,
                    "tick generation mismatch");
    failed |= check(generation.reset_generation == reset,
                    "reset generation mismatch");
    failed |= check(generation.load_generation == load,
                    "load generation mismatch");
    failed |= check(generation.mutation_generation == mutation,
                    "mutation generation mismatch");
    return failed;
}

int main(void) {
    static uint8_t wram[TEST_WRAM_SIZE];
    const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    const SrComponentHandle *component = NULL;
    SrGenerationSnapshot too_small = {sizeof(uint32_t), 0u, 0u, 0u,
                                      0u, 0u, 0u};
    SrBorrowedSpan wram_span = {sizeof(wram_span), 0u, NULL, 0u, 0u};
    SrBorrowedSpan apu_span = {sizeof(apu_span), 0u, NULL, 0u, 0u};
    SrBorrowedSpan high_oam_span = {
        sizeof(high_oam_span), 0u, NULL, 0u, 0u};
    SrBorrowedSpan unsupported = {sizeof(unsupported), 0u, NULL, 0u, 0u};
    SrBorrowedSpan too_small_span = {sizeof(uint32_t), 0u, NULL, 0u, 0u};
    SrCpuStateSnapshot cpu_state = {sizeof(cpu_state), 0u, 0u, 0u, 0u,
                                    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    SrCpuStateSnapshot small_cpu_state = {sizeof(uint32_t), 0u, 0u, 0u, 0u,
                                          0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
                                          0u};
    SrBorrowedU16Span vram_span = {
        sizeof(vram_span), 0u, NULL, 0u, 0u};
    SrBorrowedU16Span cgram_span = {
        sizeof(cgram_span), 0u, NULL, 0u, 0u};
    SrBorrowedU16Span oam_span = {
        sizeof(oam_span), 0u, NULL, 0u, 0u};
    SrBorrowedU16Span small_u16_span = {
        sizeof(uint32_t), 0u, NULL, 0u, 0u};
    SrPpuStateSnapshot ppu_state = {sizeof(ppu_state), 0u};
    SrPpuStateSnapshot small_ppu_state = {sizeof(uint32_t), 0u};
    SrPpuFrameSnapshot ppu_frame = {sizeof(ppu_frame), 0u};
    SrPpuFrameSnapshot small_ppu_frame = {sizeof(uint32_t), 0u};
    SrPpuSurfaceSnapshot ppu_surfaces = {sizeof(ppu_surfaces), 0u};
    SrPpuSurfaceSnapshot rebound_surfaces = {sizeof(rebound_surfaces), 0u};
    SrPpuSurfaceSnapshot small_ppu_surfaces = {sizeof(uint32_t), 0u};
    uint32_t obj_pixels[8u * 8u];
    SrPpuObjRasterRequest obj_request = {
        .struct_size = sizeof(obj_request),
        .lifetime_generation = 0u,
        .first_sprite = 0u,
        .sprite_count = 1u,
        .priority = 2u,
        .pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32,
        .pixels = obj_pixels,
        .pixel_byte_size = sizeof(obj_pixels),
        .pitch_bytes = 8u * sizeof(uint32_t),
    };
    SrPpuObjRasterResult obj_result = {sizeof(obj_result), 0u};
    SrPpuObjRasterResult small_obj_result = {sizeof(uint32_t), 0u};
    SaveLoadInfo load = {leave_loaded_state_unchanged, false, false, false};
    Snes *snes;
    SrRunnerHandle *runner;
    int failed = 0;

    failed |= check(offsetof(SnesRunnerApi, abi_version) == 0u,
                    "API version is not the first field");
    failed |= check(offsetof(SnesRunnerApi, struct_size) == sizeof(uint32_t),
                    "API size is not the second field");
    failed |= check(offsetof(SnesRunnerApi, capabilities) == 2u * sizeof(uint32_t),
                    "API capability layout mismatch");
    failed |= check(offsetof(SrBorrowedSpan, struct_size) == 0u,
                    "span size is not the first field");
    failed |= check(offsetof(SrGenerationSnapshot, struct_size) == 0u,
                    "generation size is not the first field");
    failed |= check(SR_BORROWED_SPAN_V1_SIZE <= sizeof(SrBorrowedSpan),
                    "span v1 size exceeds structure");
    failed |= check(SR_GENERATION_SNAPSHOT_V1_SIZE <=
                        sizeof(SrGenerationSnapshot),
                    "generation v1 size exceeds structure");
    failed |= check(SNES_RUNNER_API_V1_SIZE <= sizeof(SnesRunnerApi),
                    "API v1 size exceeds structure");
    failed |= check(SR_CPU_STATE_SNAPSHOT_V1_SIZE <=
                        sizeof(SrCpuStateSnapshot),
                    "CPU snapshot v1 size exceeds structure");
    failed |= check(SNES_RUNNER_API_CPU_STATE_SIZE <= sizeof(SnesRunnerApi),
                    "CPU API extent exceeds structure");
    failed |= check(SR_BORROWED_U16_SPAN_V1_SIZE <=
                        sizeof(SrBorrowedU16Span),
                    "u16 span v1 size exceeds structure");
    failed |= check(SR_PPU_STATE_SNAPSHOT_V1_SIZE <=
                        sizeof(SrPpuStateSnapshot),
                    "PPU snapshot v1 size exceeds structure");
    failed |= check(SNES_RUNNER_API_PPU_STATE_SIZE <= sizeof(SnesRunnerApi),
                    "PPU API extent exceeds structure");
    failed |= check(SR_PPU_FRAME_SNAPSHOT_V1_SIZE <=
                        sizeof(SrPpuFrameSnapshot),
                    "PPU frame snapshot v1 size exceeds structure");
    failed |= check(SNES_RUNNER_API_PPU_FRAME_STATE_SIZE <=
                        sizeof(SnesRunnerApi),
                    "PPU frame API extent exceeds structure");
    failed |= check(SR_PPU_OBJ_RASTER_REQUEST_V1_SIZE <=
                        sizeof(SrPpuObjRasterRequest) &&
                        SR_PPU_OBJ_RASTER_RESULT_V1_SIZE <=
                            sizeof(SrPpuObjRasterResult),
                    "PPU OBJ raster extent exceeds structure");
    failed |= check(SNES_RUNNER_API_PPU_OBJ_RASTER_SIZE <=
                        sizeof(SnesRunnerApi),
                    "PPU OBJ raster API extent exceeds structure");
    failed |= check(SR_PPU_SURFACE_SNAPSHOT_V1_SIZE <=
                        sizeof(SrPpuSurfaceSnapshot),
                    "PPU surface snapshot extent exceeds structure");
    failed |= check(SNES_RUNNER_API_PPU_SURFACE_SIZE <=
                        sizeof(SnesRunnerApi),
                    "PPU surface API extent exceeds structure");
    failed |= check(sizeof(((SrBorrowedSpan *)0)->byte_size) == sizeof(uint64_t),
                    "span size is not fixed width");
    failed |= check(sr_runner_get_api(SR_RUNNER_ABI_VERSION + 1u) == NULL,
                    "unsupported API version accepted");
    failed |= check(api != NULL, "API is null");
    if (api == NULL) return 1;
    failed |= check(api->abi_version == SR_RUNNER_ABI_VERSION,
                    "API version mismatch");
    failed |= check(api->struct_size == sizeof(*api), "API size mismatch");
    failed |= check((api->capabilities & SR_RUNNER_CAP_COMPONENT_HANDLES) != 0u,
                    "component capability missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_GENERATION_COUNTERS) != 0u,
                    "generation capability missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_BORROWED_BYTE_SPANS) != 0u,
                    "borrow capability missing");
    failed |= check((api->capabilities & SR_RUNNER_CAP_CPU_STATE) != 0u,
                    "CPU state capability missing");
    failed |= check((api->capabilities &
                         (SR_RUNNER_CAP_PPU_STATE |
                          SR_RUNNER_CAP_BORROWED_U16_SPANS |
                          SR_RUNNER_CAP_PPU_FRAME_STATE |
                          SR_RUNNER_CAP_PPU_OBJ_RASTER |
                          SR_RUNNER_CAP_PPU_SURFACE_VIEWS)) ==
                        (SR_RUNNER_CAP_PPU_STATE |
                         SR_RUNNER_CAP_BORROWED_U16_SPANS |
                         SR_RUNNER_CAP_PPU_FRAME_STATE |
                         SR_RUNNER_CAP_PPU_OBJ_RASTER |
                         SR_RUNNER_CAP_PPU_SURFACE_VIEWS),
                    "PPU capabilities missing");

    snes = snes_init(wram);
    failed |= check(snes != NULL, "runner allocation failed");
    if (snes == NULL) return 1;
    runner = sr_runner_handle(snes);

    snes->ppu->inidisp = 0x8du;
    snes->ppu->obsel = 0x63u;
    snes->ppu->bgmode = 0x19u;
    snes->ppu->mosaic = 0x31u;
    snes->ppu->bgXsc[0] = 0x63u;
    snes->ppu->bgTileAdr = 0x0005u;
    snes->ppu->hScroll[0] = 0x1234u;
    snes->ppu->vScroll[0] = 0x2345u;
    snes->ppu->screenEnabled[0] = 0x17u;
    snes->ppu->screenEnabled[1] = 0x03u;
    snes->ppu->screenWindowed[0] = 0x02u;
    snes->ppu->screenWindowed[1] = 0x04u;
    snes->ppu->extraLeftCur = 12u;
    snes->ppu->extraRightCur = 13u;
    snes->ppu->extraTopCur = 14u;
    snes->ppu->extraBottomCur = 15u;
    snes->ppu->vram[7] = 0x4567u;
    snes->ppu->cgram[8] = 0x1234u;
    snes->ppu->oam[9] = 0x89abu;
    snes->ppu->highOam[10] = 0xcdu;
    snes->ppu->wsHudSplitHeight = 32u;
    snes->ppu->wsHudLeftEnd = 64u;
    snes->ppu->wsHudRightStart = 192u;
    snes->ppu->wsHudPlayerRowY = 8u;
    snes->ppu->wsHudLeftOnlyY = 16u;
    snes->ppu->extraLeftRight = 48u;
    snes->ppu->overlayCaptures[0].x0 = -12;
    snes->ppu->overlayCaptures[0].x1 = 268;
    snes->ppu->overlayCaptures[0].y0 = -4;
    snes->ppu->overlayCaptures[0].y1 = 228;
    snes->ppu->overlayCaptures[0].flags =
        kPpuOverlayFlag_MarkFullAddSubscreen |
        kPpuOverlayFlag_MarkOwningScreenWinner;
    snes->ppu->overlayCaptures[0].transparentFillMode =
        kPpuOverlayTransparentFill_Cgram;
    snes->ppu->overlayCaptures[0].transparentFillCgram = 8u;
    snes->ppu->overlayCaptures[0].transparentFillConfigured = 1u;
    snes->ppu->overlayCaptures[0].oamFirst = 7u;
    snes->ppu->overlayCaptures[0].oamCount = 9u;
    snes->ppu->overlayRenderBuffer[0] = &s_overlay_pixel;
    snes->ppu->overlayRenderBands[0][0] = &s_overlay_pixel;
    snes->ppu->overlayRenderContentMask[0] = 3u;
    snes->ppu->overlayRenderContentMask[1] = 1u;
    snes->ppu->m7Override.rgba = &s_mode7_pixel;
    failed |= check(api->query_cpu_state(runner, &cpu_state) ==
                        SR_RESULT_UNAVAILABLE,
                    "CPU state without a provider was available");
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) ==
                        SR_RESULT_UNAVAILABLE,
                    "PPU OBJ raster without a provider was available");
    cpu_state.struct_size = sizeof(cpu_state);
    sr_runner_set_cpu_state_provider(
        snes, query_test_cpu_state, &s_cpu_component);
    sr_runner_bind_ppu_services(snes, true);

    failed |= check_generation(api, runner, 0u, 0u, 0u, 0u, 0u);
    failed |= check(api->query_generations(runner, &too_small) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized generation output accepted");
    failed |= check(api->get_component(runner, SR_COMPONENT_PPU, &component) ==
                        SR_RESULT_OK && component != NULL,
                    "PPU component unavailable");
    failed |= check(api->get_component(runner, SR_COMPONENT_CPU, &component) ==
                        SR_RESULT_OK && component != NULL,
                    "CPU component unavailable");
    failed |= check(api->get_component(runner, UINT32_MAX, &component) ==
                        SR_RESULT_UNSUPPORTED && component == NULL,
                    "unknown component accepted");

    failed |= check(api->borrow_memory(runner, SR_MEMORY_WRAM, &wram_span) ==
                        SR_RESULT_OK,
                    "WRAM borrow failed");
    failed |= check(wram_span.data == wram && wram_span.byte_size == sizeof(wram),
                    "WRAM span mismatch");
    failed |= check(api->borrow_is_valid(runner, &wram_span),
                    "new WRAM span is invalid");
    failed |= check(api->borrow_memory(runner, SR_MEMORY_APU_RAM, &apu_span) ==
                        SR_RESULT_UNSUPPORTED && apu_span.data == NULL,
                    "asynchronous APU RAM borrow accepted");
    failed |= check(api->borrow_memory(runner, SR_MEMORY_HIGH_OAM,
                                       &high_oam_span) == SR_RESULT_OK &&
                        high_oam_span.byte_size ==
                            SR_PPU_HIGH_OAM_BYTE_COUNT &&
                        high_oam_span.data[10] == 0xcdu,
                    "high OAM byte borrow mismatch");
    failed |= check(api->borrow_memory(runner, UINT32_MAX, &unsupported) ==
                        SR_RESULT_UNSUPPORTED && unsupported.data == NULL,
                    "unknown memory accepted");
    failed |= check(api->borrow_memory(runner, SR_MEMORY_WRAM,
                                       &too_small_span) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized span output accepted");

    failed |= check(api->borrow_u16_memory(runner, SR_MEMORY_VRAM,
                                           &vram_span) == SR_RESULT_OK &&
                        vram_span.element_count == SR_PPU_VRAM_WORD_COUNT &&
                        vram_span.data[7] == 0x4567u,
                    "VRAM u16 borrow mismatch");
    failed |= check(api->borrow_u16_memory(runner, SR_MEMORY_CGRAM,
                                           &cgram_span) == SR_RESULT_OK &&
                        cgram_span.element_count == SR_PPU_CGRAM_WORD_COUNT &&
                        cgram_span.data[8] == 0x1234u,
                    "CGRAM u16 borrow mismatch");
    failed |= check(api->borrow_u16_memory(runner, SR_MEMORY_OAM,
                                           &oam_span) == SR_RESULT_OK &&
                        oam_span.element_count == SR_PPU_OAM_WORD_COUNT &&
                        oam_span.data[9] == 0x89abu,
                    "OAM u16 borrow mismatch");
    failed |= check(api->borrow_u16_is_valid(runner, &vram_span),
                    "new VRAM span is invalid");
    failed |= check(api->borrow_u16_memory(runner, SR_MEMORY_WRAM,
                                           &small_u16_span) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized u16 span output accepted");

    failed |= check(api->query_ppu_state(runner, &ppu_state) == SR_RESULT_OK,
                    "PPU state query failed");
    failed |= check(ppu_state.display_control == 0x8du &&
                        ppu_state.object_select == 0x63u &&
                        ppu_state.bg_mode_control == 0x19u &&
                        ppu_state.mosaic_control == 0x31u &&
                        ppu_state.bg_mode == 1u &&
                        ppu_state.brightness == 13u,
                    "PPU register snapshot mismatch");
    failed |= check((ppu_state.flags &
                         (SR_PPU_STATE_FORCED_BLANK |
                          SR_PPU_STATE_BG3_PRIORITY)) ==
                        (SR_PPU_STATE_FORCED_BLANK |
                         SR_PPU_STATE_BG3_PRIORITY),
                    "PPU flag snapshot mismatch");
    failed |= check(ppu_state.main_screen == 0x17u &&
                        ppu_state.sub_screen == 0x03u &&
                        ppu_state.main_windowed == 0x02u &&
                        ppu_state.sub_windowed == 0x04u,
                    "PPU screen snapshot mismatch");
    failed |= check(ppu_state.margin_left == 12u &&
                        ppu_state.margin_right == 13u &&
                        ppu_state.margin_top == 14u &&
                        ppu_state.margin_bottom == 15u,
                    "PPU margin snapshot mismatch");
    failed |= check(ppu_state.backgrounds[0].h_scroll == 0x1234u &&
                        ppu_state.backgrounds[0].v_scroll == 0x2345u &&
                        ppu_state.backgrounds[0].tilemap_base_word == 0x6000u &&
                        ppu_state.backgrounds[0].tile_base_word == 0x5000u &&
                        ppu_state.backgrounds[0].tilemap_width_tiles == 64u &&
                        ppu_state.backgrounds[0].tilemap_height_tiles == 64u &&
                        ppu_state.backgrounds[0].tile_size_pixels == 16u &&
                        ppu_state.backgrounds[0].bits_per_pixel == 4u,
                    "PPU background snapshot mismatch");
    failed |= check(api->query_ppu_state(runner, &small_ppu_state) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU snapshot accepted");

    failed |= check(api->query_ppu_frame_state(runner, &ppu_frame) ==
                        SR_RESULT_OK,
                    "PPU frame query failed");
    failed |= check(ppu_frame.display_control == 0x8du &&
                        ppu_frame.bg_mode == 1u &&
                        ppu_frame.hud_split_height == 32u &&
                        ppu_frame.hud_left_end == 64u &&
                        ppu_frame.hud_right_start == 192u &&
                        ppu_frame.hud_player_row_y == 8u &&
                        ppu_frame.hud_left_only_y == 16u &&
                        ppu_frame.margin_budget == 48u &&
                        ppu_frame.mode7_override_active == 1u &&
                        ppu_frame.overlay_count == SR_PPU_OVERLAY_SOURCE_COUNT,
                    "PPU frame scalar snapshot mismatch");
    failed |= check(ppu_frame.overlays[0].x0 == -12 &&
                        ppu_frame.overlays[0].x1 == 268 &&
                        ppu_frame.overlays[0].y0 == -4 &&
                        ppu_frame.overlays[0].y1 == 228 &&
                        ppu_frame.overlays[0].flags ==
                            (SR_PPU_OVERLAY_MARK_FULL_ADD_SUBSCREEN |
                             SR_PPU_OVERLAY_MARK_OWNING_SCREEN_WINNER) &&
                        ppu_frame.overlays[0].content_band_mask == 3u &&
                        ppu_frame.overlays[0].transparent_fill_argb ==
                            UINT32_C(0xff8f791c) &&
                        ppu_frame.overlays[0].transparent_fill_configured == 1u &&
                        ppu_frame.overlays[0].oam_first == 7u &&
                        ppu_frame.overlays[0].oam_count == 9u,
                    "PPU overlay snapshot mismatch");
    failed |= check(ppu_frame.overlays[1].content_band_mask == 0u,
                    "unbound overlay surface reported content");
    failed |= check(api->query_ppu_frame_state(runner, &small_ppu_frame) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU frame snapshot accepted");

    PpuBeginDrawing(snes->ppu, (uint8_t *)s_main_surface,
                    384u * sizeof(uint32_t), 0u);
    failed |= check(PpuBindAuthenticSurface(
                        snes->ppu, (uint8_t *)s_authentic_surface,
                        384u * sizeof(uint32_t)),
                    "authentic surface bind failed");
    failed |= check(PpuBindOverlaySurface(
                        snes->ppu, kPpuOverlaySource_Bg1,
                        (uint8_t *)s_overlay_surface,
                        384u * sizeof(uint32_t)),
                    "overlay surface bind failed");
    failed |= check(PpuBindOverlayPrioSurface(
                        snes->ppu, kPpuOverlaySource_Bg1, 1,
                        (uint8_t *)s_overlay_band_surface),
                    "overlay band surface bind failed");
    snes->ppu->overlayRenderContentMask[0] = 3u;
    failed |= check(PpuBindMode7OverlaySurface(
                        snes->ppu, (uint8_t *)s_mode7_surface,
                        768u * sizeof(uint32_t), 2u),
                    "Mode-7 surface bind failed");
    failed |= check(api->query_ppu_surfaces(runner, &ppu_surfaces) ==
                        SR_RESULT_OK,
                    "PPU surface query failed");
    failed |= check(ppu_surfaces.overlay_count ==
                            SR_PPU_OVERLAY_SOURCE_COUNT &&
                        ppu_surfaces.band_count == SR_PPU_SURFACE_BAND_COUNT &&
                        ppu_surfaces.binding_generation != 0u,
                    "PPU surface snapshot header mismatch");
    failed |= check(ppu_surfaces.main.data ==
                            (const uint8_t *)s_main_surface &&
                        ppu_surfaces.main.pitch_bytes ==
                            384u * sizeof(uint32_t) &&
                        ppu_surfaces.main.width_pixels == 384u &&
                        ppu_surfaces.main.height_pixels == 253u &&
                        ppu_surfaces.main.origin_x == 64 &&
                        ppu_surfaces.main.origin_y == 14 &&
                        ppu_surfaces.main.scale == 1u &&
                        ppu_surfaces.main.flags ==
                            (SR_PPU_SURFACE_BOUND |
                             SR_PPU_SURFACE_HAS_CONTENT),
                    "main PPU surface mismatch");
    failed |= check(ppu_surfaces.authentic.data ==
                            (const uint8_t *)s_authentic_surface &&
                        ppu_surfaces.authentic.byte_size ==
                            384u * 253u * sizeof(uint32_t),
                    "authentic PPU surface mismatch");
    failed |= check(ppu_surfaces.overlays[0][0].data ==
                            (const uint8_t *)s_overlay_surface &&
                        ppu_surfaces.overlays[0][0].height_pixels == 232u &&
                        ppu_surfaces.overlays[0][0].origin_x == 64 &&
                        ppu_surfaces.overlays[0][0].origin_y == 4 &&
                        (ppu_surfaces.overlays[0][0].flags &
                         SR_PPU_SURFACE_HAS_CONTENT) != 0u &&
                        ppu_surfaces.overlays[0][1].data ==
                            (const uint8_t *)s_overlay_band_surface &&
                        (ppu_surfaces.overlays[0][1].flags &
                         SR_PPU_SURFACE_HAS_CONTENT) != 0u &&
                        ppu_surfaces.overlays[1][0].data == NULL,
                    "overlay PPU surface mismatch");
    failed |= check(ppu_surfaces.mode7.data ==
                            (const uint8_t *)s_mode7_surface &&
                        ppu_surfaces.mode7.width_pixels == 768u &&
                        ppu_surfaces.mode7.height_pixels == 506u &&
                        ppu_surfaces.mode7.origin_x == 128 &&
                        ppu_surfaces.mode7.origin_y == 28 &&
                        ppu_surfaces.mode7.scale == 2u &&
                        (ppu_surfaces.mode7.flags &
                         SR_PPU_SURFACE_HAS_CONTENT) != 0u,
                    "Mode-7 PPU surface mismatch");
    failed |= check(api->ppu_surface_snapshot_is_valid(
                        runner, &ppu_surfaces),
                    "new PPU surface snapshot is invalid");
    failed |= check(api->query_ppu_surfaces(runner, &small_ppu_surfaces) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU surface snapshot accepted");
    failed |= check(PpuBindOverlaySurface(
                        snes->ppu, kPpuOverlaySource_Bg1,
                        (uint8_t *)s_overlay_surface,
                        384u * sizeof(uint32_t)),
                    "overlay surface rebind failed");
    failed |= check(!api->ppu_surface_snapshot_is_valid(
                        runner, &ppu_surfaces),
                    "surface rebind did not expire PPU surface snapshot");
    failed |= check(api->query_ppu_surfaces(runner, &rebound_surfaces) ==
                        SR_RESULT_OK &&
                        api->ppu_surface_snapshot_is_valid(
                            runner, &rebound_surfaces),
                    "PPU surface snapshot requery failed");

    snes->ppu->inidisp = 0x0fu;
    snes->ppu->obsel = 0u;
    snes->ppu->cgram[0x81] = 0x001fu;
    set_solid_4bpp_tile(snes->ppu, 0u, 1u);
    snes->ppu->oam[0] = 10u | (20u << 8);
    snes->ppu->oam[1] = 2u << 12;
    memset(obj_pixels, 0x5a, sizeof(obj_pixels));
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) == SR_RESULT_OK,
                    "PPU OBJ raster failed");
    failed |= check(obj_result.lifetime_generation == 0u &&
                        obj_result.x0 == 10 && obj_result.y0 == 20 &&
                        obj_result.x1 == 18 && obj_result.y1 == 28 &&
                        obj_result.width == 8u && obj_result.height == 8u,
                    "PPU OBJ raster bounds mismatch");
    failed |= check(obj_pixels[0] == UINT32_C(0xffff0000) &&
                        obj_pixels[63] == UINT32_C(0xffff0000),
                    "PPU OBJ raster pixels mismatch");
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &small_obj_result) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU OBJ raster result accepted");
    obj_request.pixel_byte_size = sizeof(uint32_t);
    obj_result.struct_size = sizeof(obj_result);
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized PPU OBJ raster buffer accepted");
    obj_request.pixel_byte_size = sizeof(obj_pixels);
    obj_request.pixel_format = UINT32_MAX;
    obj_result.struct_size = sizeof(obj_result);
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) ==
                        SR_RESULT_UNSUPPORTED,
                    "unknown PPU OBJ pixel format accepted");
    obj_request.pixel_format = SR_PPU_OBJ_PIXEL_FORMAT_ARGB8888_U32;

    failed |= check(api->query_cpu_state(runner, &cpu_state) == SR_RESULT_OK,
                    "CPU state query failed");
    failed |= check(cpu_state.a == 0x1234u && cpu_state.x == 0x5678u &&
                        cpu_state.y == 0x9abcu && cpu_state.s == 0x01efu &&
                        cpu_state.d == 0x0020u,
                    "CPU 16-bit register snapshot mismatch");
    failed |= check(cpu_state.db == 0x7eu && cpu_state.pb == 0x03u &&
                        cpu_state.p == 0xa5u,
                    "CPU 8-bit register snapshot mismatch");
    failed |= check(cpu_state.lifetime_generation == 0u &&
                        cpu_state.frame_counter == 42u &&
                        cpu_state.execution_pc24 == 0x123456u,
                    "CPU execution metadata mismatch");
    failed |= check((cpu_state.flags &
                         (SR_CPU_STATE_M_FLAG | SR_CPU_STATE_EMULATION |
                          SR_CPU_STATE_HOST_RETURN_VALID |
                          SR_CPU_STATE_EXECUTION_PC_VALID)) ==
                        (SR_CPU_STATE_M_FLAG | SR_CPU_STATE_EMULATION |
                         SR_CPU_STATE_HOST_RETURN_VALID |
                         SR_CPU_STATE_EXECUTION_PC_VALID) &&
                        (cpu_state.flags & SR_CPU_STATE_X_FLAG) == 0u,
                    "CPU flag snapshot mismatch");
    failed |= check(api->query_cpu_state(runner, &small_cpu_state) ==
                        SR_RESULT_INVALID_ARGUMENT,
                    "undersized CPU snapshot accepted");

    sr_runner_note_tick(snes);
    failed |= check_generation(api, runner, 1u, 1u, 0u, 0u, 0u);
    failed |= check(!api->borrow_is_valid(runner, &wram_span),
                    "tick did not expire WRAM span");
    failed |= check(!api->borrow_u16_is_valid(runner, &vram_span),
                    "tick did not expire VRAM span");
    failed |= check(!api->ppu_surface_snapshot_is_valid(
                        runner, &rebound_surfaces),
                    "tick did not expire PPU surface snapshot");
    obj_result.struct_size = sizeof(obj_result);
    failed |= check(api->rasterize_ppu_obj_range(
                        runner, &obj_request, &obj_result) ==
                        SR_RESULT_STALE_VIEW,
                    "stale PPU OBJ raster request accepted");

    wram_span.struct_size = sizeof(wram_span);
    failed |= check(api->borrow_memory(runner, SR_MEMORY_WRAM, &wram_span) ==
                        SR_RESULT_OK && api->borrow_is_valid(runner, &wram_span),
                    "WRAM reborrow failed");
    snes_reset(snes, false);
    failed |= check_generation(api, runner, 2u, 1u, 1u, 0u, 0u);
    failed |= check(!api->borrow_is_valid(runner, &wram_span),
                    "reset did not expire WRAM span");

    snes_saveload(snes, &load);
    failed |= check_generation(api, runner, 3u, 1u, 1u, 1u, 0u);
    sr_runner_note_mutation(snes);
    failed |= check_generation(api, runner, 4u, 1u, 1u, 1u, 1u);

    sr_runner_set_cpu_state_provider(snes, NULL, NULL);
    sr_runner_bind_ppu_services(snes, false);
    snes_free(snes);
    memset(wram, 0, sizeof(wram));
    return failed;
}
