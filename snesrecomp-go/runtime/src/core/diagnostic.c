#include "diagnostic.h"

#include "runtime_trace.h"
#include "snesrecomp/game/bootstrap.h"
#include "snesrecomp/game/generated_support.h"
#include "runner_state_internal.h"
#include "snesrecomp/game/runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    kMxHistoryCapacity = 4096,
    kMxWarmupCount = 64,
    kXTraceCapacity = 512,
    kWarningCapacity = 256,
};

typedef struct MxHistoryEntry {
    uint32 pc24;
    uint32 count[4];
    bool warned;
} MxHistoryEntry;

typedef struct XTraceEntry {
    uint32 block_pc24;
    uint32 next_pc24;
    uint32 game_frame;
    uint8 new_x;
    uint8 m;
} XTraceEntry;

typedef struct WarningKey {
    uint32 first;
    uint32 second;
} WarningKey;

int g_sr_entry_mx_check_enabled;
int g_sr_mx_history_enabled;
const char *g_sr_trap_function;
int g_sr_exit_mx_check_enabled;
int g_sr_exit_stack_check_enabled;
int g_sr_call_mx_check_enabled;
uint32 *g_stack_pusher;
unsigned *g_stack_pusher_frame;

static MxHistoryEntry g_mx_history[kMxHistoryCapacity];
static XTraceEntry g_xtrace[kXTraceCapacity];
static unsigned g_xtrace_index;
static WarningKey g_oob_warning[kWarningCapacity];
static unsigned g_oob_warning_count;
/* Recompiler traps report once per site, or once per unresolved target for a
 * shared stub body. They are deliberately compiled unconditionally: before,
 * the stub and cross-function goto traps existed only under SNESRECOMP_TRACE
 * and were silent no-ops in an ordinary build, so a trap that fired during
 * bring-up produced no output at all and the failure surfaced later as
 * unexplained corruption or a hang. */
static WarningKey g_trap_warning[kWarningCapacity];
static unsigned g_trap_warning_count;

static bool g_trapfn_fired;
static int g_strace_enabled = -1;
static uint32 g_strace_low;
static uint32 g_strace_high;
static unsigned g_strace_count;

static uint32 string_hash(const char *text) {
    uint32 hash = 2166136261u;
    if (text == NULL) return 0u;
    while (*text != '\0') {
        hash ^= (uint8)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static MxHistoryEntry *mx_entry(uint32 pc24, bool create) {
    unsigned probe;
    unsigned slot = (pc24 * 2654435761u) & (kMxHistoryCapacity - 1u);
    for (probe = 0u; probe < kMxHistoryCapacity; ++probe) {
        MxHistoryEntry *entry =
            &g_mx_history[(slot + probe) & (kMxHistoryCapacity - 1u)];
        if (entry->pc24 == pc24) return entry;
        if (entry->pc24 == 0u) {
            if (!create) return NULL;
            entry->pc24 = pc24;
            return entry;
        }
    }
    return NULL;
}

void sr_diagnostic_reset(void) {
    memset(g_mx_history, 0, sizeof(g_mx_history));
    memset(g_xtrace, 0, sizeof(g_xtrace));
    memset(g_oob_warning, 0, sizeof(g_oob_warning));
    memset(g_trap_warning, 0, sizeof(g_trap_warning));
    g_xtrace_index = 0u;
    g_oob_warning_count = 0u;
    g_trap_warning_count = 0u;
    g_trapfn_fired = false;
    g_strace_enabled = -1;
    g_strace_low = g_strace_high = 0u;
    g_strace_count = 0u;
}

unsigned sr_diagnostic_trap_warning_count(void) {
    return g_trap_warning_count;
}

void sr_mx_history_record(uint32 pc24, int m, int x) {
    unsigned combination = ((unsigned)(m & 1) << 1) | (unsigned)(x & 1);
    MxHistoryEntry *entry = mx_entry(pc24 & 0xffffffu, true);
    unsigned index;
    if (entry == NULL) return;
    if (!entry->warned && entry->count[combination] == 0u) {
        unsigned dominant = 0u;
        for (index = 1u; index < 4u; ++index) {
            if (entry->count[index] > entry->count[dominant]) dominant = index;
        }
        if (entry->count[dominant] >= kMxWarmupCount &&
            dominant != combination) {
            entry->warned = true;
            fprintf(stderr,
                    "[mxhist] $%06X changed from M%uX%u to M%uX%u at frame %d\n",
                    pc24 & 0xffffffu, dominant >> 1, dominant & 1u,
                    combination >> 1, combination & 1u, snes_frame_counter);
        }
    }
    ++entry->count[combination];
}

uint32 sr_mx_history_count(uint32 pc24, int m, int x) {
    MxHistoryEntry *entry = mx_entry(pc24 & 0xffffffu, false);
    unsigned combination = ((unsigned)(m & 1) << 1) | (unsigned)(x & 1);
    return entry != NULL ? entry->count[combination] : 0u;
}

void sr_mx_history_dump(void) {
    unsigned slot;
    if (!g_sr_mx_history_enabled) return;
    for (slot = 0u; slot < kMxHistoryCapacity; ++slot) {
        const MxHistoryEntry *entry = &g_mx_history[slot];
        if (entry->pc24 == 0u) continue;
        fprintf(stderr, "[mxhist] %06X %u %u %u %u\n", entry->pc24,
                entry->count[0], entry->count[1], entry->count[2],
                entry->count[3]);
    }
}

/* Returns true the first time a (site, detail) pair is seen. */
static bool trap_should_report(uint32 site_pc24, uint32 detail) {
    unsigned index;
    for (index = 0u; index < g_trap_warning_count; ++index) {
        if (g_trap_warning[index].first == (site_pc24 & 0xffffffu) &&
            g_trap_warning[index].second == detail) {
            return false;
        }
    }
    if (g_trap_warning_count >= kWarningCapacity) return false;
    g_trap_warning[g_trap_warning_count].first = site_pc24 & 0xffffffu;
    g_trap_warning[g_trap_warning_count].second = detail;
    ++g_trap_warning_count;
    return true;
}

static void print_cpu(const char *tag, CpuState *cpu, const char *name,
                      uint32 pc24) {
    fprintf(stderr,
            "[%s] %s $%06X A=%04X X=%04X Y=%04X S=%04X DB=%02X PB=%02X M=%u Xf=%u frame=%d\n",
            tag, name != NULL ? name : "?", pc24 & 0xffffffu, cpu->A,
            cpu->X, cpu->Y, cpu->S, cpu->DB, cpu->PB, cpu->m_flag & 1u,
            cpu->x_flag & 1u, snes_frame_counter);
}

void sr_entry_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                      const char *function_name, uint32 pc24) {
    print_cpu("entry-mx", cpu, function_name, pc24);
    fprintf(stderr, "[entry-mx] expected M%dX%d\n", expected_m, expected_x);
}

void sr_exit_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24) {
    print_cpu("exit-mx", cpu, function_name, pc24);
    fprintf(stderr, "[exit-mx] expected M%dX%d\n", expected_m, expected_x);
}

