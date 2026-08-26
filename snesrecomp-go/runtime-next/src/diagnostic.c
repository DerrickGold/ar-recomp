#include "diagnostic.h"

#include "ar_trace.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"

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

int g_ar_mx_check;
int g_ar_mxhist;
const char *g_ar_trapfn;
int g_ar_exit_mx_check;
int g_ar_exit_s_check;
int g_ar_call_mx_check;
uint32 *g_stack_pusher;
unsigned *g_stack_pusher_frame;

static MxHistoryEntry g_mx_history[kMxHistoryCapacity];
static XTraceEntry g_xtrace[kXTraceCapacity];
static unsigned g_xtrace_index;
static WarningKey g_oob_warning[kWarningCapacity];
static unsigned g_oob_warning_count;
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

void ar_diagnostic_reset(void) {
    memset(g_mx_history, 0, sizeof(g_mx_history));
    memset(g_xtrace, 0, sizeof(g_xtrace));
    memset(g_oob_warning, 0, sizeof(g_oob_warning));
    g_xtrace_index = 0u;
    g_oob_warning_count = 0u;
    g_trapfn_fired = false;
    g_strace_enabled = -1;
    g_strace_low = g_strace_high = 0u;
    g_strace_count = 0u;
}

void ar_mxhist_record(uint32 pc24, int m, int x) {
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

uint32 ar_mxhist_count(uint32 pc24, int m, int x) {
    MxHistoryEntry *entry = mx_entry(pc24 & 0xffffffu, false);
    unsigned combination = ((unsigned)(m & 1) << 1) | (unsigned)(x & 1);
    return entry != NULL ? entry->count[combination] : 0u;
}

void ar_mxhist_dump(void) {
    unsigned slot;
    if (!g_ar_mxhist) return;
    for (slot = 0u; slot < kMxHistoryCapacity; ++slot) {
        const MxHistoryEntry *entry = &g_mx_history[slot];
        if (entry->pc24 == 0u) continue;
        fprintf(stderr, "[mxhist] %06X %u %u %u %u\n", entry->pc24,
                entry->count[0], entry->count[1], entry->count[2],
                entry->count[3]);
    }
}

static void print_cpu(const char *tag, CpuState *cpu, const char *name,
                      uint32 pc24) {
    fprintf(stderr,
            "[%s] %s $%06X A=%04X X=%04X Y=%04X S=%04X DB=%02X PB=%02X M=%u Xf=%u frame=%d\n",
            tag, name != NULL ? name : "?", pc24 & 0xffffffu, cpu->A,
            cpu->X, cpu->Y, cpu->S, cpu->DB, cpu->PB, cpu->m_flag & 1u,
            cpu->x_flag & 1u, snes_frame_counter);
}

void ar_entry_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                      const char *function_name, uint32 pc24) {
    print_cpu("entry-mx", cpu, function_name, pc24);
    fprintf(stderr, "[entry-mx] expected M%dX%d\n", expected_m, expected_x);
}

void ar_exit_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24) {
    print_cpu("exit-mx", cpu, function_name, pc24);
    fprintf(stderr, "[exit-mx] expected M%dX%d\n", expected_m, expected_x);
}

void ar_exit_s_fail(CpuState *cpu, uint32 entry_stack, uint32 return_stack,
                    const char *function_name, uint32 pc24) {
    print_cpu("exit-stack", cpu, function_name, pc24);
    fprintf(stderr, "[exit-stack] entry=%04X return=%04X delta=%d\n",
            (unsigned)entry_stack & 0xffffu,
            (unsigned)return_stack & 0xffffu,
            (int)((int32)return_stack - (int32)entry_stack));
}

void ar_call_mx_fail(CpuState *cpu, int expected_m, int expected_x,
                     const char *function_name, uint32 pc24) {
    print_cpu("call-mx", cpu, function_name, pc24);
    fprintf(stderr, "[call-mx] expected M%dX%d\n", expected_m, expected_x);
}

void ar_entry_trapfn(CpuState *cpu, const char *function_name, uint32 pc24) {
    if (g_trapfn_fired || g_ar_trapfn == NULL || function_name == NULL ||
        strstr(function_name, g_ar_trapfn) == NULL) return;
    g_trapfn_fired = true;
    print_cpu("trapfn", cpu, function_name, pc24);
    RecompStackDump();
}

void ar_garbage_variant_trap(CpuState *cpu, const char *function_name,
                             uint32 pc24) {
    static WarningKey warnings[kWarningCapacity];
    static unsigned warning_count;
    WarningKey key = {pc24 & 0xffffffu, string_hash(function_name)};
    unsigned index;
    if (ar_trace_active()) {
        ar_trace_garbage(pc24, function_name, cpu->m_flag & 1u,
                         cpu->x_flag & 1u);
    }
    if (getenv("AR_NOGARBAGEWARN") != NULL) return;
    for (index = 0u; index < warning_count; ++index) {
        if (warnings[index].first == key.first &&
            warnings[index].second == key.second) return;
    }
    if (warning_count < kWarningCapacity) warnings[warning_count++] = key;
    print_cpu("garbage-variant", cpu, function_name, pc24);
    if (getenv("AR_GARBAGE_ABORT") != NULL) abort();
}

