#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/trace.h"

/* The envelope generated direct calls used to carry inline at every JSR/JSL
 * site, moved here statement for statement. Its trace probes and the call
 * M/X check compile with this library's SNESRECOMP_TRACE and
 * SNESRECOMP_TRACE_RECORDER. Diagnostic builds must configure the generated
 * code and runner consistently. Keep the public boundaries out of line even under
 * IPO: expanding them back into every generated caller defeats this contract. */

NOINLINE uint16 sr_call_enter(CpuState *cpu, CpuReturnScope *scope,
                     uint16 return_address, uint16 continuation,
                     uint16 entry_stack, int expected_m, int expected_x,
                     const char *function_name, uint32 site_pc24) {
    uint16 call_stack = cpu->S;
    cpu_write8(cpu, 0x00, cpu->S, (uint8)(return_address >> 8));
    cpu->S = (uint16)(cpu->S - 1);
    cpu_write8(cpu, 0x00, cpu->S, (uint8)return_address);
    cpu->S = (uint16)(cpu->S - 1);
    cpu->host_return_valid = 1;
    cpu_return_scope_begin(scope, cpu,
                           ((uint32)cpu->PB << 16) | continuation,
                           entry_stack, 2u);
    sr_call_mx_check(cpu, expected_m, expected_x, function_name, site_pc24);
    return call_stack;
}

NOINLINE uint16 sr_call_enter_long(CpuState *cpu, CpuReturnScope *scope,
                          uint8 *saved_pb, uint8 target_bank,
                          uint16 return_address, uint16 continuation,
                          uint16 entry_stack, int expected_m,
                          int expected_x, const char *function_name,
                          uint32 site_pc24) {
    uint16 call_stack = cpu->S;
    cpu_write8(cpu, 0x00, cpu->S, cpu->PB);
    cpu->S = (uint16)(cpu->S - 1);
    cpu_write8(cpu, 0x00, cpu->S, (uint8)(return_address >> 8));
    cpu->S = (uint16)(cpu->S - 1);
    cpu_write8(cpu, 0x00, cpu->S, (uint8)return_address);
    cpu->S = (uint16)(cpu->S - 1);
    cpu->host_return_valid = 1;
    /* The continuation is in the caller's bank: open the scope before PB
     * changes. */
    cpu_return_scope_begin(scope, cpu,
                           ((uint32)cpu->PB << 16) | continuation,
                           entry_stack, 3u);
    *saved_pb = cpu->PB;
    cpu_trace_pb_change(cpu, 0, *saved_pb, target_bank, CPU_TR_JSL);
    cpu->PB = target_bank;
    sr_call_mx_check(cpu, expected_m, expected_x, function_name, site_pc24);
    return call_stack;
}

static int leave_call(CpuState *cpu, CpuReturnScope *scope,
                      RecompReturn result, uint16 call_stack, int long_call,
                      uint8 saved_pb) {
    if (result == RECOMP_RETURN_PARKED_WAIT) {
        cpu_return_scope_end(scope);
        return (int)result; /* preserve the native parked CPU, PB and S */
    }
    if (result == RECOMP_RETURN_OWNED_UNWIND) {
        if (!cpu_finish_owned_unwind(scope, cpu)) {
            cpu_return_scope_end(scope);
            return (int)result; /* discard the activation; keep native S/PB */
        }
        result = RECOMP_RETURN_NORMAL; /* resume this exact call once */
    }
    if (long_call) {
        cpu_trace_pb_change(cpu, 0, cpu->PB, saved_pb, CPU_TR_RTL);
        cpu->PB = saved_pb;
    }
    cpu_return_scope_end(scope); /* also unwinds ownership on a non-local return */
    if (result != RECOMP_RETURN_NORMAL) {
        cpu_trace_event(cpu, 0, CPU_TR_NLR_PROPAGATE, (uint8)result, 0);
        cpu_trace_mark_nlr_exit(BD_EXIT_KIND_SKIP_PROPAGATION);
        return result == RECOMP_RETURN_TAILCALL ? (int)result
                                                : (int)result - 1;
    }
    /* A native callee cleanup keeps its actual post-return S. */
    if (!scope->adjusted_return) cpu->S = call_stack;
    return -1;
}

NOINLINE int sr_call_leave(CpuState *cpu, CpuReturnScope *scope, RecompReturn result,
                  uint16 call_stack) {
    return leave_call(cpu, scope, result, call_stack, 0, 0u);
}

NOINLINE int sr_call_leave_long(CpuState *cpu, CpuReturnScope *scope,
                       RecompReturn result, uint16 call_stack,
                       uint8 saved_pb) {
    return leave_call(cpu, scope, result, call_stack, 1, saved_pb);
}