void sr_exit_s_fail(CpuState *cpu, uint32 entry_stack, uint32 return_stack,
                    const char *function_name, uint32 pc24) {
    print_cpu("exit-stack", cpu, function_name, pc24);
    fprintf(stderr, "[exit-stack] entry=%04X return=%04X delta=%d\n",
            (unsigned)entry_stack & 0xffffu,
            (unsigned)return_stack & 0xffffu,
            (int)((int32)return_stack - (int32)entry_stack));
}

void sr_call_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24) {
    print_cpu("call-mx", cpu, function_name, pc24);
    fprintf(stderr, "[call-mx] expected M%dX%d\n", expected_m, expected_x);
}

void sr_entry_trap_function(CpuState *cpu, const char *function_name, uint32 pc24) {
    if (g_trapfn_fired || g_sr_trap_function == NULL || function_name == NULL ||
        strstr(function_name, g_sr_trap_function) == NULL) return;
    g_trapfn_fired = true;
    print_cpu("trapfn", cpu, function_name, pc24);
    RecompStackDump();
}

void sr_garbage_variant_trap(CpuState *cpu, const char *function_name,
                             uint32 pc24) {
    static WarningKey warnings[kWarningCapacity];
    static unsigned warning_count;
    WarningKey key = {pc24 & 0xffffffu, string_hash(function_name)};
    unsigned index;
    if (sr_trace_active()) {
        sr_trace_garbage(pc24, function_name, cpu->m_flag & 1u,
                         cpu->x_flag & 1u);
    }
    if (getenv("SNESRECOMP_NO_GARBAGE_WARNING") != NULL) return;
    for (index = 0u; index < warning_count; ++index) {
        if (warnings[index].first == key.first &&
            warnings[index].second == key.second) return;
    }
    if (warning_count < kWarningCapacity) warnings[warning_count++] = key;
    print_cpu("garbage-variant", cpu, function_name, pc24);
    if (getenv("SNESRECOMP_GARBAGE_ABORT") != NULL) abort();
}

