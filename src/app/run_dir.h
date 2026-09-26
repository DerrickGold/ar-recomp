#ifndef RUN_DIR_H
#define RUN_DIR_H

#include <stddef.h>

/* Per-run artifact directory (runs/<YYYYMMDD-HHMMSS>/), created at startup so
 * every invocation — plain `./build/ActRaiserRecomp ar.sfc ...` included —
 * ringfences its own diagnostics: console.log (stdout+stderr tee),
 * run_info.txt (cmd line + AR_* env), anomaly captures, dumps, snapshots,
 * screenshots. Battery SRAM and save-state slots stay in saves/.
 *
 * Development/trace builds enable this by default. Play/package builds do
 * not create runs/ unless AR_ENABLE_RUN_DIR=1 is set for a diagnostic launch.
 * AR_NO_RUN_DIR=1 always opts out. When disabled, explicitly requested bare
 * diagnostic output filenames fall back under saves/. */

/* If enabled by the build/runtime policy, create the run dir, tee the console,
 * export AR_RUN_DIR, write run_info.txt, and update runs/latest. Trace capture
 * remains separately opt-in. Call once before anything prints. */
void RunDirInit(int argc, char **argv);

/* Rebase bare-filename output env vars (SNESRECOMP_TRACE_WATCH_FILE / SNESRECOMP_TRACE_FILE /
 * AR_INPUT_RECORD / AR_DRIFT_LOG / AR_MX_OUT / AR_WRAM_TRACE / AR_SIM3D_TRACE /
 * AR_SIM3D_D1_TRACE) into the run
 * dir — call once right after ParseConfigFile so ini-provided values (e.g.
 * `SNESRECOMP_TRACE_WATCH_FILE = anom` in dev-config.ini) are covered too. Values
 * containing '/' are left alone. */
void RunDirRebaseEnvOutputs(void);

/* Append the authoritative post-config runner-recorder and deep-instrumentation
 * state to run_info.txt. No-op when per-run directories are unavailable. */
void RunDirRecordTraceStatus(const char *status);

/* The run directory ("runs/<ts>"), or "saves" when opted out/unavailable.
 * No trailing slash. Always a usable prefix. */
const char *RunDirPath(void);

/* snprintf "<run-dir>/<fmt...>" into buf. */
void RunDirFile(char *buf, size_t n, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#endif
