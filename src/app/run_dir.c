#include "app/run_dir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/diagnostic_options.h"
#include "app/run_retention.h"
#include "constants.h"
#include "app/runtime_log.h"
#include "save/save_paths.h"
#include "snesrecomp/support/utf8_fs.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
extern char **environ;
#endif

#ifndef AR_RUN_DIR_DEFAULT_ENABLED
#define AR_RUN_DIR_DEFAULT_ENABLED 1
#endif

enum { kRetainedRuns = 8, kRetentionDays = 7 };
static const uint64_t kRunByteBudget = UINT64_C(512) * 1024 * 1024;
static const char kRunsRoot[] = "runs";
static char s_run_dir[kHostPathCapacity] = "saves";
static bool s_enabled;
static FILE *s_owner;
/* Keep the ownership handle open until process exit, including when a
 * stalled diagnostic worker is detached during shutdown. */
static void PruneRuns(size_t keep) {
  RunRetention_Prune(kRunsRoot, keep, kRunByteBudget,
                     time(NULL) - kRetentionDays * 24 * 60 * 60);
}

const char *RunDirPath(void) { return s_run_dir; }

void RunDirFile(char *buf, size_t n, const char *fmt, ...) {
  if (!n) return;
  const int off = snprintf(buf, n, "%s/", s_run_dir);
  if (off < 0 || (size_t)off >= n) {
    buf[0] = 0;
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  const int length = vsnprintf(buf + off, n - (size_t)off, fmt, ap);
  va_end(ap);
  if (length < 0 || (size_t)length >= n - (size_t)off) buf[0] = 0;
}

void RunDirRecordTraceStatus(const char *status) {
  if (!s_enabled || !status) return;
  char path[kHostPathCapacity];
  RunDirFile(path, sizeof(path), "run_info.txt");
  FILE *file = sr_fopen(path, "a");
  if (!file) return;
  fprintf(file, "--- resolved diagnostics (post-config) ---\nrunner_trace=%s\n", status);
  fclose(file);
}

static void SetEnvironment(const char *name, const char *value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

static void WriteRunInfo(int argc, char **argv) {
  char path[kHostPathCapacity];
  RunDirFile(path, sizeof(path), "run_info.txt");
  FILE *file = sr_fopen(path, "w");
  if (!file) return;
  fprintf(file, "cmd:");
  for (int i = 0; i < argc; i++) fprintf(file, " %s", argv[i]);
  const time_t now = time(NULL);
  char timestamp[64];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
  fprintf(file, "\ndate: %s\n--- AR_*/SNESRECOMP_* env (pre-config) ---\n", timestamp);
#ifdef _WIN32
  char **env = _environ;
#else
  char **env = environ;
#endif
  for (; env && *env; ++env)
    if (!strncmp(*env, "AR_", 3) || !strncmp(*env, "SNESRECOMP_", 11) ||
        !strncmp(*env, "SNESREF_", 8))
      fprintf(file, "%s\n", *env);
  fclose(file);
}

FILE *RunDirOpenRuntimeLog(void) {
  if (!s_enabled) return NULL;
  char path[kHostPathCapacity];
  RunDirFile(path, sizeof(path), "console.log");
  return sr_fopen(path, "wbx");
}

bool RunDirIsTemporary(void) { return s_owner != NULL; }

void RunDirReport(void) {
  if (s_enabled)
    RuntimeLog_Printf("[run-dir] %s (%s diagnostic bundle)\n", s_run_dir,
                     RunDirIsTemporary() ? "temporary" : "preserved");
}

bool RunDirInit(int argc, char **argv, const char *destination) {
  if (destination) {
    if (strlen(destination) >= sizeof(s_run_dir) ||
        !SavePaths_EnsureDirectory(destination, NULL) ||
        !RunRetention_Preserve(destination)) {
      fprintf(stderr, "[run-dir] --log-dir requires a new or empty directory: %s\n", destination);
      return false;
    }
    memcpy(s_run_dir, destination, strlen(destination) + 1);
  } else {
    const bool automatic = !DiagnosticOptionEnabled("AR_NO_RUN_DIR") &&
        (DiagnosticOptionEnabled("AR_ENABLE_RUN_DIR") || AR_RUN_DIR_DEFAULT_ENABLED);
    if (!automatic) return true;
    if (sr_mkdir(kRunsRoot) != 0 && !sr_path_is_directory(kRunsRoot)) return true;
    const time_t now = time(NULL);
    char timestamp[32], directory[64];
    if (!strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", localtime(&now))) return true;
    if (!RunRetention_AutomaticPath(kRunsRoot, timestamp, directory, sizeof(directory), &s_owner))
      return true;
    memcpy(s_run_dir, directory, strlen(directory) + 1);
    PruneRuns(kRetainedRuns - 1);
#ifndef _WIN32
    /* Never unlink a user-created regular file named latest. */
    char relative[64];
    if (!sr_path_exists("runs/latest") || readlink("runs/latest", relative, sizeof(relative)) >= 0) {
      unlink("runs/latest");
      symlink(directory + strlen(kRunsRoot) + 1, "runs/latest");
    }
#endif
  }
  s_enabled = true;
  SetEnvironment("AR_RUN_DIR", s_run_dir);
  WriteRunInfo(argc, argv);
  return true;
}

static void RebaseBareOutput(const char *name) {
  const char *value = getenv(name);
  if (!value || !*value || !strcmp(value, ".") || !strcmp(value, "..") ||
      strchr(value, '/') || strchr(value, '\\') || strchr(value, ':'))
    return;
  char path[kHostPathCapacity];
  RunDirFile(path, sizeof(path), "%s", value);
  if (path[0]) SetEnvironment(name, path);
}

void RunDirRebaseEnvOutputs(void) {
  static const char *const names[] = {
    "SNESRECOMP_TRACE_WATCH_FILE", "SNESRECOMP_TRACE_FILE", "AR_INPUT_RECORD", "AR_DRIFT_LOG",
    "AR_MX_OUT", "AR_WRAM_TRACE", "AR_SIM3D_TRACE", "AR_SIM3D_D1_TRACE",
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) RebaseBareOutput(names[i]);
  if (!s_enabled) return;
  RebaseBareOutput("AR_FRAME_STREAM_TRACE");
  RebaseBareOutput("AR_FRAME_PACING_TRACE");
  RebaseBareOutput("AR_FRAME_SYNC_TRACE");
  RebaseBareOutput("AR_GPU_PREPARE_TRACE");
}
