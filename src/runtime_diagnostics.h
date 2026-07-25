#ifndef RUNTIME_DIAGNOSTICS_H
#define RUNTIME_DIAGNOSTICS_H

/* Write the current CPU, WRAM, SRAM, dispatch, and present-cadence state into
 * the active run directory. The symbol name is also consumed by the runtime's
 * weak watchdog hook and therefore remains part of the runtime ABI. */
void DumpDiagState(const char *tag);

#endif /* RUNTIME_DIAGNOSTICS_H */
