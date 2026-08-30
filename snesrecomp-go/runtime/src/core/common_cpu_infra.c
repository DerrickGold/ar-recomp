#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"

#include "snesrecomp/game/runtime.h"
#include "runtime_trace.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/trace.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/cpu.h"
#include "snes/dma.h"
#include "snes/dsp.h"
#include "snes/msu1.h"
#include "snes/ppu.h"
#include "snes/saveload.h"
#include "snes/snes.h"
#include "snes/spc.h"
#include "runner_internal.h"
#include "runner_game_module_internal.h"
#include "runner_state_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) && SNESRECOMP_WATCHDOG
#include <windows.h>
#endif

enum { kRecompStackCapacity = 64 };

Snes *g_snes;
Cpu *g_snes_cpu;
bool g_fail;

const char *g_last_recomp_func = "(none)";
const char *g_recomp_stack[kRecompStackCapacity];
int g_recomp_stack_top;
unsigned long g_recomp_push_count;
uint16 g_cpu_entry_s[kRecompStackCapacity];
uint8 g_cpu_entry_hrv[kRecompStackCapacity];

uint32 g_sr_block_ring[kRuntimeBlockTraceRingCapacity];
uint32 g_sr_block_aux[kRuntimeBlockTraceRingCapacity];
uint16 g_sr_block_stack[kRuntimeBlockTraceRingCapacity];
unsigned g_sr_block_index;

uint32 g_tailcall_pc24;
uint16 g_tailcall_miss_s;
uint32 g_tailcall_src24;
static uint16 g_tailcall_entry_s;
static uint8 g_tailcall_hrv;
static bool g_tailcall_context_valid;

uint64 g_watchdog_loop_headers;
int g_watchdog_tripped;

_Static_assert(kDspHardwareVoiceCount == 8,
               "audio adapter hardware voice count mismatch");
_Static_assert(kDspHardwareVoiceCount ==
                   RTL_AUDIO_ADAPTER_HARDWARE_VOICE_COUNT,
               "audio adapter hardware voice count mismatch");
_Static_assert(kDspExtendedVoiceCount ==
                   RTL_AUDIO_ADAPTER_EXTENDED_VOICE_COUNT,
               "audio adapter extended voice count mismatch");
_Static_assert(kDspMaximumVoiceCount == RTL_AUDIO_ADAPTER_VOICE_MAX,
               "audio adapter maximum voice count mismatch");
_Static_assert(kDspVoiceBus_Unclassified == RTL_AUDIO_VOICE_BUS_UNCLASSIFIED &&
                   kDspVoiceBus_Music == RTL_AUDIO_VOICE_BUS_MUSIC &&
                   kDspVoiceBus_Sfx == RTL_AUDIO_VOICE_BUS_SFX,
               "audio adapter bus values must match the DSP");

static uint32 audio_voice_count(void) {
    return dsp_extendedVoicesEnabled()
        ? kDspMaximumVoiceCount : kDspHardwareVoiceCount;
}

static bool s_audio_extension_enabled;
static void install_game_hooks(void);

static bool audio_extension_dsp_operation(
        void *service_context, uint32_t operation, uint32_t voice,
        uint8_t address, uint8_t value, uint8_t update_mask) {
    Apu *apu = (Apu *)service_context;
    const uint32_t voice_count = audio_voice_count();
    if (apu == NULL || apu->dsp == NULL) return false;
    switch ((RtlAudioDspOperation)operation) {
    case RTL_AUDIO_DSP_SET_VOICE_BUS:
        if (voice >= voice_count || value > RTL_AUDIO_VOICE_BUS_SFX)
            return false;
        dsp_setVoiceBus(apu->dsp, (int)voice, (DspVoiceBus)value);
        return true;
    case RTL_AUDIO_DSP_WRITE_VIRTUAL_REGISTER:
        if (voice < kDspHardwareVoiceCount || voice >= voice_count ||
            address >= 0x80u)
            return false;
        dsp_writeVirtualVoiceRegister(
            apu->dsp, (int)voice, address, value);
        return true;
    case RTL_AUDIO_DSP_WRITE_VIRTUAL_CONTROL:
        if (voice < kDspHardwareVoiceCount || voice >= voice_count ||
            address >= 0x80u || value > 1u)
            return false;
        dsp_writeVirtualVoiceControl(
            apu->dsp, (int)voice, address, value != 0u);
        return true;
    case RTL_AUDIO_DSP_WRITE_HARDWARE_MASK:
        if (address >= 0x80u) return false;
        dsp_writeHardwareVoiceMask(apu->dsp, address, value, update_mask);
        return true;
    default:
        return false;
    }
}

