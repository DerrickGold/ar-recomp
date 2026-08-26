#include "common_cpu_infra.h"
#include "cpu_state.h"

#include <stdio.h>
#include <string.h>

uint8 g_ram[kSnesWramSize];
static uint8 rom[0x100000];
static uint8 save_ram[0x8000];
uint8 *g_sram = save_ram;
int g_sram_size = sizeof(save_ram);
uint64_t g_main_cpu_cycles_estimate;
uint64_t g_apu_pace_cycles_estimate;
int snes_frame_counter;
const char *g_last_recomp_func = "cpu-test";
uint32_t g_ar_blk_ring[kRuntimeBlockTraceRingCapacity];
unsigned g_ar_blk_idx;
uint32_t g_tailcall_pc24;
uint16_t g_tailcall_miss_s;
uint32_t g_tailcall_src24;
static uint16 last_register;
static uint16 last_register_value;
static unsigned handler_calls;
static uint8 selected_variant;
static unsigned continuation_calls;
static uint8 continuation_mx;
static uint8 continuation_host_return_valid;

static RecompReturn variant_handler(CpuState *cpu, uint8 variant) {
    ++handler_calls;
    selected_variant = variant;
    ++cpu->A;
    return RECOMP_RETURN_SKIP_1;
}

static RecompReturn handler_m0x0(CpuState *cpu) {
    return variant_handler(cpu, 0u);
}
static RecompReturn handler_m0x1(CpuState *cpu) {
    return variant_handler(cpu, 1u);
}
static RecompReturn handler_m1x0(CpuState *cpu) {
    return variant_handler(cpu, 2u);
}
static RecompReturn handler_m1x1(CpuState *cpu) {
    return variant_handler(cpu, 3u);
}

static RecompReturn tail_handler(CpuState *cpu) {
    (void)cpu;
    g_tailcall_pc24 = 0x018000u;
    g_tailcall_miss_s = 0x01f0u;
    g_tailcall_src24 = 0x028000u;
    return RECOMP_RETURN_TAILCALL;
}

static RecompReturn continuation_handler(CpuState *cpu) {
    ++continuation_calls;
    continuation_mx = (uint8)(((cpu->m_flag & 1u) << 1) |
                              (cpu->x_flag & 1u));
    continuation_host_return_valid = cpu->host_return_valid;
    return RECOMP_RETURN_SKIP_2;
}

static bool recover_test_dispatch(uint32 source_pc24, uint32 target_pc24) {
    (void)target_pc24;
    return source_pc24 == 0x018900u;
}

static const RtlGameInfo test_game_info = {
    .title = "cpu-state-test",
    .recover_dispatch_miss = recover_test_dispatch,
    .save_name_prefix = "test",
};

const DispatchEntry g_dispatch_table[] = {
    {0x018000u, {handler_m0x0, handler_m0x1,
                 handler_m1x0, handler_m1x1}},
    {0x018300u, {continuation_handler, continuation_handler,
                 continuation_handler, continuation_handler}},
    {0x018400u, {continuation_handler, continuation_handler,
                 continuation_handler, continuation_handler}},
    {0x028000u, {tail_handler, NULL, NULL, tail_handler}},
};
const unsigned g_dispatch_table_count =
    sizeof(g_dispatch_table) / sizeof(g_dispatch_table[0]);
const RtlGameInfo *g_rtl_game_info;

uint8 ReadReg(uint16 reg) {
    last_register = reg;
    return 0x5au;
}
uint16 ReadRegWord(uint16 reg) {
    last_register = reg;
    return 0xa55au;
}
void WriteReg(uint16 reg, uint8 value) {
    last_register = reg;
    last_register_value = value;
}
void WriteRegWord(uint16 reg, uint16 value) {
    last_register = reg;
    last_register_value = value;
}
uint8 *RomPtr(uint32 address) {
    return &rom[(((address >> 16) << 15) | (address & 0x7fffu)) &
                (sizeof(rom) - 1u)];
}

static int failures;
static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next CPU state failed: %s\n", message);
        ++failures;
    }
}