RecompReturn ar_dispatch_oob_warn(CpuState *cpu, uint32 site_pc24,
                                  uint16 index_value) {
    unsigned index;
    if (getenv("AR_NOOOBWARN") != NULL) return RECOMP_RETURN_NORMAL;
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
        fprintf(stderr, "[dispatch-oob] index=%u\n", index_value);
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
    total = g_ar_blk_idx;
    count = total < (unsigned)maximum ? (int)total : maximum;
    for (index = 0; index < count; ++index) {
        unsigned slot = (g_ar_blk_idx - (unsigned)count + (unsigned)index) &
                        kRuntimeBlockTraceRingMask;
        pc[index] = g_ar_blk_ring[slot];
        if (aux != NULL) aux[index] = g_ar_blk_aux[slot];
        if (stack != NULL) stack[index] = g_ar_blk_s[slot];
    }
    return count;
}

int ar_block_history2(uint32 *pc, uint32 *aux, int maximum) {
    return copy_block_history(pc, aux, NULL, maximum);
}

int ar_block_history3(uint32 *pc, uint32 *aux, uint16 *stack, int maximum) {
    return copy_block_history(pc, aux, stack, maximum);
}

int ar_xtrace_enabled(void) { return getenv("AR_XTRACE") != NULL; }

void ar_xtrace_record(uint32 block, uint32 next, int new_x, int m,
                      uint32 game_frame) {
    XTraceEntry *entry = &g_xtrace[g_xtrace_index++ & (kXTraceCapacity - 1u)];
    entry->block_pc24 = block & 0xffffffu;
    entry->next_pc24 = next & 0xffffffu;
    entry->game_frame = game_frame;
    entry->new_x = (uint8)(new_x != 0);
    entry->m = (uint8)(m != 0);
}

unsigned ar_xtrace_count(void) {
    return g_xtrace_index < kXTraceCapacity ? g_xtrace_index : kXTraceCapacity;
}

void ar_xtrace_dump(const char *reason, int maximum) {
    int count;
    int index;
    if (maximum <= 0) return;
    count = (int)ar_xtrace_count();
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
        const char *low = getenv("AR_STRACE_LO");
        const char *high = getenv("AR_STRACE_HI");
        g_strace_enabled = getenv("AR_STRACE") != NULL;
        g_strace_low = low != NULL ? (uint32)strtoul(low, NULL, 16) :
            0x03b200u;
        g_strace_high = high != NULL ? (uint32)strtoul(high, NULL, 16) :
            0x03b260u;
    }
    return g_strace_enabled != 0;
}

int ar_strace_active(void) {
    uint32 pc24;
    if (!strace_configure() || g_strace_count >= 400u ||
        g_ar_blk_idx == 0u) return false;
    pc24 = g_ar_blk_ring[(g_ar_blk_idx - 1u) &
                         kRuntimeBlockTraceRingMask];
    return pc24 >= g_strace_low && pc24 <= g_strace_high;
}

void ar_strace_op(const char *kind, uint16 address, uint8 value, uint16 stack) {
    uint32 pc24;
    if (!ar_strace_active()) return;
    pc24 = g_ar_blk_ring[(g_ar_blk_idx - 1u) &
                         kRuntimeBlockTraceRingMask];
    fprintf(stderr, "[strace] %-5s $%04X=$%02X S=%04X pc=$%06X\n",
            kind != NULL ? kind : "op", address, value, stack,
            pc24 & 0xffffffu);
    if (++g_strace_count == 400u)
        fputs("[strace] trace limit reached (400 records)\n", stderr);
}

void ar_strace_block(uint32 pc24, uint16 stack, int m, int x) {
    if (!strace_configure() || g_strace_count >= 400u ||
        pc24 < g_strace_low || pc24 > g_strace_high) return;
    fprintf(stderr, "[strace] BLOCK $%06X S=%04X M=%u X=%u\n",
            pc24 & 0xffffffu, stack, (unsigned)(m & 1),
            (unsigned)(x & 1));
    if (++g_strace_count == 400u)
        fputs("[strace] trace limit reached (400 records)\n", stderr);
}

int ar_stackprov_enabled(void) {
    if (getenv("AR_STACKPROV") == NULL) return false;
    if (g_stack_pusher == NULL) {
        g_stack_pusher = (uint32 *)calloc(0x10000u, sizeof(uint32));
        g_stack_pusher_frame =
            (unsigned *)calloc(0x10000u, sizeof(unsigned));
    }
    return g_stack_pusher != NULL && g_stack_pusher_frame != NULL;
}

void ar_vramraw(uint16 address, uint8 value, int port) {
    if (getenv("AR_VRAMRAW") == NULL) return;
    fprintf(stderr, "[vramraw] frame=%d port=%02X address=%04X value=%02X\n",
            snes_frame_counter, port & 0xff, address, value);
}

int ar_vramwatch(uint16 address, uint8 value) {
    if (getenv("AR_VRAMWATCH") == NULL) return 0;
    fprintf(stderr, "[vramwatch] frame=%d address=%04X value=%02X\n",
            snes_frame_counter, address, value);
    return 0;
}
