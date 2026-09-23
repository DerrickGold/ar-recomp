#include "actraiser_native_call.h"
#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"

RecompReturn ActRaiserNativeCall(CpuState *cpu, ActRaiserNativeLeaf leaf,
    uint8_t bank, uint16_t caller, bool long_call) {
  const uint16_t stack = cpu->S;
  const uint8_t pb = cpu->PB;
  if (long_call) cpu_write8(cpu, 0, cpu->S--, pb);
  ActRaiserCpuHle_PushWord(cpu, caller);
  cpu->host_return_valid = 1;
  CpuReturnScope scope;
  cpu_return_scope_begin(&scope, cpu, ((uint32_t)pb << 16) | (uint16_t)(caller + 1),
                         stack, long_call ? 3 : 2);
  cpu->PB = bank;
  RecompReturn result = leaf(cpu);
  if (result == RECOMP_RETURN_OWNED_UNWIND && cpu_finish_owned_unwind(&scope, cpu))
    result = RECOMP_RETURN_NORMAL;
  /* Generated RTL delegates PB restoration to its paired JSL caller. Parked
   * and escaping owned returns instead retain their live native continuation. */
  if (long_call && result != RECOMP_RETURN_PARKED_WAIT && result != RECOMP_RETURN_OWNED_UNWIND)
    cpu->PB = pb;
  cpu_return_scope_end(&scope);
  if (result == RECOMP_RETURN_NORMAL && (cpu->S != stack || cpu->PB != pb))
    ActRaiserHleFatal("Native leaf at $%02X:%04X returned outside its frame contract", bank, caller);
  return result;
}