static bool populate_audio_extension_context(
        Apu *apu, RtlAudioExtensionContext *context) {
    uint32_t voice_count;
    if (apu == NULL || context == NULL || apu->spc == NULL ||
        apu->dsp == NULL)
        return false;
    voice_count = audio_voice_count();
    memset(context, 0, sizeof(*context));
    context->struct_size = RTL_AUDIO_EXTENSION_CONTEXT_V2_SIZE;
    context->apu_ram = apu->ram;
    context->apu_ram_byte_size = sizeof(apu->ram);
    context->hardware_voice_count = kDspHardwareVoiceCount;
    context->extended_voice_count =
        voice_count - kDspHardwareVoiceCount;
    context->spc_pc = apu->spc->pc;
    context->spc_x = apu->spc->x;
    context->spc_z = (uint8_t)apu->spc->z;
    context->service_context = apu;
    context->dsp_operation = audio_extension_dsp_operation;
    return true;
}

static bool route_game_audio_extension_dsp_write(
        Apu *apu, uint8_t address, uint8_t *value) {
    RtlAudioExtensionContext context;
    if (!s_audio_extension_enabled || g_rtl_game_audio == NULL ||
        g_rtl_game_audio->extension_dsp_write == NULL ||
        value == NULL || !populate_audio_extension_context(apu, &context))
        return true;
    return g_rtl_game_audio->extension_dsp_write(
        &context, address, value);
}

static void route_game_audio_extension_spc_opcode(
        Spc *spc, uint16_t opcode_pc) {
    RtlAudioExtensionContext context;
    if (!s_audio_extension_enabled || g_rtl_game_audio == NULL ||
        g_rtl_game_audio->extension_spc_opcode == NULL || spc == NULL ||
        !populate_audio_extension_context(spc->apu, &context))
        return;
    g_rtl_game_audio->extension_spc_opcode(&context, opcode_pc);
    if (context.struct_size < RTL_AUDIO_EXTENSION_CONTEXT_V2_SIZE ||
        context.flags != 0u || context.spc_z > 1u)
        return;
    spc->pc = context.spc_pc;
    spc->x = context.spc_x;
    spc->z = context.spc_z != 0u;
}

static int route_game_audio_extension_spc_cycle(
        Spc *spc, uint16_t opcode_pc, int cycles) {
    (void)spc;
    if (!s_audio_extension_enabled || g_rtl_game_audio == NULL ||
        g_rtl_game_audio->extension_spc_cycle == NULL)
        return cycles;
    return g_rtl_game_audio->extension_spc_cycle(opcode_pc, cycles);
}

static bool audio_save_transfer(
        void *service_context, uint32_t kind, void *values, uint64_t count) {
    SaveLoadInfo *info = (SaveLoadInfo *)service_context;
    uint64_t index;
    if (info == NULL || info->func == NULL || values == NULL ||
        count > (uint64_t)SIZE_MAX)
        return false;
    switch ((RtlAudioSaveValueKind)kind) {
    case RTL_AUDIO_SAVE_BYTES:
        saveload_bytes(info, values, (size_t)count);
        break;
    case RTL_AUDIO_SAVE_U8:
        saveload_bytes(info, values, (size_t)count);
        break;
    case RTL_AUDIO_SAVE_U16:
        for (index = 0u; index < count; ++index)
            saveload_u16(info, &((uint16_t *)values)[index]);
        break;
    case RTL_AUDIO_SAVE_U32:
        for (index = 0u; index < count; ++index)
            saveload_u32(info, &((uint32_t *)values)[index]);
        break;
    case RTL_AUDIO_SAVE_U64:
        for (index = 0u; index < count; ++index)
            saveload_u64(info, &((uint64_t *)values)[index]);
        break;
    default:
        info->failed = true;
        return false;
    }
    return !info->failed;
}

static void route_game_audio_extension_save(
        Apu *apu, SaveLoadInfo *info) {
    RtlAudioSaveContext context;
    (void)apu;
    if (!s_audio_extension_enabled || g_rtl_game_audio == NULL ||
        g_rtl_game_audio->extension_save == NULL || info == NULL ||
        info->func == NULL)
        return;
    memset(&context, 0, sizeof(context));
    context.struct_size = RTL_AUDIO_SAVE_CONTEXT_V2_SIZE;
    context.saving = (uint8_t)info->saving;
    context.portable = (uint8_t)info->portable;
    context.service_context = info;
    context.transfer = audio_save_transfer;
    g_rtl_game_audio->extension_save(&context);
}

