#ifndef AR_TEST_CHECK_H
#define AR_TEST_CHECK_H
/* Report a failed condition and increment the caller's counter. Unlike assert,
 * this lets a suite report several independent failures before returning. */
#include <stdio.h>

#define AR_TEST_CHECK(failures, condition)                                                         \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                              \
      ++(failures);                                                                                \
    }                                                                                              \
  } while (0)

#endif
