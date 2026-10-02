#include "host/frame_producer.h"

#include <SDL3/SDL.h>
#include <string.h>

static struct {
  SDL_Thread *thread;
  SDL_Semaphore *start, *done;
  HostFrameProducerWork work, cleanup;
  void *context, *cleanup_context;
  bool pending, stop;
} s_producer;

static int Produce(void *unused) {
  (void)unused;
  for (;;) {
    SDL_WaitSemaphore(s_producer.start);
    if (s_producer.stop) break;
    s_producer.work(s_producer.context);
    SDL_SignalSemaphore(s_producer.done);
  }
  if (s_producer.cleanup) s_producer.cleanup(s_producer.cleanup_context);
  return 0;
}

bool HostFrameProducer_Init(HostFrameProducerWork cleanup, void *context) {
  if (s_producer.thread) return true;
  s_producer.cleanup = cleanup;
  s_producer.cleanup_context = context;
  s_producer.start = SDL_CreateSemaphore(0);
  s_producer.done = SDL_CreateSemaphore(0);
  if (s_producer.start && s_producer.done) {
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties) {
      SDL_SetPointerProperty(properties, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                             (void *)Produce);
      SDL_SetStringProperty(properties, SDL_PROP_THREAD_CREATE_NAME_STRING, "Frame producer");
      /* Native frame setup has a larger stack footprint than a tiny worker
       * task. Avoid platform-dependent defaults (notably macOS). */
      SDL_SetNumberProperty(properties, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER,
                            4 * 1024 * 1024);
      s_producer.thread = SDL_CreateThreadWithProperties(properties);
      SDL_DestroyProperties(properties);
    }
  }
  if (!s_producer.thread) {
    HostFrameProducer_Shutdown();
    return false;
  }
  return true;
}

bool HostFrameProducer_Enabled(void) { return s_producer.thread != NULL; }

bool HostFrameProducer_Submit(HostFrameProducerWork work, void *context) {
  if (!s_producer.thread || s_producer.pending || !work) return false;
  s_producer.work = work;
  s_producer.context = context;
  s_producer.pending = true;
  SDL_SignalSemaphore(s_producer.start);
  return true;
}

bool HostFrameProducer_Poll(void) {
  if (!s_producer.pending || !SDL_TryWaitSemaphore(s_producer.done)) return false;
  s_producer.pending = false;
  return true;
}

void HostFrameProducer_Wait(void) {
  if (!s_producer.pending) return;
  SDL_WaitSemaphore(s_producer.done);
  s_producer.pending = false;
}

void HostFrameProducer_Shutdown(void) {
  if (s_producer.thread) {
    HostFrameProducer_Wait();
    s_producer.stop = true;
    SDL_SignalSemaphore(s_producer.start);
    SDL_WaitThread(s_producer.thread, NULL);
  }
  if (s_producer.start) SDL_DestroySemaphore(s_producer.start);
  if (s_producer.done) SDL_DestroySemaphore(s_producer.done);
  memset(&s_producer, 0, sizeof(s_producer));
}
