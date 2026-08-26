#include "common_cpu_infra.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "snes/cart.h"
#include "snes/cpu.h"
#include "snes/dma.h"
#include "snes/snes.h"

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

static Snes test_snes;
static Cpu test_cpu;
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
static bool load_rom_success = true;
static int failures;

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
    test_snes.ram = ram;
    test_snes.cpu = &test_cpu;
    test_snes.cart = &test_cart;
    test_snes.dma = &test_dma;
    test_snes.ppu = &test_ppu;
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

uint8 *RomPtr(uint32 address) {
    return &test_rom[address & (sizeof(test_rom) - 1u)];
}

static void initialize_game(void) { ++initialize_count; }
static int read_rdnmi(Snes *snes) { (void)snes; return 1; }

static void test_registration_and_initialization(void) {
    static const RtlGameInfo info = {
        .title = "test",
        .initialize = initialize_game,
        .read_rdnmi = read_rdnmi,
        .save_name_prefix = "test",
    };
    RtlRegisterGame(&info);
    check(g_rtl_game_info == &info, "game registration");
    check(g_snes_rdnmi_read_hook == read_rdnmi, "RDNMI hook registration");
    check(msu_init_count == 1, "MSU initialization");

    check(SnesInit(test_rom, (int)sizeof(test_rom)) == &test_snes,
          "ROM-backed initialization");
    check(initialize_count == 1, "game initialization callback");
    check(reset_count == 1, "hard reset after ROM load");
    check(g_snes_cpu == &test_cpu && g_ppu == &test_ppu && g_dma == &test_dma,
          "device publication");
    check(g_rom == test_rom && g_sram == g_ram + 0x18000 &&
          g_sram_size == 0x2000, "cartridge memory publication");
    check(!test_cpu.e && test_cpu.sp == 0x01ffu && !test_cpu.mf &&
          !test_cpu.xf && test_cpu.i, "native-mode bootstrap");

    check(SnesInit(NULL, 0) == &test_snes, "ROM-free initialization");
    check(free_count == 1, "reinitialization releases previous runner");
    check(initialize_count == 2, "ROM-free callback");
    check(ppu_reset_count == 1 && dma_reset_count == 1,
          "ROM-free device resets");
    check(g_sram_size == 2048, "ROM-free SRAM allocation");

    load_rom_success = false;
    check(SnesInit(test_rom, (int)sizeof(test_rom)) == NULL,
          "ROM load failure is reported");
    check(free_count == 3, "failed replacement releases both runner instances");
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
    SnesShutdown();
    check(free_count == 4, "repeated shutdown is harmless");
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