void RtlAudioExtensionConfigure(bool enabled) {
    s_audio_extension_enabled = enabled;
    dsp_setExtendedVoicesEnabled(enabled);
    install_game_hooks();
}

void RtlAudioExtensionNotifyUploadLocked(uint32 source24) {
    RtlAudioExtensionContext context;
    if (!s_audio_extension_enabled || g_rtl_game_audio == NULL ||
        g_rtl_game_audio->extension_upload == NULL || g_snes == NULL ||
        g_snes->apu == NULL)
        return;
    if (populate_audio_extension_context(g_snes->apu, &context))
        g_rtl_game_audio->extension_upload(&context, source24);
}

static bool audio_bus_update_valid(const RtlAudioVoiceBusUpdate *update,
                                   uint32 voice_count) {
    return update != NULL && update->voice < voice_count &&
        update->bus <= RTL_AUDIO_VOICE_BUS_SFX &&
        update->reserved8[0] == 0u && update->reserved8[1] == 0u;
}

static void route_game_audio_dsp_write(Apu *apu, uint8 address, uint8 value) {
    RtlAudioDspWriteContext context;
    RtlAudioDspWriteRouting routing = {
        .struct_size = RTL_AUDIO_DSP_WRITE_ROUTING_V2_SIZE,
    };
    uint32 voice_count;
    uint32 index;
    if (apu == NULL || apu->spc == NULL || apu->dsp == NULL ||
        g_rtl_game_audio == NULL ||
        g_rtl_game_audio->dsp_write_routing == NULL)
        return;
    voice_count = audio_voice_count();
    context.struct_size = RTL_AUDIO_DSP_WRITE_CONTEXT_V2_SIZE;
    context.flags = 0u;
    context.apu_ram = apu->ram;
    context.voice_bus = apu->dsp->voiceBus;
    context.apu_ram_byte_size = sizeof(apu->ram);
    context.voice_bus_count = voice_count;
    context.spc_x = apu->spc->x;
    context.dsp_address = address;
    context.dsp_value = value;
    context.extended_voices_enabled =
        (uint8)(voice_count > kDspHardwareVoiceCount);
    g_rtl_game_audio->dsp_write_routing(&context, &routing);
    if (routing.struct_size < RTL_AUDIO_DSP_WRITE_ROUTING_V2_SIZE ||
        routing.flags != 0u || routing.reserved != 0u ||
        routing.update_count > RTL_AUDIO_DSP_WRITE_UPDATE_MAX)
        return;
    for (index = 0u; index < routing.update_count; ++index) {
        if (!audio_bus_update_valid(&routing.update[index], voice_count))
            return;
    }
    for (index = 0u; index < routing.update_count; ++index) {
        dsp_setVoiceBus(apu->dsp, routing.update[index].voice,
                        (DspVoiceBus)routing.update[index].bus);
    }
}

void rtl_game_audio_state_loaded(Apu *apu) {
    RtlAudioStateLoadedContext context;
    RtlAudioStateLoadedRouting routing = {
        .struct_size = RTL_AUDIO_STATE_LOADED_ROUTING_V2_SIZE,
    };
    uint32 voice_count;
    uint32 index;
    if (apu == NULL || apu->dsp == NULL || g_rtl_game_audio == NULL ||
        g_rtl_game_audio->state_loaded_routing == NULL)
        return;
    voice_count = audio_voice_count();
    context.struct_size = RTL_AUDIO_STATE_LOADED_CONTEXT_V2_SIZE;
    context.flags = 0u;
    context.apu_ram = apu->ram;
    context.apu_ram_byte_size = sizeof(apu->ram);
    context.voice_bus_count = voice_count;
    context.extended_voices_enabled =
        (uint8)(voice_count > kDspHardwareVoiceCount);
    memset(context.reserved8, 0, sizeof(context.reserved8));
    g_rtl_game_audio->state_loaded_routing(&context, &routing);
    if (routing.struct_size < RTL_AUDIO_STATE_LOADED_ROUTING_V2_SIZE ||
        routing.flags != 0u || routing.reserved != 0u ||
        routing.voice_bus_count > voice_count)
        return;
    for (index = 0u; index < routing.voice_bus_count; ++index) {
        if (routing.voice_bus[index] > RTL_AUDIO_VOICE_BUS_SFX) return;
    }
    for (index = 0u; index < routing.voice_bus_count; ++index) {
        dsp_setVoiceBus(apu->dsp, (int)index,
                        (DspVoiceBus)routing.voice_bus[index]);
    }
}

