#include "snesrecomp/game/cpu.h"
#include "snesrecomp/game/generated_support.h"
#include "paired_tail_internal.h"
#include <stddef.h>

struct PairedTailDriver {
    PairedTailDriver *previous;
    CpuState *cpu;
    uint16 entry_stack;
    uint8 hrv;
    int activation_depth;
};

/* Keep dispatch-dependent code in its own archive member. Device-only SDK
 * consumers must not acquire a generated dispatch-table link dependency. */
RecompReturn cpu_dispatch_paired_tail_from(CpuState *cpu, uint32 pc24,
        uint16 entry_stack, uint8 hrv, uint32 source_pc24) {
    PairedTailDriver *owner = g_sr_paired_tail_driver;
    int adopted = 0;
    /* A requester whose context lies below native S has already discarded
     * its own entry frame (for example PLA, then a jump into caller code).
     * Inner drivers whose activation entry also lies below native S are dead
     * for the same reason. When S is exactly the entry of the nearest live
     * owner, no suspended inner C frame can resume by a native return: hand
     * the transfer back to that owner instead of nesting a driver around a
     * stale context. Foreign or partially popped drivers are barriers. As
     * with moved-return ownership, wrapping/non-WRAM stacks are not inferred. */
    if (owner != NULL && !cpu->emulation && cpu->S <= 0x1fffu &&
        entry_stack < cpu->S) {
        PairedTailDriver *live = owner;
        int skipped = 0;
        while (live != NULL && live->cpu == cpu && live->entry_stack < cpu->S) {
            live = live->previous;
            ++skipped;
        }
        if (live != NULL && live->cpu == cpu && cpu->S == live->entry_stack &&
            live->activation_depth <= g_recomp_stack_top) {
            owner = live;
            entry_stack = live->entry_stack;
            hrv = live->hrv;
            adopted = 1;
            if (skipped != 0) {
                g_sr_paired_tail_owner = live;
            }
        }
    }
    cpu_tailcall_inherit_return_context(entry_stack, hrv);
    if (owner != NULL && owner->cpu == cpu && owner->entry_stack == entry_stack &&
        (adopted || owner->activation_depth == g_recomp_stack_top)) {
        cpu_tailcall_request(pc24, entry_stack, source_pc24);
        return RECOMP_RETURN_TAILCALL;
    }
    PairedTailDriver driver = {g_sr_paired_tail_driver, cpu, entry_stack, hrv,
                               g_recomp_stack_top};
    g_sr_paired_tail_driver = &driver;
    RecompReturn result = cpu_dispatch_pc_from(cpu, pc24, entry_stack, source_pc24);
    while (result == RECOMP_RETURN_TAILCALL && g_sr_paired_tail_owner == &driver) {
        /* A deeper transfer adopted this driver through dead inner drivers. */
        g_sr_paired_tail_owner = NULL;
        result = cpu_dispatch_pc_from(cpu, g_tailcall_pc24, g_tailcall_miss_s,
                                      g_tailcall_src24);
    }
    g_sr_paired_tail_driver = driver.previous;
    /* A missing body or an HLE without a generated prologue must not leak
     * this one-shot context into an unrelated later function. An adopted
     * transfer still unwinding outward keeps its context for the owner. */
    if (result != RECOMP_RETURN_TAILCALL || g_sr_paired_tail_owner == NULL)
        (void)cpu_take_tailcall_return_context(NULL, NULL);
    return result;
}