static void test_registers(void) {
    CpuState cpu;
    cpu_state_init(&cpu, g_ram);
    check(cpu.S == 0x01ffu && cpu.P == 0x34u && cpu.m_flag && cpu.x_flag &&
              cpu.emulation && cpu._flag_I,
          "reset state");
    cpu.A = 0xab00u;
    cpu_write_a8(&cpu, 0x34u);
    check(cpu.A == 0xab34u && cpu_read_b(&cpu) == 0xabu,
          "8-bit accumulator preservation");
    cpu.X = 0xabcd;
    cpu_write_x8(&cpu, 0x12u);
    check(cpu.X == 0x12u, "8-bit index zero extension");
    cpu.P = CPU_P_N | CPU_P_X | CPU_P_C;
    cpu.X = 0xff12u;
    cpu_p_to_mirrors(&cpu);
    check(cpu._flag_N && cpu._flag_C && cpu.x_flag && cpu.X == 0x12u,
          "P-to-mirror synchronization");
    cpu_mirrors_to_p(&cpu);
    check(cpu.P == (CPU_P_N | CPU_P_X | CPU_P_C),
          "mirror-to-P synchronization");
}

static void test_memory(void) {
    CpuState cpu;
    cpu_state_init(&cpu, g_ram);
    memset(g_ram, 0, sizeof(g_ram));
    cpu_write16(&cpu, 0x7eu, 0x1234u, 0xbeefu);
    check(cpu_read16(&cpu, 0x00u, 0x1234u) == 0xbeefu,
          "WRAM low-bank mirror");
    cpu_write8(&cpu, 0x7fu, 0x4321u, 0x77u);
    check(g_ram[0x14321u] == 0x77u, "WRAM high bank");
    cpu_write16(&cpu, 0x00u, 0x2140u, 0x2211u);
    check(last_register == 0x2140u && last_register_value == 0x2211u &&
              g_apu_pace_cycles_estimate == 256u,
          "hardware-register routing and APU pacing");
    check(cpu_read8(&cpu, 0x80u, 0x2100u) == 0x5au &&
              last_register == 0x2100u,
          "mirrored hardware read");
    cpu_write8(&cpu, 0x70u, 0x0010u, 0x66u);
    check(cpu_read8(&cpu, 0xf0u, 0x0010u) == 0x66u,
          "LoROM SRAM mirrors");
    *RomPtr(0x018123u) = 0x9cu;
    check(cpu_read8(&cpu, 0x01u, 0x8123u) == 0x9cu, "ROM read");
}

static void test_stack_and_dispatch(void) {
    CpuState cpu;
    cpu_state_init(&cpu, g_ram);
    cpu.emulation = 0u;
    cpu.PB = 3u;
    cpu._flag_C = 1u;
    cpu_push_interrupt_frame(&cpu);
    check(cpu.S == 0x01fbu && g_ram[0x01ffu] == 3u &&
              g_ram[0x01fcu] == (CPU_P_M | CPU_P_X | CPU_P_I | CPU_P_C),
          "native interrupt frame");
    cpu_state_init(&cpu, g_ram);
    cpu.m_flag = 0u;
    cpu.x_flag = 0u;
    cpu.A = 10u;
    handler_calls = 0u;
    selected_variant = 0xffu;
    check(cpu_dispatch_pc(&cpu, 0x818000u, 0x01f0u) ==
              RECOMP_RETURN_SKIP_1 &&
              cpu.A == 11u && handler_calls == 1u && selected_variant == 0u,
          "LoROM mirror dispatch variant");
    cpu.S = 0x0100u;
    check(cpu_dispatch_pc(&cpu, 0x777777u, 0x0123u) ==
              RECOMP_RETURN_NORMAL && cpu.S == 0x0123u,
          "dispatch miss stack restore");
    cpu.S = 0x0100u;
    *RomPtr(0x018500u) = 0x60u;
    g_ram[0x0101u] = 0xffu;
    g_ram[0x0102u] = 0x7fu;
    check(cpu_dispatch_pc(&cpu, 0x018500u, 0x0130u) ==
              RECOMP_RETURN_SKIP_1 && handler_calls == 2u &&
              cpu.S == 0x0102u,
          "bare RTS dispatch following");
    cpu.S = 0x0100u;
    *RomPtr(0x018510u) = 0x6bu;
    g_ram[0x0101u] = 0xffu;
    g_ram[0x0102u] = 0x7fu;
    g_ram[0x0103u] = 0x01u;
    check(cpu_dispatch_pc(&cpu, 0x018510u, 0x0130u) ==
              RECOMP_RETURN_SKIP_1 && handler_calls == 3u &&
              cpu.S == 0x0103u,
          "bare RTL dispatch following");
    cpu.S = 0x0100u;
    *RomPtr(0x018520u) = 0x60u;
    g_ram[0x0101u] = 0xffu;
    g_ram[0x0102u] = 0x86u;
    check(cpu_dispatch_pc(&cpu, 0x018520u, 0x0130u) ==
              RECOMP_RETURN_NORMAL && cpu.S == 0x0130u,
          "bare-return chain miss restores caller stack");
    cpu.S = 0x01f0u;
    check(cpu_dispatch_pc(&cpu, 0x028000u, 0x01f0u) ==
              RECOMP_RETURN_SKIP_1 && handler_calls == 4u,
          "flat tail dispatch");
}