static bool optional_table_matches(uint64_t capabilities, uint64_t bit,
                                   const void *table) {
    return ((capabilities & bit) != 0u) == (table != NULL);
}

static bool game_identity_valid(const RtlGameIdentity *identity) {
    return identity != NULL &&
        identity->struct_size >= RTL_GAME_IDENTITY_V1_SIZE &&
        identity->flags == 0u && identity->game_id != NULL &&
        identity->game_id[0] != '\0';
}

static bool game_execution_valid(const RtlGameExecutionApi *execution) {
    return execution != NULL &&
        execution->struct_size >= RTL_GAME_EXECUTION_API_V2_SIZE &&
        execution->flags == 0u && execution->run_frame != NULL;
}

static bool game_lifecycle_valid(const RtlGameLifecycleApi *lifecycle) {
    return lifecycle != NULL &&
        lifecycle->struct_size >= RTL_GAME_LIFECYCLE_API_V1_SIZE &&
        lifecycle->flags == 0u;
}

static bool game_state_providers_valid(
        const RtlGameStateProviderApi *providers) {
    if (providers == NULL ||
        providers->struct_size < RTL_GAME_STATE_PROVIDER_API_V1_SIZE ||
        providers->flags != 0u ||
        (providers->query_cpu_state == NULL &&
         providers->query_execution_state == NULL))
        return false;
    return providers->query_cpu_state == NULL ||
        providers->cpu_component_handle != NULL;
}

static bool game_audio_valid(const RtlGameAudioApi *audio) {
    const bool spc_upload = audio != NULL &&
        (audio->capabilities & RTL_GAME_AUDIO_CAP_SPC_UPLOAD) != 0u;
    const bool voice_routing = audio != NULL &&
        (audio->capabilities & RTL_GAME_AUDIO_CAP_VOICE_ROUTING) != 0u;
    const bool extension = audio != NULL &&
        (audio->capabilities & RTL_GAME_AUDIO_CAP_EXTENSION) != 0u;
    const bool presentation = audio != NULL &&
        (audio->capabilities & RTL_GAME_AUDIO_CAP_PRESENTATION) != 0u;
    if (audio == NULL || audio->struct_size < RTL_GAME_AUDIO_API_V2_SIZE ||
        audio->flags != 0u || audio->capabilities == 0u ||
        (audio->capabilities & ~RTL_GAME_AUDIO_CAP_SUPPORTED) != 0u)
        return false;
    if (spc_upload != (audio->spc_upload_source != NULL) ||
        (!spc_upload && (audio->spc_upload_customize != NULL ||
                         audio->spc_upload_commit != NULL ||
                         audio->spc_upload_stack_pop != NULL)))
        return false;
    if (voice_routing != (audio->dsp_write_routing != NULL) ||
        voice_routing != (audio->state_loaded_routing != NULL))
        return false;
    if (extension != (audio->extension_dsp_write != NULL ||
                      audio->extension_spc_opcode != NULL ||
                      audio->extension_spc_cycle != NULL ||
                      audio->extension_save != NULL ||
                      audio->extension_upload != NULL))
        return false;
    if (presentation != (audio->apu_port_pace != NULL ||
                         audio->apu_port_write != NULL ||
                         audio->spc_upload_completed != NULL ||
                         audio->mix_output != NULL))
        return false;
    return true;
}

