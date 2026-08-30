#include "runner_internal.h"

#include "snesrecomp/game/apu_sync.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/snes.h"
#include "snes/spc.h"

#include <string.h>

_Static_assert(sizeof(((Apu *)0)->ram) == SR_APU_RAM_BYTE_COUNT,
               "public ABI ARAM extent must match the APU");
_Static_assert(sizeof(((Dsp *)0)->ram) == SR_DSP_REGISTER_BYTE_COUNT,
               "public ABI DSP register extent must match the DSP");
_Static_assert(kDspHardwareVoiceCount == SR_DSP_HARDWARE_VOICE_COUNT &&
                   kDspExtendedVoiceCount == SR_DSP_EXTENDED_VOICE_COUNT,
               "public ABI voice counts must match the DSP");

static Snes *runner_from_handle(SrRunnerHandle *runner) {
    return (Snes *)(void *)runner;
}

static uint8_t spc_status(const Spc *spc) {
    return (uint8_t)((spc->c ? 0x01u : 0u) |
                     (spc->z ? 0x02u : 0u) |
                     (spc->i ? 0x04u : 0u) |
                     (spc->h ? 0x08u : 0u) |
                     (spc->b ? 0x10u : 0u) |
                     (spc->p ? 0x20u : 0u) |
                     (spc->v ? 0x40u : 0u) |
                     (spc->n ? 0x80u : 0u));
}

SrResult sr_runner_query_apu_state(
        SrRunnerHandle *runner, const SrApuStateQuery *query,
        SrApuStateSnapshot *out_state) {
    Snes *snes = runner_from_handle(runner);
    Apu *apu;
    Dsp *dsp;
    Spc *spc;
    unsigned index;

    if (snes == NULL || query == NULL || out_state == NULL ||
        query->struct_size < SR_APU_STATE_QUERY_V2_SIZE ||
        out_state->struct_size < SR_APU_STATE_SNAPSHOT_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    memset(out_state, 0, SR_APU_STATE_SNAPSHOT_V2_SIZE);
    out_state->struct_size = SR_APU_STATE_SNAPSHOT_V2_SIZE;
    if (query->flags != 0u || query->reserved[0] != 0u ||
        query->reserved[1] != 0u || query->apu_ram == NULL ||
        query->apu_ram_capacity < SR_APU_RAM_BYTE_COUNT ||
        query->dsp_registers == NULL ||
        query->dsp_register_capacity < SR_DSP_REGISTER_BYTE_COUNT)
        return SR_RESULT_INVALID_ARGUMENT;
    if (sr_runner_audio_query_forbidden()) return SR_RESULT_BUSY;
    apu = snes->apu;
    if (apu == NULL || apu->dsp == NULL || apu->spc == NULL)
        return SR_RESULT_UNAVAILABLE;

    RtlApuLock();
    dsp = apu->dsp;
    spc = apu->spc;
    memcpy(query->apu_ram, apu->ram, sizeof(apu->ram));
    dsp_copyRegisters(dsp, query->dsp_registers);
    out_state->flags = (spc->stopped ? SR_APU_STATE_SPC_STOPPED : 0u) |
        (dsp->mute ? SR_APU_STATE_DSP_MUTED : 0u) |
        (dsp->reset ? SR_APU_STATE_DSP_RESET : 0u) |
        (apu->romReadable ? SR_APU_STATE_BOOT_ROM_VISIBLE : 0u) |
        (dsp_extendedVoicesEnabled() ? SR_APU_STATE_EXTENDED_VOICES : 0u);
    out_state->lifetime_generation = snes->abiLifetimeGeneration;
    out_state->load_generation = snes->abiLoadGeneration;
    out_state->apu_cycles = apu->cycleClock;
    out_state->dsp_frames_completed = apu->sampleClock;
    out_state->pcm_write_cursor = dsp->sampleWrite;
    out_state->pcm_read_cursor = dsp->sampleRead;
    out_state->apu_ram_bytes_written = sizeof(apu->ram);
    out_state->dsp_register_bytes_written = sizeof(dsp->ram);
    out_state->pcm_ring_fill_frames = dsp->sampleWrite - dsp->sampleRead;
    out_state->scheduled_port_write_count =
        apu->portQTail - apu->portQHead;
    out_state->spc_pc = spc->pc;
    out_state->spc_instruction_pc = spc->instructionPc;
    out_state->spc_a = spc->a;
    out_state->spc_x = spc->x;
    out_state->spc_y = spc->y;
    out_state->spc_sp = spc->sp;
    out_state->spc_psw = spc_status(spc);
    out_state->spc_instruction_cycle =
        spc->cyclesUsed > apu->cpuCyclesLeft
            ? (uint8_t)(spc->cyclesUsed - apu->cpuCyclesLeft) : 0u;
    out_state->dsp_slot = apu->dspSlot;
    out_state->current_dsp_address = apu->dspAdr;
    memcpy(out_state->cpu_to_apu_ports, apu->inPorts,
           sizeof(out_state->cpu_to_apu_ports));
    memcpy(out_state->apu_aux_ports,
           apu->inPorts + SR_APU_CPU_PORT_COUNT,
           sizeof(out_state->apu_aux_ports));
    memcpy(out_state->apu_to_cpu_ports, apu->outPorts,
           sizeof(out_state->apu_to_cpu_ports));
    for (index = 0u; index < 3u; ++index) {
        out_state->timer_targets[index] = apu->timer[index].target;
        out_state->timer_outputs[index] = apu->timer[index].counter;
        if (apu->timer[index].enabled)
            out_state->timer_enabled_mask |= (uint8_t)(1u << index);
    }
    out_state->hardware_voice_count = SR_DSP_HARDWARE_VOICE_COUNT;
    out_state->extended_voice_count = dsp_extendedVoicesEnabled()
        ? SR_DSP_EXTENDED_VOICE_COUNT : 0u;
    RtlApuUnlock();
    return SR_RESULT_OK;
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
        (control->flags & ~SR_AUDIO_MIX_FLAGS_SUPPORTED) != 0u ||
        control->music_gain_percent > 100u ||
        control->sfx_gain_percent > 100u || control->reserved != 0u ||
        ((control->flags &
          SR_AUDIO_MIX_PARTITION_UNCLASSIFIED_BY_SOURCE) != 0u
             ? control->unclassified_music_source_min > UINT8_MAX
             : control->unclassified_music_source_min != 0u))
        return SR_RESULT_INVALID_ARGUMENT;
    if (snes->apu == NULL || snes->apu->dsp == NULL)
        return SR_RESULT_UNAVAILABLE;
    RtlApuLock();
    dsp_setBusGains((int)control->music_gain_percent,
                    (int)control->sfx_gain_percent);
    dsp_setMusicBusMuted(
        (control->flags & SR_AUDIO_MIX_MUTE_MUSIC) != 0u);
    dsp_setUnclassifiedMusicSourceMinimum(
        (control->flags &
         SR_AUDIO_MIX_PARTITION_UNCLASSIFIED_BY_SOURCE) != 0u
            ? (int)control->unclassified_music_source_min : -1);
    RtlApuUnlock();
    return SR_RESULT_OK;
}
