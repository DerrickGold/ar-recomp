#ifndef AR_RUN_DIR_H
#define AR_RUN_DIR_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/* Each opted-in invocation gets a timestamped diagnostic directory with
 * automatic retention or a preserved destination selected by --log-dir.
 * It contains console.log,
 * run_info.txt (cmd line + AR_* env), anomaly captures, dumps, snapshots,
 * screenshots. Battery SRAM and save-state slots stay in saves/.
 *
 * Development/trace builds enable this by default. Play/package builds do
 * not create runs/ unless AR_ENABLE_RUN_DIR=1 is set for a diagnostic launch.
 * AR_NO_RUN_DIR=1 always opts out. When disabled, explicitly requested bare
 * diagnostic output filenames fall back under saves/. */

/* Initialize automatic storage or reserve a new/empty explicit destination.
 * False means the explicit destination could not be opened. */
bool RunDirInit(int argc, char **argv, const char *destination);
void RunDirReport(void);
/* The runtime logger owns the returned stream. Temporary captures are capped. */
FILE *RunDirOpenRuntimeLog(void);
bool RunDirIsTemporary(void);

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