static SrResult validate_game_module(const RtlGameModule *module) {
    uint64_t capabilities;
    if (module == NULL || module->struct_size < RTL_GAME_MODULE_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    if (module->abi_version != RTL_GAME_MODULE_ABI_VERSION)
        return SR_RESULT_UNSUPPORTED;
    capabilities = module->capabilities;
    if ((capabilities & ~RTL_GAME_MODULE_CAP_SUPPORTED) != 0u)
        return SR_RESULT_UNSUPPORTED;
    if ((capabilities & RTL_GAME_MODULE_CAP_REQUIRED) !=
            RTL_GAME_MODULE_CAP_REQUIRED ||
        !optional_table_matches(
            capabilities, RTL_GAME_MODULE_CAP_IDENTITY, module->identity) ||
        !optional_table_matches(
            capabilities, RTL_GAME_MODULE_CAP_EXECUTION,
            module->execution) ||
        !optional_table_matches(
            capabilities, RTL_GAME_MODULE_CAP_LIFECYCLE,
            module->lifecycle) ||
        !optional_table_matches(
            capabilities, RTL_GAME_MODULE_CAP_STATE_PROVIDERS,
            module->state_providers) ||
        !optional_table_matches(
            capabilities, RTL_GAME_MODULE_CAP_AUDIO, module->audio))
        return SR_RESULT_INVALID_ARGUMENT;
    return game_identity_valid(module->identity) &&
        game_execution_valid(module->execution) &&
        (module->lifecycle == NULL ||
         game_lifecycle_valid(module->lifecycle)) &&
        (module->state_providers == NULL ||
         game_state_providers_valid(module->state_providers)) &&
        (module->audio == NULL || game_audio_valid(module->audio))
        ? SR_RESULT_OK : SR_RESULT_INVALID_ARGUMENT;
}

static void install_game_hooks(void) {
    g_snes_rdnmi_read_hook = g_rtl_game_execution != NULL
        ? g_rtl_game_execution->read_rdnmi : NULL;
    g_apu_spc_dsp_write_hook = g_rtl_game_audio != NULL &&
            g_rtl_game_audio->dsp_write_routing != NULL
        ? route_game_audio_dsp_write : NULL;
    g_apu_spc_dsp_write_filter_hook = s_audio_extension_enabled &&
            g_rtl_game_audio != NULL &&
            g_rtl_game_audio->extension_dsp_write != NULL
        ? route_game_audio_extension_dsp_write : NULL;
    g_spc_opcode_patch_hook = s_audio_extension_enabled &&
            g_rtl_game_audio != NULL &&
            g_rtl_game_audio->extension_spc_opcode != NULL
        ? route_game_audio_extension_spc_opcode : NULL;
    g_spc_opcode_cycle_hook = s_audio_extension_enabled &&
            g_rtl_game_audio != NULL &&
            g_rtl_game_audio->extension_spc_cycle != NULL
        ? route_game_audio_extension_spc_cycle : NULL;
    g_apu_extra_saveload_hook = s_audio_extension_enabled &&
            g_rtl_game_audio != NULL &&
            g_rtl_game_audio->extension_save != NULL
        ? route_game_audio_extension_save : NULL;
}

SrResult RtlRegisterGame(const RtlGameModule *module) {
    SrResult result;
    if (g_snes != NULL) return SR_RESULT_BUSY;
    result = validate_game_module(module);
    if (result != SR_RESULT_OK) return result;
    g_rtl_game_identity = module->identity;
    g_rtl_game_lifecycle = module->lifecycle;
    g_rtl_game_execution = module->execution;
    g_rtl_game_state_providers = module->state_providers;
    g_rtl_game_audio = module->audio;
    install_game_hooks();
    msu1_init();
    return SR_RESULT_OK;
}

const char *RtlGameIdentifier(void) {
    return g_rtl_game_identity != NULL ? g_rtl_game_identity->game_id : NULL;
}

_Static_assert(
    RTL_GAME_FRAME_DISPATCH_NMI_IF_ENABLED ==
        SR_GAME_TIMING_DISPATCH_NMI_IF_ENABLED,
    "linked and public game-timing flags must match");
_Static_assert(
    RTL_GAME_FRAME_NMI_ENTERED ==
        SR_GAME_TIMING_TRANSITION_NMI_ENTERED,
    "linked and public game-timing transitions must match");

bool RtlGameDrawPpuFrame(void) {
    if (g_snes != NULL) g_snes->diagnosticDrawRequested = true;
    if (g_rtl_game_execution == NULL ||
        g_rtl_game_execution->draw_ppu_frame == NULL) {
        if (g_snes != NULL && !g_snes->diagnosticMissingDrawReported) {
            g_snes->diagnosticMissingDrawReported = true;
            fprintf(stderr,
                    "[runner] the host requested a PPU frame, but the game "
                    "registered no draw_ppu_frame callback.\n"
                    "[runner] Set RtlGameExecutionApi.draw_ppu_frame, or do "
                    "not request video for an intentional headless run.\n");
        }
        return false;
    }
    g_rtl_game_execution->draw_ppu_frame();
    return true;
}

int RtlGameFrameBegin(void) {
    if (g_snes == NULL) return -1;
    (void)sr_runner_transition_game_timing(
        g_snes, SR_GAME_TIMING_BEGIN_FRAME_SLICE, 0u);
    return 0;
}

int RtlGameFrameComplete(uint32_t flags) {
    if (g_snes == NULL ||
        (flags & ~RTL_GAME_FRAME_DISPATCH_NMI_IF_ENABLED) != 0u)
        return -1;
    return (int)sr_runner_transition_game_timing(
        g_snes, SR_GAME_TIMING_COMPLETE_FRAME_SLICE, flags);
}

uint32_t RtlGamePpuDisplayState(void) {
    const Ppu *ppu = g_snes != NULL ? g_snes->ppu : NULL;
    if (ppu == NULL) return 0u;
    return (uint32_t)ppu->inidisp |
        ((uint32_t)ppu->bgmode << 8) |
        ((uint32_t)ppu->screenEnabled[0] << 16) |
        ((uint32_t)ppu->screenEnabled[1] << 24);
}

SrResult RtlGameApplyPpuFramePolicy(const SrPpuFramePolicy *policy) {
    if (g_snes == NULL) return SR_RESULT_UNAVAILABLE;
    return sr_runner_apply_ppu_frame_policy(g_snes, policy);
}

SrRunnerHandle *RtlGameRunner(void) {
    return g_snes != NULL ? sr_runner_handle(g_snes) : NULL;
}

bool RtlGameEventEnabled(SrEventMask event_mask) {
    return sr_runner_event_enabled(event_mask);
}

void RtlGameEmitInterrupt(SrInterruptKind kind, uint32_t flags,
                          uint32_t pc24, uint16_t vector,
                          int32_t scanline, const char *label) {
    if (g_snes == NULL) return;
    sr_runner_emit_interrupt(g_snes, kind, flags, pc24, vector, scanline,
                             label);
}

uint8 *SnesRomPtr(uint32 address) { return RomPtr(address); }

void SnesEnterNativeMode(void) {
    if (g_snes_cpu == NULL) return;
    g_snes_cpu->e = false;
    g_snes_cpu->sp = 0x01ffu;
    g_snes_cpu->dp = 0u;
    g_snes_cpu->mf = false;
    g_snes_cpu->xf = false;
    g_snes_cpu->d = false;
    g_snes_cpu->i = true;
}

uint8 *IndirPtrDB(uint8 direct_page_address, uint16 offset) {
    uint16 pointer = (uint16)g_ram[direct_page_address] |
                     ((uint16)g_ram[(uint8)(direct_page_address + 1u)] << 8);
    uint32 address = (((uint32)g_snes_cpu->db << 16) | pointer) + offset;
    uint8 bank = (uint8)(address >> 16);
    if (bank == 0x7eu || bank == 0x7fu) {
        return &g_ram[address & kSnesWramMask];
    }
    if ((uint16)address < 0x2000u) return &g_ram[(uint16)address];
    return RomPtr(address & 0xffffffu);
}

void cpu_trace_block(CpuState *cpu, uint32 pc24) {
    unsigned slot = g_sr_block_index++ & kRuntimeBlockTraceRingMask;
    g_sr_block_ring[slot] = pc24 & 0xffffffu;
    g_sr_block_aux[slot] = ((uint32)(cpu->x_flag & 1u) << 17) |
                         ((uint32)(cpu->m_flag & 1u) << 16) | cpu->X;
    g_sr_block_stack[slot] = cpu->S;
    if (sr_runner_event_enabled(SR_EVENT_MASK_EXECUTION_BLOCK)) {
        SrRunnerEvent event = {0};
        event.type = SR_EVENT_EXECUTION_BLOCK;
        event.frame_counter = snes_frame_counter >= 0
            ? (uint64)snes_frame_counter : 0u;
        event.cpu_flags =
            (cpu->m_flag ? SR_CPU_STATE_M_FLAG : 0u) |
            (cpu->x_flag ? SR_CPU_STATE_X_FLAG : 0u) |
            (cpu->emulation ? SR_CPU_STATE_EMULATION : 0u) |
            (cpu->host_return_valid ? SR_CPU_STATE_HOST_RETURN_VALID : 0u);
        event.pc24 = pc24 & 0x00ffffffu;
        event.register_x = cpu->X;
        event.stack_pointer = cpu->S;
        event.label = g_last_recomp_func;
        sr_runner_emit_event(g_snes, SR_EVENT_MASK_EXECUTION_BLOCK, &event);
    }
#if SNESRECOMP_TRACE
    cpu_trace_event(cpu, pc24, CPU_TR_BLOCK, 0u, 0u);
#endif
}

int sr_block_history_available(void) {
    unsigned available = g_sr_block_index;
    if (available > kRuntimeBlockTraceRingCapacity) {
        available = kRuntimeBlockTraceRingCapacity;
    }
    return (int)available;
}

int sr_block_history(uint32 *output, int maximum) {
    unsigned available;
    int count;
    int index;
    if (output == NULL || maximum <= 0) return 0;
    if (maximum > kRuntimeBlockTraceRingCapacity) {
        maximum = kRuntimeBlockTraceRingCapacity;
    }
    available = (unsigned)sr_block_history_available();
    count = available < (unsigned)maximum ? (int)available : maximum;
    for (index = 0; index < count; ++index) {
        output[index] = g_sr_block_ring[
            (g_sr_block_index - (unsigned)count + (unsigned)index) &
            kRuntimeBlockTraceRingMask];
    }
    return count;
}

void cpu_tailcall_inherit_return_context(uint16 entry_stack, uint8 hrv) {
    g_tailcall_entry_s = entry_stack;
    g_tailcall_hrv = hrv;
    g_tailcall_context_valid = true;
}

int cpu_take_tailcall_return_context(uint16 *entry_stack, uint8 *hrv) {
    if (!g_tailcall_context_valid) return 0;
    if (entry_stack != NULL) *entry_stack = g_tailcall_entry_s;
    if (hrv != NULL) *hrv = g_tailcall_hrv;
    g_tailcall_context_valid = false;
    return 1;
}

void cpu_tailcall_request(uint32 pc24, uint16 miss_stack,
                          uint32 source_pc24) {
    g_tailcall_pc24 = pc24 & 0xffffffu;
    g_tailcall_miss_s = miss_stack;
    g_tailcall_src24 = source_pc24 & 0xffffffu;
}

int cpu_resolve_ancestor_skip(uint16 return_stack) {
    int frame;
    if (g_recomp_stack_top < 2 ||
        g_recomp_stack_top > kRecompStackCapacity) return -1;
    /* The emulated stack is authoritative. A hardware RTS may deliberately
     * discard several nested frames, including frames that happened to have
     * paired host callers. Return a SKIP distance to the nearest matching
     * emulated entry instead of resuming an arbitrary lexical C caller. */
    for (frame = g_recomp_stack_top - 2; frame >= 0; --frame) {
        if (g_cpu_entry_s[frame] == return_stack) {
            return (g_recomp_stack_top - 1) - frame;
        }
    }
    return -1;
}

void RecompStackPush(const char *name) {
    const char *safe_name = name != NULL ? name : "(unnamed)";
    ++g_recomp_push_count;
    if (g_recomp_stack_top < kRecompStackCapacity) {
        g_recomp_stack[g_recomp_stack_top++] = safe_name;
    }
    g_last_recomp_func = safe_name;
    boundary_audit_record_entry(safe_name);
}

void RecompStackPop(void) {
    if (g_recomp_stack_top > 0) {
        boundary_audit_record_exit(g_recomp_stack[g_recomp_stack_top - 1]);
        --g_recomp_stack_top;
    }
    g_last_recomp_func = g_recomp_stack_top > 0
        ? g_recomp_stack[g_recomp_stack_top - 1] : "(none)";
}

void RecompStackDump(void) {
    int frame;
    fprintf(stderr, "Recomp call stack (%d deep):\n", g_recomp_stack_top);
    for (frame = g_recomp_stack_top - 1; frame >= 0; --frame) {
        fprintf(stderr, "  [%d] %s\n", g_recomp_stack_top - 1 - frame,
                g_recomp_stack[frame]);
    }
}

#if SNESRECOMP_WATCHDOG
static uint64 monotonic_nanoseconds(void) {
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    uint64 ticks;
    uint64 rate;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    ticks = (uint64)counter.QuadPart;
    rate = (uint64)frequency.QuadPart;
    if (rate == 0u) return 0u;
    return (ticks / rate) * 1000000000ull +
           ((ticks % rate) * 1000000000ull) / rate;
#else
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64)value.tv_sec * 1000000000ull + (uint64)value.tv_nsec;
#endif
}

