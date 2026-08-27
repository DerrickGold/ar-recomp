#include "runner_next_internal.h"

#include "apu_sync.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/snes.h"
#include "snes/spc.h"

#include <string.h>

_Static_assert(sizeof(((Apu *)0)->ram) == SR_APU_RAM_BYTE_COUNT,
               "public ABI ARAM extent must match the APU");

static Snes *runner_from_handle(SrRunnerHandle *runner) {
    return (Snes *)(void *)runner;
}

SrResult sr_runner_compare_exchange_spc_pc(
        SrRunnerHandle *runner, const SrSpcPcControlRequest *request,
        SrSpcPcControlResult *out_result) {
    Snes *snes = runner_from_handle(runner);
    Apu *apu;
    Spc *spc;
    uint16_t observed_pc;
    uint32_t matched;

    if (snes == NULL || request == NULL || out_result == NULL ||
        request->struct_size < SR_SPC_PC_CONTROL_REQUEST_V2_SIZE ||
        out_result->struct_size < SR_SPC_PC_CONTROL_RESULT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_result, 0, SR_SPC_PC_CONTROL_RESULT_V2_SIZE);
    out_result->struct_size = SR_SPC_PC_CONTROL_RESULT_V2_SIZE;
    if (request->flags != 0u || request->expected_pc_low >
            request->expected_pc_high ||
        request->expected_aram_count > SR_SPC_PC_EXPECTED_ARAM_MAX ||
        request->reserved8[0] != 0u || request->reserved8[1] != 0u ||
        request->reserved8[2] != 0u ||
        (uint32_t)request->expected_aram_address +
                request->expected_aram_count > SR_APU_RAM_BYTE_COUNT)
        return SR_RESULT_INVALID_ARGUMENT;

    apu = snes->apu;
    if (apu == NULL || apu->spc == NULL) return SR_RESULT_UNAVAILABLE;
    spc = apu->spc;
    RtlApuLock();
    observed_pc = spc->pc;
    matched = observed_pc >= request->expected_pc_low &&
        observed_pc <= request->expected_pc_high &&
        memcmp(apu->ram + request->expected_aram_address,
               request->expected_aram,
               request->expected_aram_count) == 0;
    out_result->observed_pc = observed_pc;
    if (matched) {
        sr_runner_note_mutation(snes);
        spc->pc = request->replacement_pc;
        out_result->flags = SR_SPC_PC_CONTROL_MATCHED |
                            SR_SPC_PC_CONTROL_WRITTEN;
    }
    out_result->current_pc = spc->pc;
    RtlApuUnlock();
    return SR_RESULT_OK;
}

SrResult sr_runner_configure_audio_mix(
        SrRunnerHandle *runner, const SrAudioMixControl *control) {
    Snes *snes = runner_from_handle(runner);
    if (snes == NULL || control == NULL ||
        control->struct_size < SR_AUDIO_MIX_CONTROL_V2_SIZE ||
        control->flags != 0u || control->music_gain_percent > 100u ||
        control->sfx_gain_percent > 100u || control->reserved[0] != 0u ||
        control->reserved[1] != 0u)
        return SR_RESULT_INVALID_ARGUMENT;
    if (snes->apu == NULL || snes->apu->dsp == NULL)
        return SR_RESULT_UNAVAILABLE;
    RtlApuLock();
    dsp_setBusGains((int)control->music_gain_percent,
                    (int)control->sfx_gain_percent);
    RtlApuUnlock();
    return SR_RESULT_OK;
}