static void test_dispatch_mx_variants(void) {
    CpuState cpu;
    unsigned variant;
    for (variant = 0u; variant < 4u; ++variant) {
        cpu_state_init(&cpu, g_ram);
        cpu.emulation = 0u;
        cpu.m_flag = (uint8)(variant >> 1);
        cpu.x_flag = (uint8)(variant & 1u);
        selected_variant = 0xffu;
        check(cpu_dispatch_pc(&cpu, 0x018000u, 0x01ffu) ==
                  RECOMP_RETURN_SKIP_1 && selected_variant == variant,
              "dispatch selects exact runtime M/X variant");
    }
}

static void test_recovered_branch_handlers(void) {
    CpuState cpu;
    cpu_state_init(&cpu, g_ram);
    cpu.emulation = 0u;
    cpu.m_flag = 0u;
    cpu.x_flag = 1u;
    cpu.S = 0x01e0u;
    g_rtl_game_info = &test_game_info;

    *RomPtr(0x0183f0u) = 0x80u; /* BRA $8400 */
    *RomPtr(0x0183f1u) = 0x0eu;
    continuation_calls = 0u;
    check(cpu_dispatch_pc_from(&cpu, 0x0183f0u, 0x01f0u, 0x018900u) ==
              RECOMP_RETURN_SKIP_2 && continuation_calls == 1u &&
              continuation_mx == 1u && cpu.S == 0x01e0u,
          "approved BRA handler resolves without consuming an RTS frame");

    *RomPtr(0x018100u) = 0x82u; /* BRL $8400 */
    *RomPtr(0x018101u) = 0xfdu;
    *RomPtr(0x018102u) = 0x02u;
    continuation_calls = 0u;
    check(cpu_dispatch_pc_from(&cpu, 0x018100u, 0x01f0u, 0x018900u) ==
              RECOMP_RETURN_SKIP_2 && continuation_calls == 1u &&
              continuation_mx == 1u && cpu.S == 0x01e0u,
          "approved BRL handler resolves without consuming an RTS frame");
    g_rtl_game_info = NULL;
}

static void test_recovered_handler_continuation(void) {
    CpuState cpu;
    cpu_state_init(&cpu, g_ram);
    cpu.emulation = 0u;
    cpu.m_flag = 1u;
    cpu.x_flag = 0u;
    cpu.PB = 1u;
    cpu.S = 0x01e0u;

    /* A policy-approved computed handler may be data rather than emitted code.
     * Treating that miss as an ordinary host unwind skips the handler's RTS
     * continuation, including any REP/SEP epilogue located there. */
    *RomPtr(0x018200u) = 0xeau; /* unresolved handler body (not BRA/BRL/RTS) */
    g_ram[0x01e1u] = 0xffu;    /* RTS return address $82ff -> $8300 */
    g_ram[0x01e2u] = 0x82u;
    continuation_calls = 0u;
    continuation_mx = 0xffu;
    continuation_host_return_valid = 0xffu;
    g_rtl_game_info = &test_game_info;

    check(cpu_dispatch_pc_from(&cpu, 0x018200u, 0x01f0u, 0x018900u) ==
              RECOMP_RETURN_SKIP_2,
          "approved unresolved handler resumes its RTS continuation");
    check(continuation_calls == 1u && continuation_mx == 2u,
          "recovered continuation preserves runtime M/X variant");
    check(cpu.S == 0x01e2u && continuation_host_return_valid == 0u,
          "recovered continuation consumes handler frame and is unpaired");
    g_rtl_game_info = NULL;
}

int main(void) {
    test_registers();
    test_memory();
    test_stack_and_dispatch();
    test_dispatch_mx_variants();
    test_recovered_branch_handlers();
    test_recovered_handler_continuation();
    return failures == 0 ? 0 : 1;
}
