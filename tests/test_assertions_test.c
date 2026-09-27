/* Built with NDEBUG explicitly set: test setup and checks must still run. */
#ifndef NDEBUG
#error This probe must exercise a build with NDEBUG enabled
#endif
#include "support/test_assert.h"

int main(void) {
  int calls = 0;
  assert(++calls == 1);
  /* This return deliberately does not use assert, so a disabled assertion
   * fails the probe instead of silently disabling its own verification. */
  return calls == 1 ? 0 : 1;
}
