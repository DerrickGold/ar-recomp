#ifndef AR_TEST_ASSERT_H
#define AR_TEST_ASSERT_H
/* Test assertions always evaluate, including in Release configurations.
 * Keep setup calls inside existing assertions active without changing the
 * production translation units' NDEBUG policy. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#endif
