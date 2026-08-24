#include "session_fatal.h"

#include <stdio.h>
#include <string.h>

int main(void) {
  if (SessionFatal_Requested()) return 1;
  if (SessionFatal_Message()[0]) return 1;

  SessionFatal_Request("renderer contract %d failed", 7);
  if (!SessionFatal_Requested()) return 1;
  if (strcmp(SessionFatal_Message(), "renderer contract 7 failed")) return 1;

  /* The initiating error remains actionable even if a later layer notices the
   * same shutdown while unwinding. */
  SessionFatal_Request("secondary failure");
  if (strcmp(SessionFatal_Message(), "renderer contract 7 failed")) return 1;

  puts("session_fatal_test: PASS");
  return 0;
}
