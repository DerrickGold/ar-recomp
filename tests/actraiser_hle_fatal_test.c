#include "actraiser/actraiser_hle_fatal.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf s_escape;
static char s_message[256];

static void CaptureAndEscape(const char *message) {
  snprintf(s_message, sizeof(s_message), "%s", message ? message : "");
  longjmp(s_escape, 1);
}

int main(void) {
  ActRaiserHleFatal_RegisterHostEscape(CaptureAndEscape);
  if (setjmp(s_escape) == 0)
    ActRaiserHleFatal("routine $%02X violated %s", 0x42, "entry mode");

  ActRaiserHleFatal_RegisterHostEscape(NULL);
  if (strcmp(s_message, "routine $42 violated entry mode") != 0) {
    fprintf(stderr, "actraiser_hle_fatal_test: unexpected message: %s\n",
            s_message);
    return 1;
  }

  puts("actraiser_hle_fatal_test: PASS");
  return 0;
}
