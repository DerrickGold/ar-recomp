#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/trace.h"
#include "runner_internal.h"
#include "runner_game_module_internal.h"
#include "runner_state_internal.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/cpu.h"
#include "snes/dma.h"
#include "snes/dsp.h"
#include "snes/ppu.h"
#include "snes/saveload.h"
#include "snes/snes.h"
#include "snes/spc.h"

#include <stdio.h>
#include <string.h>

uint8 g_ram[kSnesWramSize];
uint8 *g_sram;
int g_sram_size;
const uint8 *g_rom;
Ppu *g_ppu;
Dma *g_dma;
int snes_frame_counter;
SnesRdnmiReadHook *g_snes_rdnmi_read_hook;
SrEventMask g_sr_runner_event_mask;
void (*g_apu_spc_dsp_write_hook)(Apu *, uint8_t, uint8_t);
bool (*g_apu_spc_dsp_write_filter_hook)(Apu *, uint8_t, uint8_t *);
void (*g_apu_extra_saveload_hook)(Apu *, SaveLoadInfo *);
void (*g_spc_opcode_patch_hook)(Spc *, uint16_t);
int (*g_spc_opcode_cycle_hook)(Spc *, uint16_t, int);

void sr_runner_emit_event(Snes *snes, SrEventMask event_mask,
                          SrRunnerEvent *event) {
    (void)snes;
    (void)event_mask;
    (void)event;
}

void sr_runner_emit_interrupt(Snes *snes, SrInterruptKind kind,
                              uint32_t flags, uint32_t pc24,
                              uint16_t vector, int32_t scanline,
                              const char *label) {
    (void)snes;
    (void)kind;
    (void)flags;
    (void)pc24;
    (void)vector;
    (void)scanline;
    (void)label;
}

void sr_runner_clear_event_subscriptions(Snes *snes) { (void)snes; }
void sr_runner_clear_audio_trace_subscriptions(Snes *snes) { (void)snes; }

static Snes test_snes;
static Cpu test_cpu;
static Apu test_apu;
static Dsp test_dsp;
static Spc test_spc;
static Cart test_cart;
static Dma test_dma;
static Ppu test_ppu;
static uint8 test_rom[0x10000];
static int initialize_count;
static int reset_count;
static int ppu_reset_count;
static int dma_reset_count;
static int msu_init_count;
static int free_count;
static int abi_bind_count;
static int abi_unbind_count;
static SrRunnerHandle *abi_bound_runner;
static int cpu_provider_bind_count;
static int cpu_provider_unbind_count;
static int execution_provider_bind_count;
static int execution_provider_unbind_count;
static int ppu_service_bind_count;
static int ppu_service_unbind_count;
static int ppu_frame_policy_count;
static int trace_bind_count;
static int trace_unbind_count;
static int draw_frame_count;
static int initialization_context_valid = 1;
static bool initialize_success = true;
static bool load_rom_success = true;
static int failures;
static int dsp_routing_count;
static int state_routing_count;
static int extension_dsp_count;
static int extension_opcode_count;
static int extension_save_count;
static int extension_upload_count;
static int rdnmi_context_valid;
static int apu_lock_count;
static int apu_unlock_count;
static bool extended_voices_enabled;
static int virtual_register_voice = -1;
static uint8 virtual_register_address;
static uint8 virtual_register_value;
static size_t extension_saved_bytes;

SrResult sr_runner_apply_ppu_frame_policy(
        Snes *snes, const SrPpuFramePolicy *policy) {
    if (snes != &test_snes || policy == NULL ||
        policy->struct_size < SR_PPU_FRAME_POLICY_V2_SIZE)
        return SR_RESULT_INVALID_ARGUMENT;
    ++ppu_frame_policy_count;
    return SR_RESULT_OK;
}

SrRunnerHandle *sr_runner_handle(Snes *snes) {
    return (SrRunnerHandle *)(void *)snes;
}

void sr_runner_set_cpu_state_provider(
        Snes *snes, RtlGameCpuStateQueryFunc *provider, void *user_data,
        const void *component_handle) {
    (void)snes;
    if (provider != NULL && user_data == &test_cpu &&
        component_handle == &test_cpu) {
        ++cpu_provider_bind_count;
    } else if (provider == NULL && user_data == NULL &&
               component_handle == NULL) {
        ++cpu_provider_unbind_count;
    }
}

