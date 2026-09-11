#include "host/parallel_work.h"
#include "performance_metrics.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdlib.h>

enum { kMaximumHelpers = 3 };

typedef struct ParallelHelper {
  SDL_Thread *thread;
  SDL_Semaphore *start;
  SDL_Semaphore *finished;
  HostParallelWorkRange range;
  void *context;
  size_t first, end;
  bool stop;
} ParallelHelper;

struct HostParallelWork {
  ParallelHelper helpers[kMaximumHelpers];
  SDL_Semaphore *finished;
  unsigned count;
};

static int SDLCALL RunHelper(void *context) {
  ParallelHelper *helper = context;
  for (;;) {
    /* Semaphore publication owns both the job payload and its output. No
     * producer modifies either until the matching completion has arrived. */
    SDL_WaitSemaphore(helper->start);
    if (helper->stop) return 0;
    const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_WorkHelpers);
    helper->range(helper->context, helper->first, helper->end);
    PerformanceMetrics_End(performance);
    SDL_SignalSemaphore(helper->finished);
  }
}

void HostParallelWork_Destroy(HostParallelWork *work) {
  if (!work) return;
  for (unsigned i = 0; i < kMaximumHelpers; ++i) {
    ParallelHelper *helper = &work->helpers[i];
    if (helper->thread) {
      helper->stop = true;
      SDL_SignalSemaphore(helper->start);
      SDL_WaitThread(helper->thread, NULL);
    }
    if (helper->start) SDL_DestroySemaphore(helper->start);
  }
  if (work->finished) SDL_DestroySemaphore(work->finished);
  free(work);
}

HostParallelWork *HostParallelWork_Create(unsigned maximum_helpers) {
  unsigned helpers = maximum_helpers < kMaximumHelpers
      ? maximum_helpers : kMaximumHelpers;
  const char *override = SDL_getenv("AR_RENDER_WORKERS");
  if (override && override[0] >= '0' && override[0] <= '3' && !override[1]) {
    const unsigned requested = (unsigned)(override[0] - '0');
    if (helpers > requested) helpers = requested;
  }
  const int cores = SDL_GetNumLogicalCPUCores();
  if (cores < 2) return NULL;
  if (helpers > (unsigned)(cores - 1)) helpers = (unsigned)(cores - 1);
  if (!helpers) return NULL;
  HostParallelWork *work = calloc(1, sizeof(*work));
  if (!work) return NULL;
  work->finished = SDL_CreateSemaphore(0);
  if (!work->finished) goto unavailable;
  for (unsigned i = 0; i < helpers; ++i) {
    ParallelHelper *helper = &work->helpers[i];
    helper->finished = work->finished;
    helper->start = SDL_CreateSemaphore(0);
    if (!helper->start) goto unavailable;
    helper->thread = SDL_CreateThread(RunHelper, "render-math", helper);
    if (!helper->thread) goto unavailable;
    work->count++;
  }
  return work;
unavailable:
  HostParallelWork_Destroy(work);
  return NULL;
}

void HostParallelWork_Run(HostParallelWork *work, size_t count,
    size_t minimum_per_part, HostParallelWorkRange range, void *context) {
  if (!count || !range) return;
  PerformanceMetrics_Add(kPerformanceCount_WorkJobs, 1);
  if (!minimum_per_part) minimum_per_part = 1;
  size_t parts = count / minimum_per_part;
  const size_t available = work ? (size_t)work->count + 1 : 1;
  if (parts > available) parts = available;
  if (parts < 2) {
    const PerformanceScope performance = PerformanceMetrics_Begin(kPerformance_WorkOwner);
    range(context, 0, count);
    PerformanceMetrics_End(performance);
    return;
  }
  const size_t quotient = count / parts, remainder = count % parts;
  PerformanceMetrics_Add(kPerformanceCount_HelperJobs, parts - 1);
  const size_t owner_end = quotient + (remainder != 0);
  size_t first = owner_end;
  for (size_t i = 1; i < parts; ++i) {
    ParallelHelper *helper = &work->helpers[i - 1];
    helper->range = range;
    helper->context = context;
    helper->first = first;
    first += quotient + (i < remainder);
    helper->end = first;
    SDL_SignalSemaphore(helper->start);
  }
  const PerformanceScope owner = PerformanceMetrics_Begin(kPerformance_WorkOwner);
  range(context, 0, owner_end);
  PerformanceMetrics_End(owner);
  const PerformanceScope join = PerformanceMetrics_Begin(kPerformance_WorkJoin);
  for (size_t i = 1; i < parts; ++i) SDL_WaitSemaphore(work->finished);
  PerformanceMetrics_End(join);
}
