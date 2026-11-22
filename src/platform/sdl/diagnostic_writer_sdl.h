#ifndef AR_DIAGNOSTIC_WRITER_SDL_H
#define AR_DIAGNOSTIC_WRITER_SDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct DiagnosticWriter DiagnosticWriter;
typedef void (*DiagnosticWrite)(FILE *output, const void *data, size_t size);
typedef bool (*DiagnosticFinalize)(void *context, bool complete);

typedef struct DiagnosticWriterResult {
  size_t dropped;
  bool failed;
  bool timed_out;
} DiagnosticWriterResult;

typedef struct DiagnosticWriterOptions {
  bool flush_when_idle;
  bool close_output;
  bool report_drops;
  long byte_limit;
  /* Runs on the worker after flushing and closing, including on abandonment.
   * The callback owns its context after successful writer creation. */
  DiagnosticFinalize finalize;
  void *context;
} DiagnosticWriterOptions;

/* With close_output, ownership transfers on successful creation. A byte
 * limit requires a seekable binary stream and is checked between records.
 * Producers copy into preallocated slots and never wait for I/O. */
DiagnosticWriter *DiagnosticWriter_Create(FILE *output, size_t capacity, size_t item_size,
                                          DiagnosticWriterOptions options);
bool DiagnosticWriter_Submit(DiagnosticWriter *writer, DiagnosticWrite write,
                             const void *data, size_t size);
void DiagnosticWriter_RequestStop(DiagnosticWriter *writer);
/* Stop producers before finishing. A borrowed stream requires timeout_ms=-1.
 * An owned stream and its queue can outlive a timed-out finish. */
DiagnosticWriterResult DiagnosticWriter_Finish(DiagnosticWriter *writer, int timeout_ms);
/* CRT process exit can wait on a FILE still held by an abandoned worker. */
bool DiagnosticWriter_AbandonedIO(void);

#endif