static uint64 g_watchdog_started;
static unsigned g_watchdog_poll_count;
static bool g_watchdog_enabled;
void (*g_watchdog_yield_hook)(void);

void WatchdogFrameStart(void) {
    g_watchdog_started = monotonic_nanoseconds();
    g_watchdog_poll_count = 0u;
    g_watchdog_enabled = true;
    g_watchdog_tripped = 0;
    g_recomp_stack_top = 0;
    g_tailcall_context_valid = false;
}

void WatchdogFrameEnd(void) { g_watchdog_enabled = false; }

void WatchdogCheck(void) {
    double elapsed;
    ++g_watchdog_loop_headers;
    if (!g_watchdog_enabled || ++g_watchdog_poll_count < 10000u) return;
    g_watchdog_poll_count = 0u;
    if (snes_frame_counter == 0) return;
    elapsed = (double)(monotonic_nanoseconds() - g_watchdog_started) / 1e9;
    if (elapsed <= 5.0) return;
    fprintf(stderr, "watchdog: frame %d exceeded %.1f seconds\n",
            snes_frame_counter, elapsed);
    RecompStackDump();
    g_watchdog_enabled = false;
    g_watchdog_tripped = 1;
    if (g_watchdog_yield_hook != NULL) g_watchdog_yield_hook();
}
#else
void WatchdogFrameStart(void) {
    g_watchdog_tripped = 0;
    g_recomp_stack_top = 0;
    g_tailcall_context_valid = false;
}
void WatchdogFrameEnd(void) {}
void WatchdogCheck(void) { ++g_watchdog_loop_headers; }
#endif

