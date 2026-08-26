#include "common_cpu_infra.h"

#include "common_rtl.h"
#include "cpu_state.h"
#include "cpu_trace.h"
#include "debug_server.h"
#include "snes/cart.h"
#include "snes/cpu.h"
#include "snes/dma.h"
#include "snes/msu1.h"
#include "snes/ppu.h"
#include "snes/snes.h"
#include "runner_next_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) && AR_WATCHDOG
#include <windows.h>
#endif

enum { kRecompStackCapacity = 64 };

Snes *g_snes;
Cpu *g_snes_cpu;
bool g_fail;
const RtlGameInfo *g_rtl_game_info;

const char *g_last_recomp_func = "(none)";
const char *g_recomp_stack[kRecompStackCapacity];
int g_recomp_stack_top;
unsigned long g_recomp_push_count;
uint16 g_cpu_entry_s[kRecompStackCapacity];
uint8 g_cpu_entry_hrv[kRecompStackCapacity];

uint32 g_ar_blk_ring[kRuntimeBlockTraceRingCapacity];
uint32 g_ar_blk_aux[kRuntimeBlockTraceRingCapacity];
uint16 g_ar_blk_s[kRuntimeBlockTraceRingCapacity];
unsigned g_ar_blk_idx;

uint32 g_tailcall_pc24;
uint16 g_tailcall_miss_s;
uint32 g_tailcall_src24;
static uint16 g_tailcall_entry_s;
static uint8 g_tailcall_hrv;
static bool g_tailcall_context_valid;

uint64 g_watchdog_loop_headers;
int g_watchdog_tripped;

void RtlRegisterGame(const RtlGameInfo *info) {
    g_rtl_game_info = info;
    g_snes_rdnmi_read_hook = info != NULL ? info->read_rdnmi : NULL;
    msu1_init();
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
    unsigned slot = g_ar_blk_idx++ & kRuntimeBlockTraceRingMask;
    g_ar_blk_ring[slot] = pc24 & 0xffffffu;
    g_ar_blk_aux[slot] = ((uint32)(cpu->x_flag & 1u) << 17) |
                         ((uint32)(cpu->m_flag & 1u) << 16) | cpu->X;
    g_ar_blk_s[slot] = cpu->S;
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

int ar_block_history(uint32 *output, int maximum) {
    unsigned available;
    int count;
    int index;
    if (output == NULL || maximum <= 0) return 0;
    if (maximum > kRuntimeBlockTraceRingCapacity) {
        maximum = kRuntimeBlockTraceRingCapacity;
    }
    available = g_ar_blk_idx;
    count = available < (unsigned)maximum ? (int)available : maximum;
    for (index = 0; index < count; ++index) {
        output[index] = g_ar_blk_ring[
            (g_ar_blk_idx - (unsigned)count + (unsigned)index) &
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
    debug_server_profile_push(safe_name);
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

#if AR_WATCHDOG
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
    debug_server_profile_latch(snes_frame_counter);
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
    if (g_snes != NULL && g_rtl_game_info != NULL &&
        g_rtl_game_info->bind_runner_abi != NULL) {
        g_rtl_game_info->bind_runner_abi(g_snes, false);
    }
    sr_runner_clear_event_subscriptions(g_snes);
    g_snes = NULL;
    g_snes_cpu = NULL;
    g_dma = NULL;
    g_ppu = NULL;
    g_rom = NULL;
    g_sram = NULL;
    g_sram_size = 0;
}

void SnesShutdown(void) {
    Snes *snes = g_snes;
    clear_published_runner();
    snes_free(snes);
}

Snes *SnesInit(const uint8 *data, int data_size) {
    bool loaded;
    if (data_size < 0 || (data_size > 0 && data == NULL) ||
        g_rtl_game_info == NULL) return NULL;
    SnesShutdown();
    g_snes = snes_init(g_ram);
    if (g_snes == NULL) return NULL;
    if (g_rtl_game_info->bind_runner_abi != NULL) {
        g_rtl_game_info->bind_runner_abi(g_snes, true);
    }
    g_snes_cpu = g_snes->cpu;
    g_dma = g_snes->dma;
    g_ppu = g_snes->ppu;
    if (data_size > 0) {
        loaded = snes_loadRom(g_snes, data, data_size);
        if (!loaded) goto fail;
        g_rom = g_snes->cart->rom;
        if (g_rtl_game_info->initialize != NULL) {
            g_rtl_game_info->initialize();
        }
        snes_reset(g_snes, true);
        SnesEnterNativeMode();
    } else {
        uint8 *ram = (uint8 *)calloc(2048u, 1u);
        if (ram == NULL) goto fail;
        g_snes->cart->ram = ram;
        g_snes->cart->ramSize = 2048u;
        if (g_rtl_game_info->initialize != NULL) {
            g_rtl_game_info->initialize();
        }
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
