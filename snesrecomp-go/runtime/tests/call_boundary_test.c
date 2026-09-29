#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/trace.h"

#include <stdio.h>
#include <string.h>

/* Compare the outlined helpers with the former inline call envelope. Record
 * memory/probe order and the state visible to each probe, not just final CPU
 * state. Existing native emitter contracts exercise the real unwind engine;
 * this isolated test controls whether that engine accepts a return. */
CpuReturnScope *g_cpu_return_scope;
CpuReturnScope *g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
int g_sr_call_mx_check_enabled = 1;

typedef struct Observation {
    unsigned kind;
    uint32 pc;
    unsigned a, b;
    uint16 stack;
    uint8 bank, paired;
    int depth, active_scope;
} Observation;

typedef struct Snapshot {
    CpuState entered, left;
    uint32 continuation;
    uint16 entry_stack, caller_limit, call_stack;
    uint8 frame_bytes, adjusted, saved_pb;
    int result, parent_restored, owned_cleared, depth;
    unsigned observations;
    Observation events[16];
} Snapshot;

static Snapshot current;
static CpuState *active_cpu;
static CpuReturnScope *active_scope;
static int accept_unwind;
static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "call boundary: %s\n", message);
        ++failures;
    }
}

static void observe(unsigned kind, uint32 pc, unsigned a, unsigned b) {
    Observation *event;
    if (current.observations >= countof(current.events)) {
        check(0, "observation overflow");
        return;
    }
    event = &current.events[current.observations++];
    event->kind = kind; event->pc = pc; event->a = a; event->b = b;
    event->stack = active_cpu->S;
    event->bank = active_cpu->PB;
    event->paired = active_cpu->host_return_valid;
    event->depth = g_recomp_stack_top;
    event->active_scope = g_cpu_return_scope == active_scope;
}

void cpu_write8(CpuState *cpu, uint8 bank, uint16 address, uint8 value) {
    check(cpu == active_cpu && bank == 0, "hardware frame writes use bank zero");
    observe(1, address, value, bank);
}

void sr_call_mx_fail(CpuState *cpu, int m, int x, const char *name, uint32 pc) {
    check(cpu == active_cpu && strcmp(name, "Caller") == 0, "M/X probe identity");
    observe(2, pc, (unsigned)m, (unsigned)x);
}

int sr_trace_active(void) { return 1; }
void sr_trace_call(uint32 pc, const char *name, int m, int x, int em, int ex) {
    check(strcmp(name, "Caller") == 0, "recorder caller identity");
    observe(3, pc, (unsigned)(m * 2 + x), (unsigned)(em * 2 + ex));
}

#if SNESRECOMP_TRACE
void cpu_trace_pb_change(CpuState *cpu, uint32 pc, uint8 old_pb,
                         uint8 new_pb, uint8 type) {
    check(cpu == active_cpu && old_pb == cpu->PB, "PB probe precedes mutation");
    observe(type == CPU_TR_JSL ? 4u : 5u, pc, old_pb, new_pb);
}
void cpu_trace_event(CpuState *cpu, uint32 pc, uint8 type, uint8 a, uint16 b) {
    check(cpu == active_cpu && type == CPU_TR_NLR_PROPAGATE, "NLR event type");
    observe(6, pc, a, b);
}
void cpu_trace_mark_nlr_exit(uint8 kind) { observe(7, 0, kind, 0); }
#endif

int cpu_finish_owned_unwind(CpuReturnScope *scope, CpuState *cpu) {
    check(scope == active_scope && cpu == active_cpu, "unwind uses live caller scope");
    observe(8, 0, 0, 0);
    if (!accept_unwind) return 0;
    scope->adjusted_return = 1;
    g_cpu_owned_unwind_scope = NULL;
    return 1;
}

/* Frozen reference: order and restoration policy of emitReturnFramePush +
 * emitCall + finishCall before outlining. No production helper calls here. */
