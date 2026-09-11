#include "host/parallel_work.h"

#include <SDL3/SDL.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Work {
  uint32_t values[17003];
  unsigned visits[17003];
  SDL_ThreadID threads[17003];
  uint32_t seed;
} Work;

static void Fill(void *context, size_t first, size_t end) {
  Work *work = context;
  for (size_t i = first; i < end; ++i) {
    work->values[i] = (uint32_t)i * UINT32_C(747796405) + work->seed;
    work->visits[i]++;
    work->threads[i] = SDL_GetCurrentThreadID();
  }
}

int main(void) {
  assert(!HostParallelWork_Create(0));
  const size_t counts[] = {0, 1, 3, 7, 2047, 2048, 4095, 4096, 17003};
  for (unsigned helpers = 0; helpers <= 3; ++helpers) {
    HostParallelWork *pool = HostParallelWork_Create(helpers);
    for (unsigned repeat = 0; repeat < 24; ++repeat) {
      for (size_t n = 0; n < sizeof(counts) / sizeof(counts[0]); ++n) {
        Work expected = {.seed = repeat}, actual = {.seed = repeat};
        HostParallelWork_Run(NULL, counts[n], 1, Fill, &expected);
        HostParallelWork_Run(pool, counts[n], repeat % 2 ? 2048 : 0, Fill, &actual);
        assert(!memcmp(expected.values, actual.values, sizeof(actual.values)));
        assert(!memcmp(expected.visits, actual.visits, sizeof(actual.visits)));
        if (pool && counts[n] == 17003) {
          bool helper_participated = false;
          for (size_t i = 0; i < counts[n]; ++i)
            helper_participated |= actual.threads[i] != SDL_GetCurrentThreadID();
          assert(helper_participated);
        }
      }
    }
    HostParallelWork_Destroy(pool);
  }
  HostParallelWork_Destroy(NULL);
  puts("parallel_work_test: PASS");
  return 0;
}
