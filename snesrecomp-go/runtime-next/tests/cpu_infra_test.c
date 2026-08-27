#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "runner_next_internal.h"
#include "snes/apu.h"
#include "snes/cart.h"
#include "snes/cpu.h"
#include "snes/dma.h"
#include "snes/dsp.h"
#include "snes/saveload.h"
#include "snes/snes.h"
#include "snes/spc.h"

#include <stdio.h>
#include <string.h>

struct Ppu { int marker; };

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
void (*g_rtl_apu_state_loaded_hook)(Apu *);

void sr_runner_emit_event(Snes *snes, SrEventMask event_mask,
                          SrRunnerEvent *event) {
    (void)snes;
    (void)event_mask;
    (void)event;
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
static Snes *abi_bound_runner;
static bool load_rom_success = true;
static int failures;
static int dsp_routing_count;
static int state_routing_count;
static int extension_dsp_count;
static int extension_opcode_count;
static int extension_save_count;
static int extension_upload_count;
static int apu_lock_count;
static int apu_unlock_count;
static bool extended_voices_enabled;
static int virtual_register_voice = -1;
static uint8 virtual_register_address;
static uint8 virtual_register_value;
static size_t extension_saved_bytes;

static void check(int condition, const char *message) {
    if (condition) return;
    ++failures;
    fprintf(stderr, "runtime-next CPU infra failed: %s\n", message);
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

static void initialize_game(void) { ++initialize_count; }
static int read_rdnmi(Snes *snes) { (void)snes; return 1; }
static void bind_runner_abi(Snes *snes, bool enabled) {
    if (enabled) {
        ++abi_bind_count;
        abi_bound_runner = snes;
    } else {
        ++abi_unbind_count;
        if (abi_bound_runner == snes) abi_bound_runner = NULL;
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
    static const RtlGameInfo info = {
        .title = "test",
        .initialize = initialize_game,
        .read_rdnmi = read_rdnmi,
        .save_name_prefix = "test",
        .audio_dsp_write_routing = route_dsp_write,
        .audio_state_loaded_routing = route_state_loaded,
        .audio_extension_dsp_write = route_extension_dsp_write,
        .audio_extension_spc_opcode = route_extension_spc_opcode,
        .audio_extension_spc_cycle = route_extension_spc_cycle,
        .audio_extension_save = route_extension_save,
        .audio_extension_upload = route_extension_upload,
        .bind_runner_abi = bind_runner_abi,
    };
    RtlAudioExtensionConfigure(true);
    RtlRegisterGame(&info);
    check(g_rtl_game_info == &info, "game registration");
    check(g_snes_rdnmi_read_hook == read_rdnmi, "RDNMI hook registration");
    check(g_apu_spc_dsp_write_hook != NULL,
          "DSP routing hook registration");
    check(g_rtl_apu_state_loaded_hook != NULL,
          "state-loaded routing hook registration");
    check(extended_voices_enabled &&
              g_apu_spc_dsp_write_filter_hook != NULL &&
              g_spc_opcode_patch_hook != NULL &&
              g_spc_opcode_cycle_hook != NULL &&
              g_apu_extra_saveload_hook != NULL,
          "audio-extension hook registration");
    check(msu_init_count == 1, "MSU initialization");

    check(SnesInit(test_rom, (int)sizeof(test_rom)) == &test_snes,
          "ROM-backed initialization");
    check(initialize_count == 1, "game initialization callback");
    check(abi_bind_count == 1 && abi_unbind_count == 0 &&
              abi_bound_runner == &test_snes,
          "runner ABI initial bind");
    check(reset_count == 1, "hard reset after ROM load");
    check(g_snes_cpu == &test_cpu && g_ppu == &test_ppu && g_dma == &test_dma,
          "device publication");
    check(g_rom == test_rom && g_sram == g_ram + 0x18000 &&
          g_sram_size == 0x2000, "cartridge memory publication");
    check(!test_cpu.e && test_cpu.sp == 0x01ffu && !test_cpu.mf &&
          !test_cpu.xf && test_cpu.i, "native-mode bootstrap");

    test_spc.x = 0x12u;
    g_apu_spc_dsp_write_hook(&test_apu, 0x74u, 0x09u);
    check(dsp_routing_count == 1 &&
              test_dsp.voiceBus[7] == RTL_AUDIO_VOICE_BUS_SFX,
          "DSP routing update application");
    memset(test_dsp.voiceBus, 0, sizeof(test_dsp.voiceBus));
    g_rtl_apu_state_loaded_hook(&test_apu);
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
              abi_bound_runner == &test_snes,
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

    check(SnesInit(test_rom, (int)sizeof(test_rom)) == &test_snes,
          "runner can initialize after failure");
    SnesShutdown();
    check(free_count == 4 && g_snes == NULL,
          "explicit shutdown is idempotent and clears runner");
    check(abi_bind_count == 4 && abi_unbind_count == 4 &&
              abi_bound_runner == NULL,
          "runner ABI shutdown revoke");
    SnesShutdown();
    check(free_count == 4, "repeated shutdown is harmless");

    RtlAudioExtensionConfigure(false);
    RtlRegisterGame(&info);
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
    g_ar_blk_idx = 0u;
    cpu.X = 0x4567u;
    cpu.S = 0x01e0u;
    cpu.m_flag = 1u;
    cpu.x_flag = 0u;
    cpu_trace_block(&cpu, 0x123456u);
    cpu_trace_block(&cpu, 0x234567u);
    check(ar_block_history(output, 4) == 2, "block history count");
    check(output[0] == 0x123456u && output[1] == 0x234567u,
          "block history order");
    check(g_ar_blk_aux[0] == 0x14567u && g_ar_blk_s[0] == 0x01e0u,
          "block register metadata");
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
    puts("runtime-next CPU infra: PASS");
    return 0;
}
