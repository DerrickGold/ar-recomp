#ifndef AR_RUNTIME_LOG_H
#define AR_RUNTIME_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Routine gameplay diagnostics. Standalone tools retain synchronous output.
 * The record callback receives a copy and must only read that copy and
 * immutable data. Fatal errors can still write directly to stderr. */
typedef void (*RuntimeLogFormat)(FILE *output, const void *record, size_t size);

#if defined(AR_ASYNC_RUNTIME_LOG) && AR_ASYNC_RUNTIME_LOG
void RuntimeLog_Start(void);
void RuntimeLog_Stop(void);
bool RuntimeLog_AbandonedIO(void);
void RuntimeLog_Printf(const char *format, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
void RuntimeLog_Record(RuntimeLogFormat format, const void *record, size_t size);
#else
#define RuntimeLog_Printf(...) ((void)fprintf(stderr, __VA_ARGS__))
static inline void RuntimeLog_Record(RuntimeLogFormat format, const void *record, size_t size) {
  format(stderr, record, size);
}
#endif

#endif
