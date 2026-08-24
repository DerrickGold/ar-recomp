#include "actraiser/actraiser_hle_fatal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

enum { kActRaiserHleFatalMessageCapacity = 1024 };

static ActRaiserHleFatalHostEscape s_host_escape;

void ActRaiserHleFatal_RegisterHostEscape(
    ActRaiserHleFatalHostEscape escape) {
  s_host_escape = escape;
}

AR_HLE_NORETURN void ActRaiserHleFatal(const char *format, ...) {
  char message[kActRaiserHleFatalMessageCapacity];
  if (!format || !format[0]) {
    snprintf(message, sizeof(message),
             "The emulated game violated an HLE runtime invariant.");
  } else {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
  }

  if (s_host_escape) s_host_escape(message);

  /* No registered host boundary means this is a standalone HLE invocation;
   * a returning escape means a fatally suspended coroutine was incorrectly
   * resumed. Neither path may continue through the invalid CPU contract. */
  fprintf(stderr, "FATAL: %s\n", message);
  abort();
}
