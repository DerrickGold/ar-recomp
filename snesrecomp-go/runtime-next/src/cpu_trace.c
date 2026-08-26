#include "cpu_trace.h"

#include "common_cpu_infra.h"
#include "diagnostic.h"
#include "snes/snes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if SNESRECOMP_TRACE

enum {
    kDefaultTraceCapacity = 65536,
    kMinimumTraceCapacity = 1024,
    kMaximumTraceCapacity = 1 << 22,
    kBoundaryCapacity = 8192,
    kOffrailsCapacity = 64,
};

typedef struct BoundaryRecord {
    uint64 sequence;
    uint32 name_hash;
    int32 frame;
    uint16 stack;
    uint8 depth;
    uint8 entering;
    uint8 exit_kind;
} BoundaryRecord;

typedef struct OffrailsRecord {
    uint32 tag_hash;
    uint32 hint_group;
    uint32 first_hint;
    uint32 last_hint;
    uint64 hits;
} OffrailsRecord;

static SrCpuTraceRecord *g_trace;
static uint64 g_trace_capacity;
static uint64 g_trace_index;
static BoundaryRecord g_boundary[kBoundaryCapacity];
static uint64 g_boundary_index;
static uint8 g_next_exit_kind;
static int32 g_stack_drift_min_frame = -1;
static OffrailsRecord g_offrails[kOffrailsCapacity];
static unsigned g_offrails_count;