static void clear_published_runner(void) {
    Snes *snes = g_snes;
    if (snes != NULL && g_rtl_game_lifecycle != NULL &&
        g_rtl_game_lifecycle->runner_changed != NULL) {
        g_rtl_game_lifecycle->runner_changed(NULL);
    }
    if (snes != NULL) {
        sr_runner_set_cpu_state_provider(snes, NULL, NULL, NULL);
        sr_runner_set_execution_state_provider(snes, NULL, NULL);
        sr_runner_bind_ppu_services(snes, false);
        sr_trace_bind_runner(snes, 0);
    }
    sr_runner_clear_event_subscriptions(snes);
    sr_runner_clear_audio_trace_subscriptions(snes);
    g_snes = NULL;
    g_snes_cpu = NULL;
    g_dma = NULL;
    g_ppu = NULL;
    g_rom = NULL;
    g_sram = NULL;
    g_sram_size = 0;
}

static void publish_runner(Snes *snes) {
    const RtlGameStateProviderApi *providers = g_rtl_game_state_providers;
    if (snes == NULL) return;
    sr_runner_bind_ppu_services(snes, true);
    sr_trace_bind_runner(snes, 1);
    if (providers != NULL) {
        sr_runner_set_cpu_state_provider(
            snes, providers->query_cpu_state, providers->user_data,
            providers->cpu_component_handle);
        sr_runner_set_execution_state_provider(
            snes, providers->query_execution_state, providers->user_data);
    }
    if (g_rtl_game_lifecycle != NULL &&
        g_rtl_game_lifecycle->runner_changed != NULL)
        g_rtl_game_lifecycle->runner_changed(sr_runner_handle(snes));
}

