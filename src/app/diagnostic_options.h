#ifndef AR_DIAGNOSTIC_OPTIONS_H
#define AR_DIAGNOSTIC_OPTIONS_H

#include <stdbool.h>
#include <stdlib.h>

/* Diagnostic switches are enabled by a nonempty value other than zero.
 * Callers on frame paths resolve these once, after launch config is loaded. */
static inline bool DiagnosticOptionEnabled(const char *name) {
  const char *value = getenv(name);
  return value && value[0] && value[0] != '0';
}

#endif
