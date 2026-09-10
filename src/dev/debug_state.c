#include "dev/debug_state.h"

#include <stdio.h>

#include "snesrecomp/game/runtime.h"

bool DebugState_Apply(int command, int slot) {
  if (slot < 0 || slot > 99) {
    fprintf(stderr, "[debug-state] rejected invalid slot %d (want 0..99)\n",
            slot);
    return false;
  }
  if (command == kSaveLoad_Load) {
    /* A frame boundary only suspends the host call stack; it does not make
     * that continuation serializable. Native font mode is not a workaround.
     * Do not load first and then invalidate caches: that is already too late
     * to prevent RAM/stack disagreement, including pending native controls. */
    fprintf(stderr,
            "[debug-state] restore of slot %d rejected: snapshots do not "
            "restore recompiled game execution. No state was changed. "
            "Use a battery save and input recording instead.\n",
            slot);
    return false;
  }
  if (command != kSaveLoad_Save)
    return false;
  RtlSaveLoad(kSaveLoad_Save, slot);
  fprintf(stderr,
          "[debug-state] snapshot capture requested for slot %d "
          "(inspection only; not a resumable save state)\n",
          slot);
  return true;
}
