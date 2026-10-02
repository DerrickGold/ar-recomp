#include "app/session_fatal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdatomic.h>

enum { kSessionFatalMessageCapacity = 1024 };

/* 0 empty, 1 first writer owns the payload, 2 published. */
static atomic_int s_state;
static char s_message[kSessionFatalMessageCapacity];
static SessionFailureKind s_kind;

static void Request(SessionFailureKind kind, const char *format, va_list arguments) {
  int expected = 0;
  if (!atomic_compare_exchange_strong_explicit(&s_state, &expected, 1,
          memory_order_acq_rel, memory_order_acquire)) return;
  s_kind = kind;

  if (!format || !format[0]) {
    snprintf(s_message, sizeof(s_message),
             "The game encountered an unrecoverable runtime error.");
  } else {
    vsnprintf(s_message, sizeof(s_message), format, arguments);
  }
  fprintf(stderr, "[fatal-session] %s\n", s_message);
  atomic_store_explicit(&s_state, 2, memory_order_release);
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

SessionFailureKind SessionFatal_Kind(void) {
  return atomic_load_explicit(&s_state, memory_order_acquire) == 2
      ? s_kind : kSessionFailure_Generic;
}

bool SessionFatal_Requested(void) {
  return atomic_load_explicit(&s_state, memory_order_acquire) != 0;
}

const char *SessionFatal_Message(void) {
  return atomic_load_explicit(&s_state, memory_order_acquire) == 2 ? s_message : "";
}
