#include "actraiser/actraiser_save_transaction.h"

#include "actraiser/actraiser_hle_fatal.h"
#include "save_system.h"

extern RecompReturn bank_03_A656_M0X0(CpuState *cpu);
extern RecompReturn bank_03_A656_M1X0(CpuState *cpu);
static bool s_delegate;

bool ActRaiser_SaveStoryEntry(CpuState *cpu) {
  if (s_delegate) {
    s_delegate = false;
    return false;
  }
  return cpu && cpu->PB == 3 && !cpu->x_flag && !cpu->emulation && cpu->D == 0;
}

RecompReturn ActRaiser_SaveStory(CpuState *cpu) {
  SaveError error = {{0}};
  if (!SaveSystem_BeginNativeWrite(&error))
    ActRaiserHleFatal("Cannot begin story save: %s", error.message);

  /* Delegate with the original native JSL frame and entry width. Preserve
   * every copy, checksum store and register/stack result. Frame persistence
   * stays inhibited even if the native writer services a host frame. */
  s_delegate = true;
  RecompReturn result = cpu->m_flag
      ? bank_03_A656_M1X0(cpu) : bank_03_A656_M0X0(cpu);
  s_delegate = false;
  const bool ended = SaveSystem_EndNativeWrite(result == RECOMP_RETURN_NORMAL, &error);
  if (result == RECOMP_RETURN_NORMAL && !ended)
    ActRaiserHleFatal("Cannot finish story save: %s", error.message);
  return result;
}