void sr_runner_set_execution_state_provider(
        Snes *snes, RtlGameExecutionStateQueryFunc *provider,
        void *user_data) {
    (void)snes;
    if (provider != NULL && user_data == &test_cpu) {
        ++execution_provider_bind_count;
    } else if (provider == NULL && user_data == NULL) {
        ++execution_provider_unbind_count;
    }
}

void sr_runner_bind_ppu_services(Snes *snes, bool enabled) {
    (void)snes;
    if (enabled) ++ppu_service_bind_count;
    else ++ppu_service_unbind_count;
}

void sr_trace_bind_runner(Snes *snes, int enabled) {
    (void)snes;
    if (enabled) ++trace_bind_count;
    else ++trace_unbind_count;
}

static void check(int condition, const char *message) {
    if (condition) return;
    ++failures;
    fprintf(stderr, "runtime CPU infra failed: %s\n", message);
}

Snes *snes_init(uint8 *ram) {
    memset(&test_snes, 0, sizeof(test_snes));
    memset(&test_cpu, 0, sizeof(test_cpu));
    memset(&test_cart, 0, sizeof(test_cart));
    memset(&test_dma, 0, sizeof(test_dma));
    memset(&test_apu, 0, sizeof(test_apu));
    memset(&test_dsp, 0, sizeof(test_dsp));
    memset(&test_spc, 0, sizeof(test_spc));
    test_snes.ram = ram;
    test_snes.cpu = &test_cpu;
    test_snes.cart = &test_cart;
    test_snes.dma = &test_dma;
    test_snes.ppu = &test_ppu;
    test_snes.apu = &test_apu;
    test_apu.dsp = &test_dsp;
    test_apu.spc = &test_spc;
    test_spc.apu = &test_apu;
    test_cart.snes = &test_snes;
    test_dma.snes = &test_snes;
    return &test_snes;
}

bool snes_loadRom(Snes *snes, const uint8 *data, int length) {
    if (!load_rom_success) return false;
    snes->cart->rom = (uint8 *)data;
    snes->cart->romSize = (uint32)length;
    snes->cart->ram = g_ram + 0x18000;
    snes->cart->ramSize = 0x2000u;
    return length > 0;
}

void snes_free(Snes *snes) {
    if (snes != NULL) ++free_count;
}

void snes_reset(Snes *snes, bool hard) {
    (void)snes; (void)hard; ++reset_count;
}
void snes_beginVblank(Snes *snes) {
    if (snes == NULL) return;
    snes->hPos = 0u;
    snes->vPos = 225u;
    snes->inVblank = true;
}
uint32_t sr_runner_transition_game_timing(
        Snes *snes, SrGameTimingOperation operation, uint32_t flags) {
    bool enter_nmi;
    if (snes == NULL) return 0u;
    if (operation == SR_GAME_TIMING_BEGIN_FRAME_SLICE) {
        snes_beginVblank(snes);
        snes->forceNmi = true;
        snes->nmiAvail = true;
        return 0u;
    }
    enter_nmi =
        (flags & SR_GAME_TIMING_DISPATCH_NMI_IF_ENABLED) != 0u &&
        snes->nmiEnabled;
    snes->forceNmi = false;
    snes->inNmi = snes->inNmi || enter_nmi;
    return enter_nmi ? SR_GAME_TIMING_TRANSITION_NMI_ENTERED : 0u;
}
void ppu_reset(Ppu *ppu) { (void)ppu; ++ppu_reset_count; }
void dma_reset(Dma *dma) { (void)dma; ++dma_reset_count; }
void msu1_init(void) { ++msu_init_count; }
bool dsp_extendedVoicesEnabled(void) { return extended_voices_enabled; }
void dsp_setExtendedVoicesEnabled(bool enabled) {
    extended_voices_enabled = enabled;
}
void dsp_setVoiceBus(Dsp *dsp, int channel, DspVoiceBus bus) {
    if (dsp != NULL && channel >= 0 && channel < kDspMaximumVoiceCount)
        dsp->voiceBus[channel] = (uint8_t)bus;
}
void dsp_writeVirtualVoiceRegister(Dsp *dsp, int channel,
                                   uint8_t source_address, uint8_t value) {
    (void)dsp;
    virtual_register_voice = channel;
    virtual_register_address = source_address;
    virtual_register_value = value;
}
void dsp_writeVirtualVoiceControl(Dsp *dsp, int channel,
                                  uint8_t global_address, bool enabled) {
    (void)dsp; (void)channel; (void)global_address; (void)enabled;
}
void dsp_writeHardwareVoiceMask(Dsp *dsp, uint8_t address, uint8_t value,
                                uint8_t update_mask) {
    (void)dsp; (void)address; (void)value; (void)update_mask;
}
void RtlApuLock(void) { ++apu_lock_count; }
void RtlApuUnlock(void) { ++apu_unlock_count; }

