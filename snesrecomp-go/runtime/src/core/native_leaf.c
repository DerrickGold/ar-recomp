#include "snesrecomp/game/cpu.h"

/* Separate archive member: device-only CPU consumers do not acquire generated
 * call-owner infrastructure merely by linking the CPU memory/dispatch API. */
int cpu_invoke_rts_leaf(CpuState *cpu, RecompReturn (*leaf)(CpuState *),
                        uint32 continuation_pc24) {
    if (!cpu || !leaf || cpu->emulation || cpu->S < 2u || cpu->S > 0x1fffu ||
        continuation_pc24 > 0xffffffu || (continuation_pc24 >> 16) != cpu->PB)
        return 0;
    const uint16 stack = cpu->S;
    const uint8 bank = cpu->PB, paired = cpu->host_return_valid;
    const uint16 return_word = (uint16)(continuation_pc24 - 1u);
    cpu_write8(cpu, 0, cpu->S--, (uint8)(return_word >> 8));
    cpu_write8(cpu, 0, cpu->S--, (uint8)return_word);
    cpu->host_return_valid = 1;
    CpuReturnScope owner;
    cpu_return_scope_begin(&owner, cpu, continuation_pc24, stack, 2);
    const RecompReturn result = leaf(cpu);
    const int valid = result == RECOMP_RETURN_NORMAL && cpu->S == stack &&
        cpu->PB == bank && g_cpu_return_scope == &owner && !owner.adjusted_return;
    cpu_return_scope_end(&owner);
    if (valid) cpu->host_return_valid = paired;
    return valid;
}