static uint32 hash_string(const char *text) {
    uint32 hash = 2166136261u;
    if (text == NULL) return 0u;
    while (*text != '\0') {
        hash ^= (uint8)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static uint64 next_power_of_two(uint64 value) {
    uint64 result = 1u;
    while (result < value && result < kMaximumTraceCapacity) result <<= 1;
    return result;
}

uint64 cpu_trace_init(void) {
    uint64 requested = kDefaultTraceCapacity;
    const char *setting = getenv("SNESRECOMP_CPU_TRACE_RING_ENTRIES");
    SrCpuTraceRecord *replacement;
    if (setting != NULL && *setting != '\0') {
        requested = (uint64)strtoull(setting, NULL, 0);
    }
    if (requested < kMinimumTraceCapacity) requested = kMinimumTraceCapacity;
    if (requested > kMaximumTraceCapacity) requested = kMaximumTraceCapacity;
    requested = next_power_of_two(requested);
    replacement = (SrCpuTraceRecord *)calloc((size_t)requested,
                                              sizeof(*replacement));
    if (replacement == NULL) return 0u;
    free(g_trace);
    g_trace = replacement;
    g_trace_capacity = requested;
    sr_cpu_trace_reset();
    return g_trace_capacity;
}

void sr_cpu_trace_reset(void) {
    if (g_trace != NULL) memset(g_trace, 0, (size_t)g_trace_capacity * sizeof(*g_trace));
    memset(g_boundary, 0, sizeof(g_boundary));
    memset(g_offrails, 0, sizeof(g_offrails));
    g_trace_index = 0u;
    g_boundary_index = 0u;
    g_next_exit_kind = BD_EXIT_KIND_NORMAL;
    g_offrails_count = 0u;
}

uint64 sr_cpu_trace_count(void) {
    return g_trace_index < g_trace_capacity ? g_trace_index : g_trace_capacity;
}

int sr_cpu_trace_copy(SrCpuTraceRecord *output, int maximum) {
    int count;
    int index;
    uint64 available = sr_cpu_trace_count();
    if (output == NULL || maximum <= 0 || g_trace_capacity == 0u) return 0;
    count = available < (uint64)maximum ? (int)available : maximum;
    for (index = 0; index < count; ++index) {
        uint64 sequence = g_trace_index - (uint64)count + (uint64)index;
        output[index] = g_trace[sequence & (g_trace_capacity - 1u)];
    }
    return count;
}

void cpu_trace_event(CpuState *cpu, uint32 pc24, uint8 event_type,
                     uint8 extra0, uint16 extra1) {
    SrCpuTraceRecord *record;
    uint64 sequence;
    if (cpu == NULL) return;
    if (g_trace == NULL && cpu_trace_init() == 0u) return;
    sequence = g_trace_index++;
    record = &g_trace[sequence & (g_trace_capacity - 1u)];
    record->sequence = sequence;
    record->pc24 = pc24 & 0xffffffu;
    record->frame = snes_frame_counter;
    record->A = cpu->A;
    record->X = cpu->X;
    record->Y = cpu->Y;
    record->S = cpu->S;
    record->D = cpu->D;
    record->extra1 = extra1;
    record->DB = cpu->DB;
    record->PB = cpu->PB;
    record->P = cpu->P;
    record->m = cpu->m_flag & 1u;
    record->x = cpu->x_flag & 1u;
    record->event_type = event_type;
    record->extra0 = extra0;
}

void cpu_trace_func_entry(CpuState *cpu, uint32 pc24, const char *name) {
    uint32 hash = hash_string(name);
    cpu_trace_event(cpu, pc24, CPU_TR_FUNC_ENTRY, (uint8)hash,
                    (uint16)(hash >> 8));
}

void cpu_trace_db_change(CpuState *cpu, uint32 pc24, uint8 old_db,
                         uint8 new_db, uint8 event_type) {
    cpu_trace_event(cpu, pc24, event_type, old_db, new_db);
}

void cpu_trace_pb_change(CpuState *cpu, uint32 pc24, uint8 old_pb,
                         uint8 new_pb, uint8 event_type) {
    cpu_trace_event(cpu, pc24, event_type, old_pb, new_pb);
}

void cpu_trace_px_record(CpuState *cpu, uint32 pc24, uint8 kind,
                         uint8 old_p, uint8 new_p) {
    cpu_trace_event(cpu, pc24, CPU_TR_PLP, kind,
                    (uint16)old_p | ((uint16)new_p << 8));
}

void cpu_trace_stack_op(CpuState *cpu, uint32 pc24, uint8 operation,
                        uint16 old_stack, int8 delta) {
    cpu_trace_event(cpu, pc24, CPU_TR_STACK_OP, operation,
                    (uint16)((uint8)delta << 8) | (old_stack & 0xffu));
}

uint64 boundary_audit_init(void) {
    memset(g_boundary, 0, sizeof(g_boundary));
    g_boundary_index = 0u;
    return kBoundaryCapacity;
}

static void boundary_record(const char *name, bool entering) {
    BoundaryRecord *record =
        &g_boundary[g_boundary_index & (kBoundaryCapacity - 1u)];
    record->sequence = g_boundary_index++;
    record->name_hash = hash_string(name);
    record->frame = snes_frame_counter;
    record->stack = g_cpu.S;
    record->depth = (uint8)(g_recomp_stack_top < 255 ? g_recomp_stack_top : 255);
    record->entering = entering;
    record->exit_kind = entering ? BD_EXIT_KIND_NORMAL : g_next_exit_kind;
    if (!entering) g_next_exit_kind = BD_EXIT_KIND_NORMAL;
}

void boundary_audit_record_entry(const char *name) {
    boundary_record(name, true);
}

void boundary_audit_record_exit(const char *name) {
    (void)g_stack_drift_min_frame;
    boundary_record(name, false);
}

void cpu_trace_mark_nlr_exit(uint8 kind) { g_next_exit_kind = kind; }
void cpu_trace_arm_stack_drift_tripwire(int32 frame_min) {
    g_stack_drift_min_frame = frame_min;
}

void cpu_trace_offrails(const char *tag, uint32 hint) {
    uint32 tag_hash = hash_string(tag);
    uint32 group = hint & 0xffff0000u;
    unsigned index;
    for (index = 0u; index < g_offrails_count; ++index) {
        OffrailsRecord *record = &g_offrails[index];
        if (record->tag_hash == tag_hash && record->hint_group == group) {
            record->last_hint = hint;
            ++record->hits;
            return;
        }
    }
    if (g_offrails_count >= kOffrailsCapacity) return;
    g_offrails[g_offrails_count].tag_hash = tag_hash;
    g_offrails[g_offrails_count].hint_group = group;
    g_offrails[g_offrails_count].first_hint = hint;
    g_offrails[g_offrails_count].last_hint = hint;
    g_offrails[g_offrails_count].hits = 1u;
    ++g_offrails_count;
    fprintf(stderr, "[offrails] %s $%06X frame=%d\n",
            tag != NULL ? tag : "?", hint & 0xffffffu, snes_frame_counter);
}

int cpu_trace_offrails_count(void) { return (int)g_offrails_count; }

RecompReturn cpu_trace_unresolved_goto_trap(
    CpuState *cpu, uint32 source_pc24, uint32 target_pc24,
    const char *function_name, const char *target_label) {
    cpu_trace_event(cpu, source_pc24, CPU_TR_NLR_DETECT,
                    (uint8)hash_string(function_name), (uint16)target_pc24);
    fprintf(stderr, "[unresolved-goto] %s $%06X -> %s/$%06X\n",
            function_name != NULL ? function_name : "?",
            source_pc24 & 0xffffffu,
            target_label != NULL ? target_label : "?",
            target_pc24 & 0xffffffu);
    return RECOMP_RETURN_NORMAL;
}

RecompReturn cpu_trace_unresolved_stub_trap(
    CpuState *cpu, uint32 target_pc24, const char *function_name) {
    cpu_trace_event(cpu, target_pc24, CPU_TR_NLR_DETECT,
                    (uint8)hash_string(function_name), 0u);
    fprintf(stderr, "[unresolved-stub] %s $%06X\n",
            function_name != NULL ? function_name : "?",
            target_pc24 & 0xffffffu);
    return RECOMP_RETURN_NORMAL;
}

RecompReturn cpu_trace_dispatch_oob(CpuState *cpu, uint32 site_pc24,
                                    uint16 index) {
    return ar_dispatch_oob_warn(cpu, site_pc24, index);
}

#else

void sr_cpu_trace_reset(void) {}
uint64 sr_cpu_trace_count(void) { return 0u; }
int sr_cpu_trace_copy(SrCpuTraceRecord *output, int maximum) {
    (void)output; (void)maximum; return 0;
}

#endif