uint8 *RomPtr(uint32 address) {
    return &test_rom[address & (sizeof(test_rom) - 1u)];
}

static bool initialize_game(const RtlGameInitializeContext *context) {
    ++initialize_count;
    initialization_context_valid &= context != NULL &&
        context->struct_size == RTL_GAME_INITIALIZE_CONTEXT_V1_SIZE &&
        context->runner == (SrRunnerHandle *)(void *)&test_snes;
    if (context != NULL &&
        (context->flags & RTL_GAME_INITIALIZE_HAS_ROM) != 0u) {
        initialization_context_valid &=
            context->flags == RTL_GAME_INITIALIZE_HAS_ROM &&
            context->rom_data == test_rom &&
            context->rom_byte_size == sizeof(test_rom);
    } else if (context != NULL) {
        initialization_context_valid &= context->flags == 0u &&
            context->rom_data == NULL && context->rom_byte_size == 0u;
    }
    return initialize_success;
}

static void run_frame(void) {}
static void draw_ppu_frame(void) { ++draw_frame_count; }

static SrResult query_cpu_state(
        void *user_data, SrCpuStateSnapshot *out_state) {
    if (user_data != &test_cpu || out_state == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    return SR_RESULT_OK;
}

static SrResult query_execution_state(
        void *user_data, SrExecutionSnapshot *out_state) {
    if (user_data != &test_cpu || out_state == NULL)
        return SR_RESULT_INVALID_ARGUMENT;
    return SR_RESULT_OK;
}

static int read_rdnmi(const RtlRdnmiReadContext *context) {
    rdnmi_context_valid = context != NULL &&
        context->struct_size == RTL_RDNMI_READ_CONTEXT_V2_SIZE &&
        context->flags == (RTL_RDNMI_FORCE_NMI | RTL_RDNMI_AVAILABLE);
    return 1;
}
static void runner_changed(SrRunnerHandle *runner) {
    if (runner != NULL) {
        ++abi_bind_count;
        abi_bound_runner = runner;
    } else {
        ++abi_unbind_count;
        abi_bound_runner = NULL;
    }
}

static void route_dsp_write(const RtlAudioDspWriteContext *context,
                            RtlAudioDspWriteRouting *routing) {
    check(context != NULL &&
              context->struct_size >= RTL_AUDIO_DSP_WRITE_CONTEXT_V2_SIZE &&
              context->apu_ram == test_apu.ram &&
              context->voice_bus == test_dsp.voiceBus &&
              context->spc_x == 0x12u && context->dsp_address == 0x74u &&
              context->dsp_value == 0x09u,
          "DSP routing context");
    ++dsp_routing_count;
    routing->update_count = 1u;
    routing->update[0].voice = 7u;
    routing->update[0].bus = RTL_AUDIO_VOICE_BUS_SFX;
}

static void route_state_loaded(const RtlAudioStateLoadedContext *context,
                               RtlAudioStateLoadedRouting *routing) {
    check(context != NULL &&
              context->struct_size >=
                  RTL_AUDIO_STATE_LOADED_CONTEXT_V2_SIZE &&
              context->apu_ram == test_apu.ram &&
              context->voice_bus_count == 40u,
          "state-loaded routing context");
    ++state_routing_count;
    routing->voice_bus_count = context->voice_bus_count;
    memset(routing->voice_bus, RTL_AUDIO_VOICE_BUS_MUSIC,
           routing->voice_bus_count);
    routing->voice_bus[6] = RTL_AUDIO_VOICE_BUS_SFX;
}

static bool route_extension_dsp_write(
        RtlAudioExtensionContext *context, uint8_t address, uint8_t *value) {
    check(context != NULL &&
              context->struct_size >= RTL_AUDIO_EXTENSION_CONTEXT_V2_SIZE &&
              context->apu_ram == test_apu.ram &&
              context->apu_ram_byte_size == sizeof(test_apu.ram) &&
              context->hardware_voice_count == 8u &&
              context->extended_voice_count == 32u &&
              context->spc_x == 0x12u && address == 0x74u &&
              value != NULL && *value == 0x09u &&
              context->dsp_operation != NULL,
          "audio-extension DSP context");
    ++extension_dsp_count;
    check(context->dsp_operation(
              context->service_context,
              RTL_AUDIO_DSP_WRITE_VIRTUAL_REGISTER,
              8u, address, *value, 0u),
          "audio-extension DSP operation");
    return false;
}

static void route_extension_spc_opcode(
        RtlAudioExtensionContext *context, uint16_t opcode_pc) {
    check(context != NULL && opcode_pc == 0x1234u &&
              context->spc_pc == 0x1234u && context->spc_x == 0x12u,
          "audio-extension SPC context");
    ++extension_opcode_count;
    context->spc_pc = 0x4567u;
    context->spc_x = 0x34u;
    context->spc_z = 1u;
}

static int route_extension_spc_cycle(uint16_t opcode_pc, int cycles) {
    check(opcode_pc == 0x1234u && cycles == 5,
          "audio-extension cycle context");
    return 0;
}

static void route_extension_save(RtlAudioSaveContext *context) {
    uint32_t marker = 0x12345678u;
    check(context != NULL &&
              context->struct_size >= RTL_AUDIO_SAVE_CONTEXT_V2_SIZE &&
              context->transfer != NULL,
          "audio-extension save context");
    ++extension_save_count;
    (void)context->transfer(
        context->service_context, RTL_AUDIO_SAVE_U32, &marker, 1u);
}

static void transfer_extension_state(
        SaveLoadInfo *info, void *data, size_t data_size) {
    (void)info;
    (void)data;
    extension_saved_bytes += data_size;
}

static void route_extension_upload(
        RtlAudioExtensionContext *context, uint32_t source24) {
    check(context != NULL && context->apu_ram == test_apu.ram &&
              source24 == 0x1a94b8u,
          "audio-extension upload context");
    ++extension_upload_count;
}

static void test_registration_and_initialization(void) {
    static const RtlGameIdentity identity = {
        .struct_size = RTL_GAME_IDENTITY_V1_SIZE,
        .game_id = "test",
        .display_name = "Runner Contract Test",
        .save_name_prefix = "test",
    };
    static const RtlGameLifecycleApi lifecycle = {
        .struct_size = RTL_GAME_LIFECYCLE_API_V1_SIZE,
        .initialize = initialize_game,
        .runner_changed = runner_changed,
    };
    static const RtlGameExecutionApi execution = {
        .struct_size = RTL_GAME_EXECUTION_API_V2_SIZE,
        .run_frame = run_frame,
        .draw_ppu_frame = draw_ppu_frame,
        .read_rdnmi = read_rdnmi,
    };
    static const RtlGameStateProviderApi state_providers = {
        .struct_size = RTL_GAME_STATE_PROVIDER_API_V1_SIZE,
        .user_data = &test_cpu,
        .cpu_component_handle = &test_cpu,
        .query_cpu_state = query_cpu_state,
        .query_execution_state = query_execution_state,
    };
    static const RtlGameAudioApi audio = {
        .struct_size = RTL_GAME_AUDIO_API_V2_SIZE,
        .capabilities = RTL_GAME_AUDIO_CAP_VOICE_ROUTING |
                        RTL_GAME_AUDIO_CAP_EXTENSION,
        .dsp_write_routing = route_dsp_write,
        .state_loaded_routing = route_state_loaded,
        .extension_dsp_write = route_extension_dsp_write,
        .extension_spc_opcode = route_extension_spc_opcode,
        .extension_spc_cycle = route_extension_spc_cycle,
        .extension_save = route_extension_save,
        .extension_upload = route_extension_upload,
    };
    static const RtlGameModule module = {
        .abi_version = RTL_GAME_MODULE_ABI_VERSION,
        .struct_size = RTL_GAME_MODULE_V2_SIZE,
        .capabilities = RTL_GAME_MODULE_CAP_IDENTITY |
                        RTL_GAME_MODULE_CAP_LIFECYCLE |
                        RTL_GAME_MODULE_CAP_EXECUTION |
                        RTL_GAME_MODULE_CAP_STATE_PROVIDERS |
                        RTL_GAME_MODULE_CAP_AUDIO,
        .identity = &identity,
        .lifecycle = &lifecycle,
        .execution = &execution,
        .state_providers = &state_providers,
        .audio = &audio,
    };
    RtlGameModule incompatible = module;
    RtlGameModule malformed = module;
    RtlGameExecutionApi missing_frame = execution;
    incompatible.abi_version = RTL_GAME_MODULE_ABI_VERSION + 1u;
    check(RtlRegisterGame(NULL) == SR_RESULT_INVALID_ARGUMENT,
          "null game module rejected");
    check(RtlRegisterGame(&incompatible) == SR_RESULT_UNSUPPORTED,
          "incompatible game module rejected");
    incompatible = module;
    incompatible.capabilities |= UINT64_C(0x8000000000000000);
    check(RtlRegisterGame(&incompatible) == SR_RESULT_UNSUPPORTED,
          "unknown game capability rejected");
    malformed.capabilities &= ~RTL_GAME_MODULE_CAP_AUDIO;
    check(RtlRegisterGame(&malformed) == SR_RESULT_INVALID_ARGUMENT,
          "module capability and table mismatch rejected");
    malformed = module;
    missing_frame.run_frame = NULL;
    malformed.execution = &missing_frame;
    check(RtlRegisterGame(&malformed) == SR_RESULT_INVALID_ARGUMENT,
          "missing required frame callback rejected");
    RtlAudioExtensionConfigure(true);
    check(RtlRegisterGame(&module) == SR_RESULT_OK,
          "game module registration");
    check(strcmp(RtlGameIdentifier(), "test") == 0,
          "registered game identity");
    check(RtlGameDrawPpuFrame() && draw_frame_count == 1,
          "registered draw callback");
    check(g_snes_rdnmi_read_hook == read_rdnmi,
          "direct RDNMI hook registration");
    {
        const RtlRdnmiReadContext context = {
            RTL_RDNMI_READ_CONTEXT_V2_SIZE,
            RTL_RDNMI_FORCE_NMI | RTL_RDNMI_AVAILABLE,
        };
        check(g_snes_rdnmi_read_hook(&context) == 1 && rdnmi_context_valid,
              "RDNMI fixed-width callback context");
    }
    check(g_apu_spc_dsp_write_hook != NULL,
          "DSP routing hook registration");
    check(extended_voices_enabled &&
              g_apu_spc_dsp_write_filter_hook != NULL &&
              g_spc_opcode_patch_hook != NULL &&
              g_spc_opcode_cycle_hook != NULL &&
              g_apu_extra_saveload_hook != NULL,
          "audio-extension hook registration");
    check(msu_init_count == 1, "MSU initialization");

    check(SnesInit(test_rom, (int)sizeof(test_rom)) == &test_snes,
          "ROM-backed initialization");
    check(initialize_count == 1 && initialization_context_valid,
          "game initialization callback context");
    check(RtlRegisterGame(&module) == SR_RESULT_BUSY,
          "registration rejected while a runner is active");
    check(abi_bind_count == 1 && abi_unbind_count == 0 &&
              abi_bound_runner == (SrRunnerHandle *)(void *)&test_snes,
          "runner ABI initial bind");
    check(cpu_provider_bind_count == 1 &&
              execution_provider_bind_count == 1 &&
              ppu_service_bind_count == 1 && trace_bind_count == 1,
          "runner-owned service publication");
    check(reset_count == 1, "hard reset after ROM load");
    check(g_snes_cpu == &test_cpu && g_ppu == &test_ppu && g_dma == &test_dma,
          "device publication");
    check(g_rom == test_rom && g_sram == g_ram + 0x18000 &&
          g_sram_size == 0x2000, "cartridge memory publication");
    check(!test_cpu.e && test_cpu.sp == 0x01ffu && !test_cpu.mf &&
          !test_cpu.xf && test_cpu.i, "native-mode bootstrap");

    check(RtlGameFrameBegin() == 0 && test_snes.forceNmi &&
              test_snes.nmiAvail && test_snes.vPos == 225u &&
              test_snes.inVblank,
          "direct game-frame begin adapter");
    check(RtlGameFrameComplete(UINT32_C(0x80000000)) == -1 &&
              test_snes.forceNmi,
          "invalid game-frame completion preserves timing state");
    test_snes.inNmi = true;
    test_snes.nmiEnabled = false;
    check(RtlGameFrameComplete(
              RTL_GAME_FRAME_DISPATCH_NMI_IF_ENABLED) == 0 &&
              !test_snes.forceNmi && test_snes.inNmi,
          "disabled NMI gate reports no new transition");
    test_snes.nmiEnabled = true;
    check(RtlGameFrameComplete(
              RTL_GAME_FRAME_DISPATCH_NMI_IF_ENABLED) ==
                  RTL_GAME_FRAME_NMI_ENTERED &&
              test_snes.inNmi,
          "enabled NMI gate reports transition");
    test_snes.inNmi = false;
    test_snes.nmiEnabled = false;

    test_ppu.inidisp = 0x8fu;
    test_ppu.bgmode = 0x17u;
    test_ppu.screenEnabled[0] = 0x15u;
    test_ppu.screenEnabled[1] = 0x0au;
    {
        const uint32_t display = RtlGamePpuDisplayState();
        check(RTL_GAME_PPU_DISPLAY_CONTROL(display) == 0x8fu &&
                  RTL_GAME_PPU_BG_MODE_CONTROL(display) == 0x17u &&
                  RTL_GAME_PPU_MAIN_SCREEN(display) == 0x15u &&
                  RTL_GAME_PPU_SUB_SCREEN(display) == 0x0au,
              "direct PPU display-state adapter");
    }
    {
        const SrPpuFramePolicy policy = {
            .struct_size = sizeof(policy),
            .horizontal_mode = SR_PPU_HORIZONTAL_MARGIN_CENTERED,
        };
        check(RtlGameApplyPpuFramePolicy(&policy) == SR_RESULT_OK &&
                  ppu_frame_policy_count == 1,
              "direct PPU frame-policy adapter");
    }

    test_spc.x = 0x12u;
    g_apu_spc_dsp_write_hook(&test_apu, 0x74u, 0x09u);
    check(dsp_routing_count == 1 &&
              test_dsp.voiceBus[7] == RTL_AUDIO_VOICE_BUS_SFX,
          "DSP routing update application");
    memset(test_dsp.voiceBus, 0, sizeof(test_dsp.voiceBus));
    rtl_game_audio_state_loaded(&test_apu);
    check(state_routing_count == 1 &&
              test_dsp.voiceBus[0] == RTL_AUDIO_VOICE_BUS_MUSIC &&
              test_dsp.voiceBus[6] == RTL_AUDIO_VOICE_BUS_SFX,
          "state-loaded routing plan application");

    test_spc.pc = 0x1234u;
    test_spc.x = 0x12u;
    {
        uint8 value = 0x09u;
        check(!g_apu_spc_dsp_write_filter_hook(
                  &test_apu, 0x74u, &value) && extension_dsp_count == 1 &&
                  virtual_register_voice == 8 &&
                  virtual_register_address == 0x74u &&
                  virtual_register_value == 0x09u,
              "audio-extension DSP bridge");
    }
    g_spc_opcode_patch_hook(&test_spc, 0x1234u);
    check(extension_opcode_count == 1 && test_spc.pc == 0x4567u &&
              test_spc.x == 0x34u && test_spc.z,
          "audio-extension SPC mutation bridge");
    check(g_spc_opcode_cycle_hook(&test_spc, 0x1234u, 5) == 0,
          "audio-extension cycle bridge");
    {
        SaveLoadInfo info = {
            .func = transfer_extension_state,
            .saving = true,
            .portable = true,
        };
        g_apu_extra_saveload_hook(&test_apu, &info);
        check(extension_save_count == 1 && extension_saved_bytes == 4u,
              "audio-extension save bridge");
    }
    RtlApuLock();
    RtlAudioExtensionNotifyUploadLocked(0x1a94b8u);
    RtlApuUnlock();
    check(extension_upload_count == 1 && apu_lock_count == 1 &&
              apu_unlock_count == 1,
          "audio-extension upload lock bridge");

    check(SnesInit(NULL, 0) == &test_snes, "ROM-free initialization");
    check(free_count == 1, "reinitialization releases previous runner");
    check(initialize_count == 2, "ROM-free callback");
    check(abi_bind_count == 2 && abi_unbind_count == 1 &&
              abi_bound_runner == (SrRunnerHandle *)(void *)&test_snes,
          "runner ABI rebind");
    check(ppu_reset_count == 1 && dma_reset_count == 1,
          "ROM-free device resets");
    check(g_sram_size == 2048, "ROM-free SRAM allocation");

    load_rom_success = false;
    check(SnesInit(test_rom, (int)sizeof(test_rom)) == NULL,
          "ROM load failure is reported");
    check(free_count == 3, "failed replacement releases both runner instances");
    check(abi_bind_count == 3 && abi_unbind_count == 3 &&
              abi_bound_runner == NULL,
          "failed initialization revokes runner ABI");
    check(g_snes == NULL && g_snes_cpu == NULL && g_ppu == NULL &&
              g_dma == NULL && g_rom == NULL && g_sram == NULL &&
              g_sram_size == 0,
          "failed initialization clears published state");
    load_rom_success = true;

    initialize_success = false;
    check(SnesInit(test_rom, (int)sizeof(test_rom)) == NULL,
          "game initialization failure is reported");
    check(free_count == 4 && initialize_count == 3 &&
              abi_bind_count == 4 && abi_unbind_count == 4,
          "failed game initialization revokes runner services");
    initialize_success = true;

    check(SnesInit(test_rom, (int)sizeof(test_rom)) == &test_snes,
          "runner can initialize after failure");
    SnesShutdown();
    check(free_count == 5 && g_snes == NULL,
          "explicit shutdown is idempotent and clears runner");
    check(RtlGamePpuDisplayState() == 0u,
          "PPU display-state adapter is safe without a runner");
    {
        const SrPpuFramePolicy policy = {
            .struct_size = sizeof(policy),
            .horizontal_mode = SR_PPU_HORIZONTAL_MARGIN_CENTERED,
        };
        check(RtlGameApplyPpuFramePolicy(&policy) == SR_RESULT_UNAVAILABLE &&
                  ppu_frame_policy_count == 1,
              "PPU frame-policy adapter is safe without a runner");
    }
    check(abi_bind_count == 5 && abi_unbind_count == 5 &&
              abi_bound_runner == NULL,
          "runner ABI shutdown revoke");
    check(cpu_provider_bind_count == 5 && cpu_provider_unbind_count == 5 &&
              execution_provider_bind_count == 5 &&
              execution_provider_unbind_count == 5 &&
              ppu_service_bind_count == 5 &&
              ppu_service_unbind_count == 5 &&
              trace_bind_count == 5 && trace_unbind_count == 5,
          "runner-owned services are symmetrically revoked");
    SnesShutdown();
    check(free_count == 5, "repeated shutdown is harmless");

    RtlAudioExtensionConfigure(false);
    check(RtlRegisterGame(&module) == SR_RESULT_OK,
          "game module re-registration");
    check(!extended_voices_enabled &&
              g_apu_spc_dsp_write_filter_hook == NULL &&
              g_spc_opcode_patch_hook == NULL &&
              g_spc_opcode_cycle_hook == NULL &&
              g_apu_extra_saveload_hook == NULL,
          "disabled audio extension leaves no hot hooks");
}

static void test_indirect_pointer(void) {
    g_snes_cpu = &test_cpu;
    test_cpu.db = 0x7eu;
    g_ram[0x20] = 0x34u;
    g_ram[0x21] = 0x82u;
    check(IndirPtrDB(0x20u, 2u) == &g_ram[0x8236],
          "WRAM indirect pointer");
    test_cpu.db = 0x01u;
    check(IndirPtrDB(0x20u, 2u) == &test_rom[0x8236],
          "ROM indirect pointer");
    g_ram[0xff] = 0xfeu;
    g_ram[0x00] = 0x1fu;
    check(IndirPtrDB(0xffu, 1u) == &g_ram[0x1fff],
          "direct-page pointer byte wrap");
}

static void test_block_history(void) {
    CpuState cpu;
    uint32 output[4];
    memset(&cpu, 0, sizeof(cpu));
    g_sr_block_index = 0u;
    cpu.X = 0x4567u;
    cpu.S = 0x01e0u;
    cpu.m_flag = 1u;
    cpu.x_flag = 0u;
    cpu_trace_block(&cpu, 0x123456u);
    cpu_trace_block(&cpu, 0x234567u);
    check(sr_block_history_available() == 2,
          "block history available count");
    check(sr_block_history(output, 4) == 2, "block history count");
    check(output[0] == 0x123456u && output[1] == 0x234567u,
          "block history order");
    check(sr_block_history(output, 1) == 1 &&
              sr_block_history_available() == 2,
          "block history reports truncated copy");
    check(output[0] == 0x234567u,
          "truncated block history keeps newest window");
    check(g_sr_block_aux[0] == 0x14567u && g_sr_block_stack[0] == 0x01e0u,
          "block register metadata");
    g_sr_block_index = kRuntimeBlockTraceRingCapacity + 7u;
    check(sr_block_history_available() == kRuntimeBlockTraceRingCapacity,
          "block history available count caps at ring capacity");
    g_sr_block_index = 2u;
}

static void test_stack_and_tailcalls(void) {
    uint16 entry_stack = 0u;
    uint8 hrv = 0u;
    g_recomp_stack_top = 0;
    g_recomp_push_count = 0;
    RecompStackPush("outer");
    RecompStackPush("inner");
    check(g_recomp_stack_top == 2 && g_recomp_push_count == 2,
          "recomp stack push");
    check(strcmp(g_last_recomp_func, "inner") == 0, "last function on push");
    RecompStackPop();
    check(g_recomp_stack_top == 1 && strcmp(g_last_recomp_func, "outer") == 0,
          "recomp stack pop");

    cpu_tailcall_inherit_return_context(0x01d0u, 1u);
    check(cpu_take_tailcall_return_context(&entry_stack, &hrv) == 1 &&
          entry_stack == 0x01d0u && hrv == 1u, "tailcall inherited context");
    check(cpu_take_tailcall_return_context(NULL, NULL) == 0,
          "tailcall context is one-shot");
    cpu_tailcall_request(0xff123456u, 0x01c0u, 0xaa654321u);
    check(g_tailcall_pc24 == 0x123456u && g_tailcall_miss_s == 0x01c0u &&
          g_tailcall_src24 == 0x654321u, "tailcall request masking");
}

static void test_ancestor_skip(void) {
    g_recomp_stack_top = 4;
    g_cpu_entry_s[0] = 0x01ffu;
    g_cpu_entry_s[1] = 0x01fdu;
    g_cpu_entry_s[2] = 0x01fbu;
    g_cpu_entry_s[3] = 0x01f9u;
    memset(g_cpu_entry_hrv, 0, 4);
    check(cpu_resolve_ancestor_skip(0x01fdu) == 2,
          "nearest ancestor distance");
    g_cpu_entry_hrv[2] = 1u;
    check(cpu_resolve_ancestor_skip(0x01fdu) == 2,
          "hardware-stack ancestor crosses paired host frame");
    check(cpu_resolve_ancestor_skip(0x01fbu) == 1,
          "nearest paired frame remains a valid target");
}

int main(void) {
    test_registration_and_initialization();
    test_indirect_pointer();
    test_block_history();
    test_stack_and_tailcalls();
    test_ancestor_skip();
    WatchdogFrameStart();
    WatchdogCheck();
    WatchdogFrameEnd();
    check(g_watchdog_loop_headers == 1u && !g_watchdog_tripped,
          "production watchdog accounting");
    if (failures != 0) return 1;
    puts("runtime CPU infra: PASS");
    return 0;
}
