#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/trace.h"
#include <stdio.h>
#include <string.h>

/* Frozen generated RTS/RTL versus outlined helper. These controlled predicates
 * test ordering/activation visibility; native emitter tests cover the real
 * ownership predicates and execution through multiple compiled functions. */
CpuReturnScope *g_cpu_return_scope;
int g_recomp_stack_top;
int g_sr_exit_mx_check_enabled = 1;
int g_sr_exit_stack_check_enabled = 1;
typedef struct Event {
    unsigned kind, a, b, c;
    uint16 stack;
    uint8 bank;
    int depth;
} Event;
typedef struct Snapshot {
    CpuState cpu;
    RecompReturn result;
    unsigned count, pops;
    Event events[24];
} Snapshot;
static Snapshot current;
static CpuState *active;
static uint8 memory[65536];
static int acceptance, ancestor, registered, failures;
static RecompReturn dispatch_result;

static void check(int yes, const char *message) {
    if (!yes) { fprintf(stderr, "return boundary: %s\n", message); ++failures; }
}
static void observe(unsigned kind, unsigned a, unsigned b, unsigned c) {
    Event *e;
    if (current.count >= countof(current.events)) { check(0, "event overflow"); return; }
    e = &current.events[current.count++];
    e->kind = kind; e->a = a; e->b = b; e->c = c;
    e->stack = active->S; e->bank = active->PB; e->depth = g_recomp_stack_top;
}
uint8 cpu_read8(CpuState *cpu, uint8 bank, uint16 address) {
    check(cpu == active && bank == 0, "return reads bank zero");
    observe(1, bank, address, memory[address]); return memory[address];
}
void RecompStackPop(void) { observe(2, 0, 0, 0); ++current.pops; --g_recomp_stack_top; }
int cpu_begin_owned_unwind(CpuState *cpu, uint16 stack, uint32 target, uint8 bytes) {
    check(cpu == active, "owned CPU"); observe(3, stack, target, bytes); return acceptance == 1;
}
int cpu_accept_adjusted_return(CpuState *cpu, uint16 entry, uint16 stack, uint32 target, uint8 bytes) {
    check(cpu == active, "adjusted CPU"); observe(4, entry, stack, target); observe(5, bytes, 0, 0);
    return acceptance == 2;
}
int cpu_accept_stacked_result_return(CpuState *cpu, uint16 entry, uint16 stack, uint32 target, uint8 bytes) {
    check(cpu == active, "stacked CPU"); observe(6, entry, stack, target); observe(7, bytes, 0, 0);
    return acceptance == 3;
}
int cpu_accept_return_word_relocation(CpuState *cpu, uint16 entry, uint16 stack, uint32 target, const CpuReturnScope *origin) {
    check(cpu == active, "word CPU"); observe(8, entry, stack, target); observe(9, origin != NULL, 0, 0);
    return acceptance == 4;
}
int cpu_resolve_ancestor_skip(uint16 stack) {
    check(g_recomp_stack_top == 3, "ancestor resolution must precede pop");
    observe(10, stack, 0, 0); return ancestor;
}
int cpu_dispatch_has_entry(CpuState *cpu, uint32 target) {
    check(cpu == active, "lookup CPU"); observe(11, target, 0, 0); return registered;
}
void cpu_tailcall_request(uint32 target, uint16 stack, uint32 source) { observe(12, target, stack, source); }
RecompReturn cpu_dispatch_pc_from(CpuState *cpu, uint32 target, uint16 stack, uint32 source) {
    check(cpu == active && g_recomp_stack_top == 2, "dispatch must follow pop");
    observe(13, target, stack, source); return dispatch_result;
}
RecompReturn cpu_dispatch_paired_tail_from(CpuState *cpu, uint32 target, uint16 stack, uint8 hrv, uint32 source) {
    check(cpu == active && g_recomp_stack_top == 2 && hrv == 1, "paired dispatch after pop");
    observe(14, target, stack, source); return dispatch_result;
}
void sr_exit_mx_fail(CpuState *cpu, int m, int x, const char *name, uint32 source) {
    check(cpu == active && strcmp(name, "Returner") == 0, "M/X identity"); observe(15, (unsigned)m, (unsigned)x, source);
}
void sr_exit_s_fail(CpuState *cpu, uint32 entry, uint32 stack, const char *name, uint32 source) {
    check(cpu == active && strcmp(name, "Returner") == 0, "stack identity"); observe(16, entry, stack, source);
}
void cpu_trace_resolved_dispatch(CpuState *cpu, uint32 target, uint32 source) {
    check(cpu == active, "semantic CPU"); observe(17, target, source, 0);
}
#if SNESRECOMP_TRACE
void dbg_rts_trace(CpuState *cpu, uint32 source, uint16 entry, uint16 stack, uint32 target, uint8 hrv) {
    check(cpu == active, "trace CPU"); observe(18, source, entry, stack); observe(19, target, hrv, 0);
}
void cpu_trace_missing_pushed_target(CpuState *cpu, uint32 target, uint32 source) {
    check(cpu == active, "pushed CPU"); observe(20, target, source, 0);
}
void cpu_trace_mark_nlr_exit(uint8 kind) { observe(21, kind, 0, 0); }
#endif

