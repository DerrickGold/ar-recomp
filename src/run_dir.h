#ifndef RUN_DIR_H
#define RUN_DIR_H

#include <stddef.h>

/* Per-run artifact directory (runs/<YYYYMMDD-HHMMSS>/), created at startup so
 * every invocation — plain `./build/ActRaiserRecomp ar.sfc ...` included —
 * ringfences its own diagnostics: console.log (stdout+stderr tee),
 * run_info.txt (cmd line + AR_* env), anomaly captures, dumps, snapshots,
 * screenshots. Battery SRAM and save-state slots stay in saves/.
 *
 * AR_NO_RUN_DIR=1 opts out (paths fall back to the legacy saves/ layout —
 * for the launcher/production or quick throwaway runs). */

/* Create the run dir, tee the console, export AR_RUN_DIR, apply env-var
 * defaults (AR_TRACE_WATCH into the run dir; bare-filename AR_TRACE /
 * AR_INPUT_RECORD / AR_DRIFT_LOG / AR_MX_OUT rewritten into it), write
 * run_info.txt, update the runs/latest symlink. Call once, first thing in
 * main() — before anything prints or reads AR_* paths. */
void RunDirInit(int argc, char **argv);

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
