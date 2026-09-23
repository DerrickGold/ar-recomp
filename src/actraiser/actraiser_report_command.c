#include "actraiser_report_command.h"

#include "actraiser_cpu_hle_internal.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_native_call.h"

#define LEAF(bank, pc) extern RecompReturn bank_##bank##_##pc##_M1X0(CpuState *)
LEAF(01, 899B); LEAF(03, BF8C); LEAF(01, 8A3F); LEAF(03, 8168);
LEAF(01, 8A9A); LEAF(01, 8AF5); LEAF(01, 8CB6); LEAF(01, 9270);
#undef LEAF

bool ActRaiserReportCommand_Entry(const CpuState *cpu, unsigned action) {
  return cpu && action >= 12 && action <= 15 && cpu->PB == 1 && cpu->DB == 1 &&
      !cpu->D && !cpu->emulation && cpu->m_flag && !cpu->x_flag;
}

static RecompReturn Call(CpuState *cpu, ActRaiserNativeLeaf leaf, uint8_t bank,
                         uint16_t caller, bool long_call) {
  RecompReturn result = ActRaiserNativeCall(cpu, leaf, bank, caller, long_call);
  if (result == RECOMP_RETURN_NORMAL && (!cpu->m_flag || cpu->x_flag ||
      cpu->DB != 1 || cpu->D || cpu->emulation))
    ActRaiserHleFatal("Report command leaf at $%02X:%04X broke its ABI", bank, caller);
  return result;
}

RecompReturn ActRaiserReportCommand_Run(CpuState *cpu, unsigned action, bool keep_open) {
  if (!ActRaiserReportCommand_Entry(cpu, action)) ActRaiserHleFatal("Invalid report command entry");
  /* $81D7's DEC chain selects 12/13/14 with A=0; the final default branch
   * reaches command 15 with A=1. Preserve high A and every unrelated flag. */
  cpu_write_a8(cpu, action == 15 ? 1 : 0);
  ActRaiserCpuHle_SetNegativeZero8(cpu, (uint8_t)cpu->A);
  RecompReturn result;
#define CALL(leaf, bank, caller, long_call) do { \
    result = Call(cpu, leaf, bank, caller, long_call); \
    if (result != RECOMP_RETURN_NORMAL) goto nonlocal; \
  } while (0)
  /* Original US wrappers $8530/$853B/$854A/$8559. Keep the exact return
   * addresses: localization and save ownership observe these native calls. */
  switch (action) {
    case 12: CALL(bank_01_899B_M1X0, 1, 0x8532, false); break;
    case 13:
      CALL(bank_03_BF8C_M1X0, 3, 0x853e, true);
      CALL(bank_01_8A3F_M1X0, 1, 0x8541, false); break;
    case 14:
      CALL(bank_03_8168_M1X0, 3, 0x854d, true);
      CALL(bank_01_8A9A_M1X0, 1, 0x8550, false); break;
    case 15: CALL(bank_01_8AF5_M1X0, 1, 0x855b, false); break;
  }
  if (!keep_open) {
    static const uint16_t close_caller[] = {0x8535, 0x8544, 0x8553, 0x855e};
    CALL(bank_01_8CB6_M1X0, 1, close_caller[action - 12], false);
    CALL(bank_01_9270_M1X0, 1, close_caller[action - 12] + 3, false);
  }
  /* JP wrappers $84FA/$84FF/$8504/$8509 return SEC directly. Native menu
   * redraw/release barriers own the next selection; never re-run the action. */
  cpu->_flag_C = keep_open;
  cpu->P = (uint8_t)((cpu->P & ~1u) | keep_open);
  cpu->S = (uint16_t)(cpu->S + 2);
  return RECOMP_RETURN_NORMAL;
nonlocal:
  return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
#undef CALL
}
