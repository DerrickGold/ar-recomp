#include "run_dir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
extern char **environ;
#endif

static char g_run_dir[256] = "saves";
static int g_enabled;

const char *RunDirPath(void) { return g_run_dir; }

void RunDirFile(char *buf, size_t n, const char *fmt, ...) {
  int off = snprintf(buf, n, "%s/", g_run_dir);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + off, n - off, fmt, ap);
  va_end(ap);
}

#ifndef _WIN32

/* Duplicate stdout+stderr onto a `tee` child so the console still echoes
 * while everything is captured. tee is a separate process: on a crash the
 * kernel closes our end and tee flushes what it already received, so the
 * log survives SIGSEGV/SIGBUS. */
static void tee_console(const char *log_path) {
  char cmd[300];
  snprintf(cmd, sizeof cmd, "exec tee '%s'", log_path);
  FILE *p = popen(cmd, "w");
  if (!p) return;
  setvbuf(p, NULL, _IONBF, 0);
  fflush(stdout);
  fflush(stderr);
  dup2(fileno(p), STDOUT_FILENO);
  dup2(fileno(p), STDERR_FILENO);
  /* p stays open for the process lifetime; tee exits when our fds close. */
}

/* Bare filenames (no '/') for per-run outputs land inside the run dir, so
 * `AR_TRACE=win.jsonl ./build/ActRaiserRecomp ...` doesn't litter the cwd. */
static void rebase_bare_env(const char *name) {
  const char *v = getenv(name);
  if (!v || !v[0] || strchr(v, '/')) return;
  char path[300];
  RunDirFile(path, sizeof path, "%s", v);
  setenv(name, path, 1);
}

static void write_run_info(int argc, char **argv) {
  char path[300];
  RunDirFile(path, sizeof path, "run_info.txt");
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "cmd:");
  for (int i = 0; i < argc; i++) fprintf(f, " %s", argv[i]);
  time_t t = time(NULL);
  char ts[64];
  strftime(ts, sizeof ts, "%F %T", localtime(&t));
  fprintf(f, "\ndate: %s\n--- AR_*/SNESRECOMP_* env (pre-config) ---\n", ts);
  for (char **e = environ; *e; e++)
    if (!strncmp(*e, "AR_", 3) || !strncmp(*e, "SNESRECOMP_", 11) ||
        !strncmp(*e, "SNESREF_", 8))
      fprintf(f, "%s\n", *e);
  fclose(f);
}

void RunDirInit(int argc, char **argv) {
  const char *no = getenv("AR_NO_RUN_DIR");
  if (no && no[0] && no[0] != '0') return;   /* legacy saves/ layout; the
                                              * config file's AR_TRACE_WATCH
                                              * fallback line applies */

  if (mkdir("runs", 0755) != 0 && access("runs", W_OK) != 0) return;

  time_t t = time(NULL);
  char ts[32];
  strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&t));
  char dir[256];
  snprintf(dir, sizeof dir, "runs/%s", ts);
  for (int n = 1; mkdir(dir, 0755) != 0; n++) {
    if (n > 99) return;
    snprintf(dir, sizeof dir, "runs/%s-%d", ts, n);
  }
  snprintf(g_run_dir, sizeof g_run_dir, "%s", dir);
  g_enabled = 1;

  char log[300];
  RunDirFile(log, sizeof log, "console.log");
  tee_console(log);

  /* Engine-side writers (fn_census, crash dispatch log) key off this. */
  setenv("AR_RUN_DIR", g_run_dir, 1);

  /* Default the always-on anomaly capture into the run dir. overwrite=0:
   * a real env var still wins; config.ini's setenv(...,0) runs later so
   * this default also beats a stale saves/anom line in dev-config.ini.
   * A deliberate AR_TRACE=<file> windowed capture disables watch mode in
   * ar_trace.c, so the default is harmless in that case too. */
  setenv("AR_TRACE_WATCH", "anom", 0);
  rebase_bare_env("AR_TRACE_WATCH");
  rebase_bare_env("AR_TRACE");
  rebase_bare_env("AR_INPUT_RECORD");
  rebase_bare_env("AR_DRIFT_LOG");
  rebase_bare_env("AR_MX_OUT");
  rebase_bare_env("AR_WRAM_TRACE");

  write_run_info(argc, argv);

  unlink("runs/latest");
  symlink(g_run_dir + strlen("runs/"), "runs/latest");   /* relative link */

  fprintf(stderr, "[run-dir] %s (console.log + dumps ringfenced here; "
                  "AR_NO_RUN_DIR=1 for legacy saves/ layout)\n", g_run_dir);
  (void)g_enabled;
}

#else /* _WIN32: no tee/symlink plumbing — keep the legacy layout. */
void RunDirInit(int argc, char **argv) { (void)argc; (void)argv; }
#endif