static uint16 reference_enter(CpuState *cpu, CpuReturnScope *scope,
        uint8 *saved_pb, int long_call, uint16 ret, uint16 continuation,
        uint16 entry_stack, int m, int x) {
    uint16 call_stack = cpu->S;
    if (long_call) {
        cpu_write8(cpu, 0, cpu->S, cpu->PB);
        cpu->S = (uint16)(cpu->S - 1);
    }
    cpu_write8(cpu, 0, cpu->S, (uint8)(ret >> 8));
    cpu->S = (uint16)(cpu->S - 1);
    cpu_write8(cpu, 0, cpu->S, (uint8)ret);
    cpu->S = (uint16)(cpu->S - 1);
    cpu->host_return_valid = 1;
    cpu_return_scope_begin(scope, cpu, ((uint32)cpu->PB << 16) | continuation,
                           entry_stack, long_call ? 3u : 2u);
    if (long_call) {
        *saved_pb = cpu->PB;
        cpu_trace_pb_change(cpu, 0, *saved_pb, 0x12, CPU_TR_JSL);
        cpu->PB = 0x12;
    }
    sr_call_mx_check(cpu, m, x, "Caller", 0x808123);
    return call_stack;
}

static int reference_leave(CpuState *cpu, CpuReturnScope *scope,
        RecompReturn result, uint16 call_stack, int long_call, uint8 saved_pb) {
    if (result == RECOMP_RETURN_PARKED_WAIT) {
        cpu_return_scope_end(scope);
        return result;
    }
    if (result == RECOMP_RETURN_OWNED_UNWIND) {
        if (!cpu_finish_owned_unwind(scope, cpu)) {
            cpu_return_scope_end(scope);
            return result;
        }
        result = RECOMP_RETURN_NORMAL;
    }
    if (long_call) {
        cpu_trace_pb_change(cpu, 0, cpu->PB, saved_pb, CPU_TR_RTL);
        cpu->PB = saved_pb;
    }
    cpu_return_scope_end(scope);
    if (result != RECOMP_RETURN_NORMAL) {
        cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)result, 0);
        cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);
        return result == RECOMP_RETURN_TAILCALL ? (int)result : (int)result - 1;
    }
    if (!scope->adjusted_return) cpu->S = call_stack;
    return -1;
}