RecompReturn sr_dispatch_oob_warn(CpuState *cpu, uint32 site_pc24,
                                  uint16 index_value) {
    unsigned index;
    if (getenv("SNESRECOMP_NO_OOB_WARNING") != NULL) return RECOMP_RETURN_NORMAL;
    for (index = 0u; index < g_oob_warning_count; ++index) {
        if (g_oob_warning[index].first == (site_pc24 & 0xffffffu) &&
            g_oob_warning[index].second == index_value) {
            return RECOMP_RETURN_NORMAL;
        }
    }
    if (g_oob_warning_count < kWarningCapacity) {
        g_oob_warning[g_oob_warning_count].first = site_pc24 & 0xffffffu;
        g_oob_warning[g_oob_warning_count].second = index_value;
        ++g_oob_warning_count;
        print_cpu("dispatch-oob", cpu, g_last_recomp_func, site_pc24);
        fprintf(stderr,
                "[dispatch-oob] index=%u is outside the table declared for "
                "this site.\n"
                "[dispatch-oob] Check the COUNT and tables: base on the "
                "indirect_dispatch directive for $%06X.\n",
                index_value, site_pc24 & 0xffffffu);
    }
    return RECOMP_RETURN_NORMAL;
}

/*
 * An indirect jump the recompiler could not resolve to a target set. This is a
 * distinct condition from a genuine out-of-range table index, and it used to
 * be reported as one: the emitter passed a hardcoded 0xFFFF placeholder to the
 * out-of-bounds path, so the message read "index=65535" and invited a hunt for
 * a bad index that did not exist. Say what it actually is and name the
 * directive that fixes it.
 */
RecompReturn sr_unresolved_indirect_jump(CpuState *cpu, uint32 site_pc24) {
    if (trap_should_report(site_pc24, 0u)) {
        print_cpu("indirect-unresolved", cpu, g_last_recomp_func, site_pc24);
        fprintf(stderr,
                "[indirect-unresolved] $%06X is an indirect jump with no "
                "resolved target set.\n"
                "[indirect-unresolved] If the targets are a static table, "
                "declare it with indirect_dispatch or rts_dispatch. If they "
                "come from data the game interprets at run time, route the "
                "site with hle_dispatch and resolve it in the game layer.\n",
                site_pc24 & 0xffffffu);
    }
    return RECOMP_RETURN_NORMAL;
}

RecompReturn sr_unresolved_stub_warn(CpuState *cpu, uint32 target_pc24,
                                     const char *function_name) {
    if (trap_should_report(target_pc24, 1u)) {
        print_cpu("unresolved-stub", cpu, function_name, target_pc24);
        fprintf(stderr,
                "[unresolved-stub] called $%06X, which has no generated "
                "body.\n"
                "[unresolved-stub] A target below $8000 or past the image is "
                "outside the static LoROM code domain. It often means data "
                "was decoded as code, so check the caller's M/X widths and "
                "data_region coverage. If the game intentionally executes "
                "RAM or generated code, route that site through an HLE. A "
                "target inside data_region is intentionally not code; any "
                "other valid ROM target means its bank is absent from the "
                "cfg set.\n",
                target_pc24 & 0xffffffu);
    }
    return RECOMP_RETURN_NORMAL;
}

RecompReturn sr_unresolved_goto_warn(CpuState *cpu, uint32 source_pc24,
                                     uint32 target_pc24,
                                     const char *function_name,
                                     const char *target_label) {
    if (trap_should_report(source_pc24, 2u)) {
        print_cpu("unresolved-goto", cpu, function_name, source_pc24);
        fprintf(stderr,
                "[unresolved-goto] $%06X jumps to $%06X (%s), which is not a "
                "block in this function and has no entry of its own.\n"
                "[unresolved-goto] Declare the target with func, or bound the "
                "current function with end: so the jump becomes a tail "
                "call.\n",
                source_pc24 & 0xffffffu, target_pc24 & 0xffffffu,
                target_label != NULL ? target_label : "unknown");
    }
    return RECOMP_RETURN_NORMAL;
}

static int copy_block_history(uint32 *pc, uint32 *aux, uint16 *stack,
                              int maximum) {
    unsigned total;
    int count;
    int index;
    if (pc == NULL || maximum <= 0) return 0;
    if (maximum > kRuntimeBlockTraceRingCapacity) {
        maximum = kRuntimeBlockTraceRingCapacity;
    }
    total = g_sr_block_index;
    count = total < (unsigned)maximum ? (int)total : maximum;
    for (index = 0; index < count; ++index) {
        unsigned slot = (g_sr_block_index - (unsigned)count + (unsigned)index) &
                        kRuntimeBlockTraceRingMask;
        pc[index] = g_sr_block_ring[slot];
        if (aux != NULL) aux[index] = g_sr_block_aux[slot];
        if (stack != NULL) stack[index] = g_sr_block_stack[slot];
    }
    return count;
}

int sr_block_history_with_aux(uint32 *pc, uint32 *aux, int maximum) {
    return copy_block_history(pc, aux, NULL, maximum);
}

int sr_block_history_with_stack(uint32 *pc, uint32 *aux, uint16 *stack, int maximum) {
    return copy_block_history(pc, aux, stack, maximum);
}

