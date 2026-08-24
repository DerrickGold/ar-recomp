#include "session_fatal.h"

#include <stdarg.h>
#include <stdio.h>

enum { kSessionFatalMessageCapacity = 1024 };

static bool s_requested;
static char s_message[kSessionFatalMessageCapacity];

void SessionFatal_Request(const char *format, ...) {
  if (s_requested) return;
  s_requested = true;

  if (!format || !format[0]) {
    snprintf(s_message, sizeof(s_message),
             "The game encountered an unrecoverable runtime error.");
  } else {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(s_message, sizeof(s_message), format, arguments);
    va_end(arguments);
  }
  fprintf(stderr, "[fatal-session] %s\n", s_message);
}

bool SessionFatal_Requested(void) {
  return s_requested;
}

const char *SessionFatal_Message(void) {
  return s_requested ? s_message : "";
}