static Snapshot run_case(int outlined, int long_call, unsigned mx,
        uint16 stack, int wrap_pc, RecompReturn result, int adjusted,
        int accept, int invalidated) {
    CpuState cpu;
    CpuReturnScope parent, owner;
    uint8 saved_pb = 0;
    uint16 ret = wrap_pc ? 0xffff : 0x8125;
    uint16 continuation = wrap_pc ? 0 : (uint16)(ret + 1);
    uint16 call_stack;
    int next;
    memset(&current, 0, sizeof(current));
    memset(&cpu, 0, sizeof(cpu));
    memset(&parent, 0, sizeof(parent));
    memset(&owner, 0, sizeof(owner));
    cpu.S = stack; cpu.PB = 0x80; cpu.DB = 0x7e;
    cpu.A = 0x1234; cpu.X = 0x5678; cpu.Y = 0x9abc;
    cpu.m_flag = (uint8)(mx >> 1); cpu.x_flag = (uint8)(mx & 1);
    active_cpu = &cpu; active_scope = &owner;
    accept_unwind = accept;
    g_recomp_stack_top = 3;
    parent.cpu = &cpu;
    parent.caller_stack_limit = 0x1fff;
    parent.reset_activation_depth = g_recomp_stack_top;
    g_cpu_return_scope = &parent; g_cpu_owned_unwind_scope = NULL;
    /* Deliberately mismatch one width to exercise both diagnostic probes. */
    if (outlined) {
        call_stack = long_call ?
            sr_call_enter_long(&cpu, &owner, &saved_pb, 0x12, ret,
                continuation, 0x1fe0, cpu.m_flag ^ 1, cpu.x_flag, "Caller", 0x808123) :
            sr_call_enter(&cpu, &owner, ret, continuation, 0x1fe0,
                cpu.m_flag ^ 1, cpu.x_flag, "Caller", 0x808123);
    } else {
        call_stack = reference_enter(&cpu, &owner, &saved_pb, long_call, ret,
                continuation, 0x1fe0, cpu.m_flag ^ 1, cpu.x_flag);
    }
    current.entered = cpu; current.call_stack = call_stack;
    current.continuation = owner.continuation;
    current.entry_stack = owner.entry_stack;
    current.caller_limit = owner.caller_stack_limit;
    current.frame_bytes = owner.frame_bytes; current.saved_pb = saved_pb;
    check(g_cpu_return_scope == &owner && owner.previous == &parent &&
          owner.cpu == &cpu, "enter installs caller-owned scope");
    check(owner.continuation == (0x800000u | continuation), "continuation uses live caller PB");
    check(call_stack == stack && owner.entry_stack == (uint16)(stack - (long_call ? 3 : 2)),
          "frame size and stack wrap");
    /* Simulated callee result: live S/PB must survive parked/outer unwinds,
     * and an adjusted ordinary return must retain the callee's cleanup. */
    cpu.S = 0x1ff8; cpu.PB = 0x34; cpu.host_return_valid = 0;
    owner.adjusted_return = (uint8)adjusted;
    g_cpu_owned_unwind_scope = &owner;
    if (invalidated) g_cpu_return_scope = NULL;
    if (outlined) {
        next = long_call ? sr_call_leave_long(&cpu, &owner, result, call_stack, saved_pb)
                         : sr_call_leave(&cpu, &owner, result, call_stack);
    } else {
        next = reference_leave(&cpu, &owner, result, call_stack, long_call, saved_pb);
    }
    check(g_recomp_stack_top == 3, "helpers must not pop the activation");
    if (next >= 0) { observe(9, 0, (unsigned)next, 0); --g_recomp_stack_top; }
    current.left = cpu; current.result = next;
    current.adjusted = owner.adjusted_return;
    current.parent_restored = g_cpu_return_scope == (invalidated ? NULL : &parent);
    current.owned_cleared = g_cpu_owned_unwind_scope == NULL;
    current.depth = g_recomp_stack_top;
    check(current.parent_restored && current.owned_cleared, "scope teardown, including invalidated chain");
    if (result == RECOMP_RETURN_PARKED_WAIT || (result == RECOMP_RETURN_OWNED_UNWIND && !accept)) {
        check(next == (int)result && cpu.S == 0x1ff8 && cpu.PB == 0x34,
              "parked or outer unwind must retain native state");
    }
    g_cpu_return_scope = NULL; g_cpu_owned_unwind_scope = NULL;
    active_cpu = NULL; active_scope = NULL;
    return current;
}

int main(void) {
    const RecompReturn results[] = { RECOMP_RETURN_NORMAL, RECOMP_RETURN_SKIP_1,
        RECOMP_RETURN_SKIP_2, RECOMP_RETURN_SKIP_3, RECOMP_RETURN_TAILCALL,
        RECOMP_RETURN_PARKED_WAIT, RECOMP_RETURN_OWNED_UNWIND };
    const uint16 stacks[] = {0x1fff, 0x1ff, 1, 0};
    unsigned mx, r, s, flags;
    int long_call;
    for (long_call = 0; long_call <= 1; ++long_call)
        for (mx = 0; mx < 4; ++mx)
            for (r = 0; r < countof(results); ++r)
                for (s = 0; s < countof(stacks); ++s)
                    for (flags = 0; flags < 16; ++flags) {
                        Snapshot before = run_case(0, long_call, mx, stacks[s], flags & 1,
                            results[r], !!(flags & 2), !!(flags & 4), !!(flags & 8));
                        Snapshot after = run_case(1, long_call, mx, stacks[s], flags & 1,
                            results[r], !!(flags & 2), !!(flags & 4), !!(flags & 8));
                        check(memcmp(&before, &after, sizeof(before)) == 0,
                              "outlined envelope differs from inline reference");
                    }
    if (failures) return 1;
    puts("call boundary: inline/outlined state, stack, scope and probe order PASS");
    return 0;
}
