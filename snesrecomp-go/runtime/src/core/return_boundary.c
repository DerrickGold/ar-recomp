#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "snesrecomp/game/trace.h"

/* Preserve the former inline epilogue's operation/probe order. This helper
 * owns exactly one generated activation pop, not a new activation. In
 * particular, never move the pop before ancestor queries or after dispatch. */
NOINLINE RecompReturn sr_return_native(CpuState *cpu, uint16 entry_stack,
        uint8 hrv, uint32 source, const char *name, int exit_mx,
        const CpuReturnScope *return_origin, unsigned flags) {
    uint16 ret_stack = cpu->S;
    uint16 low, high, miss_stack;
    uint8 bank, frame_bytes = (flags & SR_RETURN_LONG) ? 3u : 2u;
    uint32 target;
    cpu->S = (uint16)(cpu->S + 1);
    low = cpu_read8(cpu, 0, cpu->S);
    cpu->S = (uint16)(cpu->S + 1);
    high = cpu_read8(cpu, 0, cpu->S);
    if (flags & SR_RETURN_LONG) {
        cpu->S = (uint16)(cpu->S + 1);
        bank = cpu_read8(cpu, 0, cpu->S);
    } else {
        bank = cpu->PB;
    }
    target = ((uint32)bank << 16) | (uint16)(((high << 8) | low) + 1);
    if (flags & SR_RETURN_TRACE_EDGE)
        cpu_trace_resolved_dispatch(cpu, target, source);
#if SNESRECOMP_TRACE
    dbg_rts_trace(cpu, source, entry_stack, ret_stack, target, hrv);
#endif
    if (g_cpu_return_scope && ret_stack > g_cpu_return_scope->entry_stack &&
        cpu_begin_owned_unwind(cpu, ret_stack, target, frame_bytes)) {
        RecompStackPop();
        return RECOMP_RETURN_OWNED_UNWIND;
    }
    if ((hrv && ret_stack == entry_stack) ||
        (hrv && cpu_accept_adjusted_return(cpu, entry_stack, ret_stack,
                                           target, frame_bytes)) ||
        (hrv && cpu_accept_stacked_result_return(cpu, entry_stack, ret_stack,
                                                 target, frame_bytes)) ||
        ((flags & SR_RETURN_OWN_FRAME_WORD) && !(flags & SR_RETURN_LONG) && hrv &&
         cpu_accept_return_word_relocation(cpu, entry_stack, ret_stack,
                                           target, return_origin))) {
        if (exit_mx >= 0)
            sr_exit_mx_check(cpu, (exit_mx >> 1) & 1, exit_mx & 1, name, source);
        RecompStackPop();
        return RECOMP_RETURN_NORMAL;
    }
    if (hrv && !cpu->emulation && ret_stack < entry_stack && cpu->S <= entry_stack) {
#if SNESRECOMP_TRACE
        cpu_trace_missing_pushed_target(cpu, target, source);
#endif
        cpu->PB = bank;
        RecompStackPop();
        return cpu_dispatch_paired_tail_from(cpu, target, entry_stack, hrv, source);
    }
    if (ret_stack != entry_stack && cpu_resolve_ancestor_skip(ret_stack) >= 0) {
        cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);
        if (cpu_dispatch_has_entry(cpu, target)) {
            cpu_tailcall_request(target, (uint16)(ret_stack + frame_bytes), source);
            RecompStackPop();
            return RECOMP_RETURN_TAILCALL;
        }
        {
            int skip = cpu_resolve_ancestor_skip(ret_stack);
            RecompStackPop();
            return (RecompReturn)skip;
        }
    }
    cpu_trace_mark_nlr_exit(BD_EXIT_KIND_TRAMPOLINE);
    miss_stack = (uint16)((ret_stack > entry_stack ? ret_stack : entry_stack) + frame_bytes);
    sr_exit_s_check(cpu, entry_stack, ret_stack, name, source);
    if (!hrv) {
        cpu_tailcall_request(target, miss_stack, source);
        RecompStackPop();
        return RECOMP_RETURN_TAILCALL;
    }
    RecompStackPop();
    return cpu_dispatch_pc_from(cpu, target, miss_stack, source);
}
