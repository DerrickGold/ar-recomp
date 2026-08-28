#include "example_game.h"

#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/required_symbols.h"
#include "snesrecomp/game_runtime.h"

#include <stdbool.h>

/* These no-ops are valid only while every APU path is confined to the
 * emulation thread. A frontend with an audio thread must use its real lock. */
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

static void ExampleRunFrame(void) {
    /* Enter the recompiled game's host-resumable frame loop here. */
}

static const RtlGameIdentity kExampleIdentity = {
    .struct_size = RTL_GAME_IDENTITY_V1_SIZE,
    .game_id = "example_game",
    .display_name = "Example Game",
    .save_name_prefix = "example",
};

static const RtlGameExecutionApi kExampleExecution = {
    .struct_size = RTL_GAME_EXECUTION_API_V2_SIZE,
    .run_frame = ExampleRunFrame,
};

static const RtlGameModule kExampleModule = {
    .abi_version = RTL_GAME_MODULE_ABI_VERSION,
    .struct_size = RTL_GAME_MODULE_V2_SIZE,
    .capabilities = RTL_GAME_MODULE_CAP_IDENTITY |
                    RTL_GAME_MODULE_CAP_EXECUTION,
    .identity = &kExampleIdentity,
    .execution = &kExampleExecution,
};

SrResult ExampleRegisterGame(void) {
    return RtlRegisterGame(&kExampleModule);
}

static bool ApiHas(const SnesRunnerApi *api, uint64_t capability,
                   uint32_t minimum_size) {
    return api != NULL && api->abi_version == SR_RUNNER_ABI_VERSION &&
           api->struct_size >= minimum_size &&
           (api->capabilities & capability) != 0u;
}

SrResult ExampleBeginAuthenticFrame(void) {
    SrRunnerHandle *runner = RtlGameRunner();
    const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    SrGenerationSnapshot generations = {
        .struct_size = SR_GENERATION_SNAPSHOT_V2_SIZE,
    };
    SrPpuFrameResetRequest reset = {
        .struct_size = SR_PPU_FRAME_RESET_REQUEST_V2_SIZE,
    };
    SrPpuFramePolicyRequest policy = {
        .struct_size = SR_PPU_FRAME_POLICY_REQUEST_V2_SIZE,
        .policy = {
            .struct_size = SR_PPU_FRAME_POLICY_V2_SIZE,
            /* Intentional: this example establishes authentic-width scanout.
             * CENTERED may reserve a future wide allocation, but it does not
             * make side pixels rasterizable. Widescreen producers that want
             * ordinary PPU content in the margins use AVAILABLE instead. */
            .horizontal_mode = SR_PPU_HORIZONTAL_MARGIN_CENTERED,
        },
    };
    SrResult result;

    if (runner == NULL ||
        !ApiHas(api, SR_RUNNER_CAP_GENERATION_COUNTERS,
                SNES_RUNNER_API_V2_BASE_SIZE) ||
        !ApiHas(api, SR_RUNNER_CAP_PPU_FRAME_RESET,
                SNES_RUNNER_API_PPU_FRAME_RESET_SIZE) ||
        !ApiHas(api, SR_RUNNER_CAP_PPU_FRAME_POLICY,
                SNES_RUNNER_API_PPU_FRAME_POLICY_SIZE)) {
        return SR_RESULT_UNSUPPORTED;
    }

    result = api->query_generations(runner, &generations);
    if (result != SR_RESULT_OK) return result;

    reset.lifetime_generation = generations.lifetime_generation;
    result = api->reset_ppu_frame_state(runner, &reset);
    if (result != SR_RESULT_OK) return result;

    policy.lifetime_generation = generations.lifetime_generation;
    return api->apply_ppu_frame_policy(runner, &policy);
}