/* Deliberately retain the former sequential branches, including the second
 * ancestor query and the pop BEFORE evaluating a dispatch return expression. */
static RecompReturn reference(CpuState *cpu, uint16 entry, uint8 hrv,
        uint32 source, int mx, const CpuReturnScope *origin, unsigned flags) {
    uint16 ret = cpu->S, lo, hi, miss;
    uint8 bank, bytes = (flags & SR_RETURN_LONG) ? 3u : 2u;
    uint32 target;
    cpu->S = (uint16)(cpu->S + 1); lo = cpu_read8(cpu, 0, cpu->S);
    cpu->S = (uint16)(cpu->S + 1); hi = cpu_read8(cpu, 0, cpu->S);
    if (flags & SR_RETURN_LONG) {
        cpu->S = (uint16)(cpu->S + 1); bank = cpu_read8(cpu, 0, cpu->S);
    } else bank = cpu->PB;
    target = ((uint32)bank << 16) | ((((hi << 8) | lo) + 1u) & 0xffffu);
    if (flags & SR_RETURN_TRACE_EDGE) cpu_trace_resolved_dispatch(cpu, target, source);
#if SNESRECOMP_TRACE
    dbg_rts_trace(cpu, source, entry, ret, target, hrv);
#endif
    if (g_cpu_return_scope && ret > g_cpu_return_scope->entry_stack && cpu_begin_owned_unwind(cpu, ret, target, bytes)) {
        RecompStackPop(); return RECOMP_RETURN_OWNED_UNWIND;
    }
    if (hrv && ret == entry) {
        if (mx >= 0) sr_exit_mx_check(cpu, mx >> 1, mx & 1, "Returner", source);
        RecompStackPop(); return RECOMP_RETURN_NORMAL;
    }
    if (hrv && cpu_accept_adjusted_return(cpu, entry, ret, target, bytes)) {
        if (mx >= 0) sr_exit_mx_check(cpu, mx >> 1, mx & 1, "Returner", source);
        RecompStackPop(); return RECOMP_RETURN_NORMAL;
    }
    if (hrv && cpu_accept_stacked_result_return(cpu, entry, ret, target, bytes)) {
        if (mx >= 0) sr_exit_mx_check(cpu, mx >> 1, mx & 1, "Returner", source);
        RecompStackPop(); return RECOMP_RETURN_NORMAL;
    }
    if ((flags & SR_RETURN_OWN_FRAME_WORD) && !(flags & SR_RETURN_LONG) && hrv && cpu_accept_return_word_relocation(cpu, entry, ret, target, origin)) {
        if (mx >= 0) sr_exit_mx_check(cpu, mx >> 1, mx & 1, "Returner", source);
        RecompStackPop(); return RECOMP_RETURN_NORMAL;
    }
    if (hrv && !cpu->emulation && ret < entry && cpu->S <= entry) {
#if SNESRECOMP_TRACE
        cpu_trace_missing_pushed_target(cpu, target, source);
#endif
        cpu->PB = bank; RecompStackPop();
        return cpu_dispatch_paired_tail_from(cpu, target, entry, hrv, source);
    }
    if (ret != entry && cpu_resolve_ancestor_skip(ret) >= 0) {
        cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);
        if (cpu_dispatch_has_entry(cpu, target)) {
            cpu_tailcall_request(target, (uint16)(ret + bytes), source);
            RecompStackPop(); return RECOMP_RETURN_TAILCALL;
        }
        { int skip = cpu_resolve_ancestor_skip(ret); RecompStackPop(); return (RecompReturn)skip; }
    }
    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);
    miss = (uint16)((ret > entry ? ret : entry) + bytes);
    sr_exit_s_check(cpu, entry, ret, "Returner", source);
    if (!hrv) {
        cpu_tailcall_request(target, miss, source); RecompStackPop(); return RECOMP_RETURN_TAILCALL;
    }
    RecompStackPop(); return cpu_dispatch_pc_from(cpu, target, miss, source);
}

