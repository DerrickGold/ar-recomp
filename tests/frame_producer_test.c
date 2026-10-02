#include "host/frame_producer.h"
#include "support/test_assert.h"
#include <SDL3/SDL.h>
#include <stdio.h>

typedef struct Work {
  SDL_Semaphore *entered, *release;
  SDL_ThreadID worker, cleanup_thread;
  unsigned completed;
  uint64_t started_ns;
} Work;

static void Produce(void *context) {
  Work *work = context;
  work->worker = SDL_GetCurrentThreadID();
  work->started_ns = SDL_GetTicksNS();
  if (work->entered) {
    SDL_SignalSemaphore(work->entered);
    assert(SDL_WaitSemaphoreTimeout(work->release, 2000));
  }
  work->completed++;
}

static void Cleanup(void *context) {
  Work *work = context;
  work->cleanup_thread = SDL_GetCurrentThreadID();
}

int main(void) {
  assert(!HostFrameProducer_Submit(Produce, NULL));
  assert(!HostFrameProducer_Poll());
  HostFrameProducer_Wait();
  for (unsigned lifetime = 0; lifetime < 3; ++lifetime) {
    Work work = {0};
    assert(HostFrameProducer_Init(Cleanup, &work));
    assert(!HostFrameProducer_Submit(NULL, NULL));
    work.entered = SDL_CreateSemaphore(0);
    work.release = SDL_CreateSemaphore(0);
    assert(work.entered && work.release);
    assert(HostFrameProducer_Submit(Produce, &work));
    assert(SDL_WaitSemaphoreTimeout(work.entered, 2000));
    assert(work.worker != SDL_GetCurrentThreadID());
    assert(!HostFrameProducer_Poll());
    assert(!HostFrameProducer_Submit(Produce, &work));
    SDL_SignalSemaphore(work.release);
    HostFrameProducer_Wait();
    assert(work.completed == 1);
    assert(!HostFrameProducer_Poll());
    SDL_DestroySemaphore(work.entered);
    SDL_DestroySemaphore(work.release);
    work.entered = work.release = NULL;
    const SDL_ThreadID owner = work.worker;
    for (unsigned i = 0; i < 64; ++i) {
      assert(HostFrameProducer_Submit(Produce, &work));
      assert(!HostFrameProducer_Submit(Produce, &work));
      if (i & 1) {
        const uint64_t timeout = SDL_GetTicksNS() + 2000000000;
        while (!HostFrameProducer_Poll()) {
          assert(SDL_GetTicksNS() < timeout);
          SDL_Delay(1);
        }
      } else HostFrameProducer_Wait();
      assert(work.worker == owner);
      assert(work.completed == i + 2);
    }
    assert(HostFrameProducer_Submit(Produce, &work));
    HostFrameProducer_Shutdown(); /* Drains the job, then cleans up on its owner. */
    assert(work.completed == 66 && work.cleanup_thread == owner);
    assert(!HostFrameProducer_Enabled());
    HostFrameProducer_Shutdown();
  }
  puts("frame_producer_test: PASS");
  return 0;
}
