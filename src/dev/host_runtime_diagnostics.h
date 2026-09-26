#ifndef AR_HOST_RUNTIME_DIAGNOSTICS_H
#define AR_HOST_RUNTIME_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

/* Optional runtime checks and AR_PERF / SNESRECOMP_APU_PROFILE reporting.
 * Called on the game thread. Tick scopes include turbo subframes; draw scopes
 * include PPU capture and host post-processing, before screenshot/presentation. */
typedef struct HostRuntimeTickProfile {
  uint64_t perf_start_ms;
  uint64_t apu_start_ns;
  unsigned long pushes;
  uint64_t loops;
} HostRuntimeTickProfile;

/* Initialize tracing before loading config; arm generated checks after boot. */
void HostRuntimeDiagnostics_InitTrace(void);
void HostRuntimeDiagnostics_ConfigureChecks(void);
HostRuntimeTickProfile HostRuntimeDiagnostics_BeginTick(void);
void HostRuntimeDiagnostics_EndTick(HostRuntimeTickProfile scope);
uint64_t HostRuntimeDiagnostics_BeginDraw(void);
void HostRuntimeDiagnostics_EndDraw(uint64_t started_ms);
bool HostRuntimeDiagnostics_PipelineLoggingEnabled(void);

#endif /* AR_HOST_RUNTIME_DIAGNOSTICS_H */
