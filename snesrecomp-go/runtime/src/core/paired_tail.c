#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "paired_tail_internal.h"
#include <stddef.h>

struct PairedTailDriver {
    PairedTailDriver *previous;
    CpuState *cpu;
    uint16 entry_stack;
    int activation_depth;
};

/* Keep dispatch-dependent code in its own archive member. Device-only SDK
 * consumers must not acquire a generated dispatch-table link dependency. */
RecompReturn cpu_dispatch_paired_tail_from(CpuState *cpu, uint32 pc24,
        uint16 entry_stack, uint8 hrv, uint32 source_pc24) {
    cpu_tailcall_inherit_return_context(entry_stack, hrv);
    if (g_sr_paired_tail_driver != NULL && g_sr_paired_tail_driver->cpu == cpu &&
        g_sr_paired_tail_driver->entry_stack == entry_stack &&
        g_sr_paired_tail_driver->activation_depth == g_recomp_stack_top) {
        cpu_tailcall_request(pc24, entry_stack, source_pc24);
        return RECOMP_RETURN_TAILCALL;
    }
    PairedTailDriver driver = {g_sr_paired_tail_driver, cpu, entry_stack,
                               g_recomp_stack_top};
    g_sr_paired_tail_driver = &driver;
    RecompReturn result = cpu_dispatch_pc_from(cpu, pc24, entry_stack, source_pc24);
    g_sr_paired_tail_driver = driver.previous;
    /* A missing body or an HLE without a generated prologue must not leak
     * this one-shot context into an unrelated later function. */
    (void)cpu_take_tailcall_return_context(NULL, NULL);
    return result;
}
