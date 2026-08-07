#ifndef SIM_PHASE0_TRACE_H
#define SIM_PHASE0_TRACE_H

#include <stdbool.h>

#include "types.h"

typedef struct Ppu Ppu;

/* Read-only Phase 0 evidence trace. The explicit Open entry is useful for
 * tests/tools; production normally uses InitFromEnvironment after run-dir
 * rebasing and AR_SIM3D_TRACE=<jsonl-path>. */
bool SimPhase0Trace_Open(const char *path);
void SimPhase0Trace_InitFromEnvironment(void);
void SimPhase0Trace_Frame(uint32 host_frame, const uint8 *wram,
                          const Ppu *ppu);
void SimPhase0Trace_Close(void);

#endif  /* SIM_PHASE0_TRACE_H */