static bool initialize_game(bool has_rom) {
    RtlGameInitializeContext context;
    if (g_rtl_game_lifecycle == NULL ||
        g_rtl_game_lifecycle->initialize == NULL)
        return true;
    memset(&context, 0, sizeof(context));
    context.struct_size = RTL_GAME_INITIALIZE_CONTEXT_V1_SIZE;
    context.flags = has_rom ? RTL_GAME_INITIALIZE_HAS_ROM : 0u;
    context.runner = sr_runner_handle(g_snes);
    if (has_rom) {
        context.rom_data = g_snes->cart->rom;
        context.rom_byte_size = g_snes->cart->romSize;
    }
    return g_rtl_game_lifecycle->initialize(&context);
}

void SnesShutdown(void) {
    Snes *snes = g_snes;
    clear_published_runner();
    snes_free(snes);
}

Snes *SnesInit(const uint8 *data, int data_size) {
    bool loaded;
    if (data_size < 0 || (data_size > 0 && data == NULL) ||
        g_rtl_game_execution == NULL) return NULL;
    SnesShutdown();
    g_snes = snes_init(g_ram);
    if (g_snes == NULL) return NULL;
    publish_runner(g_snes);
    g_snes_cpu = g_snes->cpu;
    g_dma = g_snes->dma;
    g_ppu = g_snes->ppu;
    if (data_size > 0) {
        loaded = snes_loadRom(g_snes, data, data_size);
        if (!loaded) goto fail;
        g_rom = g_snes->cart->rom;
        if (!initialize_game(true)) goto fail;
        snes_reset(g_snes, true);
        SnesEnterNativeMode();
    } else {
        uint8 *ram = (uint8 *)calloc(2048u, 1u);
        if (ram == NULL) goto fail;
        g_snes->cart->ram = ram;
        g_snes->cart->ramSize = 2048u;
        if (!initialize_game(false)) goto fail;
        ppu_reset(g_ppu);
        dma_reset(g_dma);
    }
    g_sram = g_snes->cart->ram;
    g_sram_size = (int)g_snes->cart->ramSize;
    return g_snes;

fail:
    SnesShutdown();
    return NULL;
}
