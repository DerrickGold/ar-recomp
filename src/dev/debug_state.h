#ifndef DEBUG_STATE_H
#define DEBUG_STATE_H

#include <stdbool.h>

/* Host-only debug action boundary. Commands are kSaveLoad_* from the runner.
 * Capture remains useful for inspecting hardware/RAM. Restore is rejected
 * before touching the runner: its snapshots do not restore the recompiled
 * CPU, suspended game stack, or host-owned dialogue execution state.
 * True means a capture was requested, not that the void runner API proved
 * successful disk I/O. This has no connection to battery-save loading. */
bool DebugState_Apply(int command, int slot);

#endif
