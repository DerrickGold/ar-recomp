#include "session_fatal.h"

#include <stdarg.h>
#include <stdio.h>

enum { kSessionFatalMessageCapacity = 1024 };

static bool s_requested;
static char s_message[kSessionFatalMessageCapacity];
static SessionFailureKind s_kind;

static void Request(SessionFailureKind kind, const char *format, va_list arguments) {
  if (s_requested) return;
  s_requested = true;
  s_kind = kind;

  if (!format || !format[0]) {
    snprintf(s_message, sizeof(s_message),
             "The game encountered an unrecoverable runtime error.");
  } else {
    vsnprintf(s_message, sizeof(s_message), format, arguments);
  }
  fprintf(stderr, "[fatal-session] %s\n", s_message);
}

void SessionFatal_Request(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  Request(kSessionFailure_Generic, format, arguments);
  va_end(arguments);
}

void SessionFatal_RequestKind(SessionFailureKind kind, const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  Request(kind, format, arguments);
  va_end(arguments);
}

SessionFailureKind SessionFatal_Kind(void) { return s_kind; }

bool SessionFatal_Requested(void) {
  return s_requested;
}

const char *SessionFatal_Message(void) {
  return s_requested ? s_message : "";
}
