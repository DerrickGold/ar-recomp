#include "dev/debug_state.h"
#include "snesrecomp/game/runtime.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

static int s_calls;
static int s_slot;

/* A restore reaching the runner is a failure even if a caller would clear
 * localization caches afterwards. No ROM, files, or display are involved. */
void RtlSaveLoad(int command, int slot) {
  CHECK(command == kSaveLoad_Save);
  ++s_calls;
  s_slot = slot;
}

int main(void) {
  for (int slot = 0; slot <= 99; ++slot)
    CHECK(!DebugState_Apply(kSaveLoad_Load, slot));
  CHECK(s_calls == 0);
  CHECK(!DebugState_Apply(kSaveLoad_Save, -1));
  CHECK(!DebugState_Apply(kSaveLoad_Save, 100));
  CHECK(!DebugState_Apply(kSaveLoad_Load, -1));
  CHECK(!DebugState_Apply(kSaveLoad_Load, 100));
  CHECK(!DebugState_Apply(0, 0));
  CHECK(s_calls == 0);
  CHECK(DebugState_Apply(kSaveLoad_Save, 0));
  CHECK(s_calls == 1 && s_slot == 0);
  CHECK(DebugState_Apply(kSaveLoad_Save, 99));
  CHECK(s_calls == 2 && s_slot == 99);
  CHECK(!DebugState_Apply(kSaveLoad_Load, 99));
  CHECK(s_calls == 2);
  puts("debug state guards passed");
  return 0;
}