static Snapshot run(int outlined, uint16 stack, int displacement, unsigned flags,
        unsigned state, int accept, int skip, int mx) {
    CpuState cpu;
    CpuReturnScope owner;
    uint16 entry = (uint16)(stack + displacement);
    uint16 word = (state & 16) ? 0xffff : 0x8123;
    uint8 hrv = (uint8)(state & 1);
    memset(&current, 0, sizeof(current)); memset(&cpu, 0, sizeof(cpu)); memset(&owner, 0, sizeof(owner));
    cpu.S = stack; cpu.PB = 0x80; cpu.DB = 0x7e;
    cpu.A = 0x1234; cpu.X = 0x5678; cpu.Y = 0x9abc;
    cpu.emulation = (uint8)((state >> 1) & 1); cpu.m_flag = (uint8)((state >> 2) & 1);
    cpu.x_flag = (uint8)((state >> 3) & 1); cpu.host_return_valid = hrv;
    active = &cpu; acceptance = accept; ancestor = skip; registered = (state & 16) != 0;
    dispatch_result = (state & 8) ? RECOMP_RETURN_OWNED_UNWIND : RECOMP_RETURN_PARKED_WAIT;
    g_recomp_stack_top = 3; owner.entry_stack = stack ? (uint16)(stack - 1) : 0;
    g_cpu_return_scope = (state & 4) ? &owner : NULL;
    memory[(uint16)(stack + 1)] = (uint8)word;
    memory[(uint16)(stack + 2)] = (uint8)(word >> 8);
    memory[(uint16)(stack + 3)] = 0x9d;
    current.result = outlined ? sr_return_native(&cpu, entry, hrv, 0x808765, "Returner", mx, g_cpu_return_scope, flags)
                             : reference(&cpu, entry, hrv, 0x808765, mx, g_cpu_return_scope, flags);
    current.cpu = cpu;
    check(current.pops == 1 && g_recomp_stack_top == 2, "exactly one activation pop");
    g_cpu_return_scope = NULL; active = NULL;
    return current;
}
int main(void) {
    const uint16 stacks[] = {0x1ff0, 0, 0xfffe};
    const int shifts[] = {-4, 0, 1, 4};
    const int skips[] = {-1, 0, 1, 3};
    unsigned s, d, flags, state, a, k;
    int mx;
    for (s = 0; s < countof(stacks); ++s)
        for (d = 0; d < countof(shifts); ++d)
            for (flags = 0; flags < 8; ++flags)
                for (state = 0; state < 32; ++state)
                    for (a = 0; a < 5; ++a)
                        for (k = 0; k < countof(skips); ++k)
                            for (mx = -1; mx < 4; ++mx) {
                                Snapshot before = run(0, stacks[s], shifts[d], flags, state, (int)a, skips[k], mx);
                                Snapshot after = run(1, stacks[s], shifts[d], flags, state, (int)a, skips[k], mx);
                                if (memcmp(&before, &after, sizeof(before)) != 0) {
                                    fprintf(stderr, "case s=%u d=%u flags=%u state=%u a=%u k=%u mx=%d\n", s, d, flags, state, a, k, mx);
                                    check(0, "CPU/event sequence mismatch"); return 1;
                                }
                            }
    return failures ? 1 : 0;
}
