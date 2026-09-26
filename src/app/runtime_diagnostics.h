#ifndef AR_RUNTIME_DIAGNOSTICS_H
#define AR_RUNTIME_DIAGNOSTICS_H

#include <stdbool.h>

#include "snesrecomp/runner.h"

/* Subscribe the application-owned flight recorder to public runner events.
 * Bind once after SnesInit and unbind before SnesShutdown. */
bool RuntimeDiagnostics_Bind(SrRunnerHandle *runner);
void RuntimeDiagnostics_Unbind(void);

/* Write the current CPU, WRAM, SRAM, dispatch, and present-cadence state into
 * the active run directory. The symbol name is also consumed by the runtime's
 * weak watchdog hook and therefore remains part of the runtime ABI. */
void DumpDiagState(const char *tag);

#endif /* AR_RUNTIME_DIAGNOSTICS_H */
