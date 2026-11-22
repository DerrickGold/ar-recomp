#include "app/runtime_log.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define DuplicateFd _dup
#define FileFd _fileno
#define OpenFd _fdopen
#define CloseFd _close
#else
#include <unistd.h>
#define DuplicateFd dup
#define FileFd fileno
#define OpenFd fdopen
#define CloseFd close
#endif

#include "app/diagnostic_options.h"
#include "app/run_dir.h"
#include "platform/sdl/diagnostic_writer_sdl.h"

enum {
  kLineCapacity = 4096,
  kRecordCapacity = 8192,
  kDrainMs = 250,
  /* Leave room for the final formatted report and the limit marker. */
  kAutomaticLogLimit = 16 * 1024 * 1024 - 64 * 1024,
};

typedef struct SdlLogRecord {
  int category;
  SDL_LogPriority priority;
  char message[kLineCapacity];
} SdlLogRecord;

static DiagnosticWriter *s_writer, *s_capture;
static SDL_LogOutputFunction s_previous_log;
static void *s_previous_context;
static bool s_started, s_sync;
static DiagnosticWrite s_write_sdl;

static void WriteText(FILE *output, const void *data, size_t size) {
  fwrite(data, 1, size, output);
}

static void WriteSdlLog(FILE *output, const void *data, size_t size) {
  (void)size;
  const SdlLogRecord *log = data;
  fflush(output);
  if (s_previous_log) s_previous_log(s_previous_context, log->category, log->priority, log->message);
}

static void WriteSdlText(FILE *output, const void *data, size_t size) {
  (void)size;
  const SdlLogRecord *log = data;
  fprintf(output, "%s\n", log->message);
}

static void SDLCALL CaptureSdlLog(void *context, int category,
                                  SDL_LogPriority priority, const char *message) {
  (void)context;
  if (s_sync && s_previous_log)
    s_previous_log(s_previous_context, category, priority, message);
  if (!s_writer && !s_capture) return;
  SdlLogRecord record = {.category = category, .priority = priority};
  const size_t length = SDL_strlcpy(record.message, message, sizeof(record.message));
  const size_t size = length < sizeof(record.message) ? length + 1 : sizeof(record.message);
  if (length >= sizeof(record.message))
    memcpy(record.message + sizeof(record.message) - sizeof("..."), "...", sizeof("..."));
  const size_t bytes = offsetof(SdlLogRecord, message) + size;
  if (s_writer)
    (void)DiagnosticWriter_Submit(s_writer, s_write_sdl, &record, bytes);
  if (s_capture) (void)DiagnosticWriter_Submit(s_capture, WriteSdlText, &record, bytes);
}

static DiagnosticWriter *StartWriter(FILE *output, long byte_limit) {
  if (!output) return NULL;
  DiagnosticWriter *writer = DiagnosticWriter_Create(output, 256, kRecordCapacity,
      (DiagnosticWriterOptions){
        .flush_when_idle = true, .close_output = true, .report_drops = true, .byte_limit = byte_limit,
      });
  if (!writer) fclose(output);
  return writer;
}

static FILE *DuplicateStderr(void) {
  const int original = FileFd(stderr);
  const int fd = original >= 0 ? DuplicateFd(original) : -1;
  if (fd < 0) return NULL;
  FILE *output = OpenFd(fd, "wb");
  if (!output) CloseFd(fd);
  return output;
}

void RuntimeLog_Start(void) {
  if (s_started) return;
  s_started = true;
  s_sync = DiagnosticOptionEnabled("AR_SYNC_RUNTIME_LOG");
  const bool managed_stderr = DiagnosticOptionEnabled("AR_MANAGED_LAUNCH_LOG");
  s_write_sdl = managed_stderr ? WriteSdlText : WriteSdlLog;
  if (!s_sync)
    s_writer = StartWriter(DuplicateStderr(), managed_stderr ? kAutomaticLogLimit : 0);
  s_capture = StartWriter(RunDirOpenRuntimeLog(), RunDirIsTemporary() ? kAutomaticLogLimit : 0);

  SDL_GetLogOutputFunction(&s_previous_log, &s_previous_context);
  SDL_SetLogOutputFunction(CaptureSdlLog, NULL);
}

void RuntimeLog_Stop(void) {
  if (!s_started) return;
  /* SDL serializes callbacks with replacement. Game-owned producers have
   * already stopped before normal shutdown. */
  SDL_SetLogOutputFunction(s_previous_log, s_previous_context);
  const Uint64 deadline = SDL_GetTicks() + kDrainMs;
  DiagnosticWriter_RequestStop(s_capture);
  DiagnosticWriter_RequestStop(s_writer);
  DiagnosticWriter_Finish(s_capture, kDrainMs);
  s_capture = NULL;
  const Uint64 now = SDL_GetTicks();
  DiagnosticWriter_Finish(s_writer, now < deadline ? (int)(deadline - now) : 0);
  s_writer = NULL;
}

bool RuntimeLog_AbandonedIO(void) { return DiagnosticWriter_AbandonedIO(); }

void RuntimeLog_Record(RuntimeLogFormat format, const void *record, size_t size) {
  if (s_capture) (void)DiagnosticWriter_Submit(s_capture, format, record, size);
  if (s_writer) (void)DiagnosticWriter_Submit(s_writer, format, record, size);
  else if (!s_started || s_sync) format(stderr, record, size);
}

void RuntimeLog_Printf(const char *format, ...) {
  if (s_started && !s_sync && !s_writer && !s_capture) return;
  va_list args;
  va_start(args, format);
  if (!s_started || (!s_capture && s_sync)) {
    vfprintf(stderr, format, args);
  } else {
    char text[kLineCapacity];
    const int length = vsnprintf(text, sizeof(text), format, args);
    if (length > 0) {
      if ((size_t)length >= sizeof(text)) {
        static const char truncated[] = " [truncated]\n";
        memcpy(text + sizeof(text) - sizeof(truncated), truncated, sizeof(truncated));
      }
      const size_t size = (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1;
      RuntimeLog_Record(WriteText, text, size);
    }
  }
  va_end(args);
}