int sr_x_transition_trace_enabled(void) { return getenv("SNESRECOMP_X_TRACE") != NULL; }

void sr_x_transition_trace_record(uint32 block, uint32 next, int new_x, int m,
                      uint32 game_frame) {
    XTraceEntry *entry = &g_xtrace[g_xtrace_index++ & (kXTraceCapacity - 1u)];
    entry->block_pc24 = block & 0xffffffu;
    entry->next_pc24 = next & 0xffffffu;
    entry->game_frame = game_frame;
    entry->new_x = (uint8)(new_x != 0);
    entry->m = (uint8)(m != 0);
}

unsigned sr_x_transition_trace_count(void) {
    return g_xtrace_index < kXTraceCapacity ? g_xtrace_index : kXTraceCapacity;
}

void sr_x_transition_trace_dump(const char *reason, int maximum) {
    int count;
    int index;
    if (maximum <= 0) return;
    count = (int)sr_x_transition_trace_count();
    if (count > maximum) count = maximum;
    fprintf(stderr, "[xtrace] %s\n", reason != NULL ? reason : "trace");
    for (index = count; index > 0; --index) {
        const XTraceEntry *entry =
            &g_xtrace[(g_xtrace_index - (unsigned)index) &
                      (kXTraceCapacity - 1u)];
        fprintf(stderr, "[xtrace] gf=%u X=%u M=%u %06X -> %06X\n",
                entry->game_frame, entry->new_x, entry->m,
                entry->block_pc24, entry->next_pc24);
    }
}

static bool strace_configure(void) {
    if (g_strace_enabled < 0) {
        const char *low = getenv("SNESRECOMP_STACK_TRACE_LOW");
        const char *high = getenv("SNESRECOMP_STACK_TRACE_HIGH");
        g_strace_enabled = getenv("SNESRECOMP_STACK_TRACE") != NULL;
        g_strace_low = low != NULL ? (uint32)strtoul(low, NULL, 16) :
            0x03b200u;
        g_strace_high = high != NULL ? (uint32)strtoul(high, NULL, 16) :
            0x03b260u;
    }
    return g_strace_enabled != 0;
}

int sr_stack_trace_active(void) {
    uint32 pc24;
    if (!strace_configure() || g_strace_count >= 400u ||
        g_sr_block_index == 0u) return false;
    pc24 = g_sr_block_ring[(g_sr_block_index - 1u) &
                         kRuntimeBlockTraceRingMask];
    return pc24 >= g_strace_low && pc24 <= g_strace_high;
}

void sr_stack_trace_operation(const char *kind, uint16 address, uint8 value, uint16 stack) {
    uint32 pc24;
    if (!sr_stack_trace_active()) return;
    pc24 = g_sr_block_ring[(g_sr_block_index - 1u) &
                         kRuntimeBlockTraceRingMask];
    fprintf(stderr, "[strace] %-5s $%04X=$%02X S=%04X pc=$%06X\n",
            kind != NULL ? kind : "op", address, value, stack,
            pc24 & 0xffffffu);
    if (++g_strace_count == 400u)
        fputs("[strace] trace limit reached (400 records)\n", stderr);
}

void sr_stack_trace_block(uint32 pc24, uint16 stack, int m, int x) {
    if (!strace_configure() || g_strace_count >= 400u ||
        pc24 < g_strace_low || pc24 > g_strace_high) return;
    fprintf(stderr, "[strace] BLOCK $%06X S=%04X M=%u X=%u\n",
            pc24 & 0xffffffu, stack, (unsigned)(m & 1),
            (unsigned)(x & 1));
    if (++g_strace_count == 400u)
        fputs("[strace] trace limit reached (400 records)\n", stderr);
}

int sr_stack_provenance_enabled(void) {
    if (getenv("SNESRECOMP_STACK_PROVENANCE") == NULL) return false;
    if (g_stack_pusher == NULL) {
        g_stack_pusher = (uint32 *)calloc(0x10000u, sizeof(uint32));
        g_stack_pusher_frame =
            (unsigned *)calloc(0x10000u, sizeof(unsigned));
    }
    return g_stack_pusher != NULL && g_stack_pusher_frame != NULL;
}

void sr_vram_trace_raw(uint16 address, uint8 value, int port) {
    if (getenv("SNESRECOMP_VRAM_RAW") == NULL) return;
    fprintf(stderr, "[vramraw] frame=%d port=%02X address=%04X value=%02X\n",
            snes_frame_counter, port & 0xff, address, value);
}

int sr_vram_watch(uint16 address, uint8 value) {
    if (getenv("SNESRECOMP_VRAM_WATCH") == NULL) return 0;
    fprintf(stderr, "[vramwatch] frame=%d address=%04X value=%02X\n",
            snes_frame_counter, address, value);
    return 0;
}
