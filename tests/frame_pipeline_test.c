#include "host/frame_producer.h"
#include "host/frame_queue.h"
#include "support/test_assert.h"
#include <SDL3/SDL.h>
#include <stdio.h>

typedef struct Batch {
  HostFrameQueue *queue;
  SDL_ThreadID owner, cleanup_owner;
  unsigned produced;
} Batch;

static void Produce(void *context) {
  Batch *batch = context;
  batch->owner = SDL_GetCurrentThreadID();
  while (!HostFrameQueue_PauseRequested(batch->queue)) {
    HostFramePacket *packet = HostFrameQueue_BeginWrite(batch->queue);
    if (!packet) { SDL_Delay(1); continue; }
    packet->tick = (int)++batch->produced;
    packet->pixels[0] = (uint8_t)packet->tick;
    HostFrameQueue_Publish(batch->queue);
  }
}

static void Cleanup(void *context) {
  ((Batch *)context)->cleanup_owner = SDL_GetCurrentThreadID();
}

int main(void) {
  Batch batch = {.queue = HostFrameQueue_Create()};
  assert(batch.queue && HostFrameProducer_Init(Cleanup, &batch));
  SDL_ThreadID owner = 0;
  for (unsigned i = 0; i < 16; ++i) {
    HostFrameQueue_Resume(batch.queue);
    assert(HostFrameProducer_Submit(Produce, &batch));
    uint64_t deadline = SDL_GetTicksNS() + 2000000000;
    while (HostFrameQueue_ReadyCount(batch.queue) != 3) {
      assert(SDL_GetTicksNS() < deadline);
      SDL_Delay(1);
    }
    const HostFramePacket *held = HostFrameQueue_Read(batch.queue);
    const int oldest = held->tick;
    /* A full queue and a held reader must never prevent pause/teardown. */
    HostFrameQueue_RequestPause(batch.queue);
    while (!HostFrameProducer_Poll()) {
      assert(SDL_GetTicksNS() < deadline);
      SDL_Delay(1);
    }
    assert(held->tick == oldest && held->pixels[0] == (uint8_t)oldest);
    assert(batch.owner != SDL_GetCurrentThreadID());
    if (!owner) owner = batch.owner;
    assert(batch.owner == owner);
    assert(batch.produced == (i + 1) * 3);
    /* Latest-frame playback discards two stale images, never the game ticks. */
    HostFrameQueue_Release(batch.queue);
    HostFrameQueue_Release(batch.queue);
    assert(HostFrameQueue_Read(batch.queue)->tick == oldest + 2);
    HostFrameQueue_Release(batch.queue);
    assert(!HostFrameQueue_Read(batch.queue));
  }
  HostFrameProducer_Shutdown();
  assert(batch.cleanup_owner == owner);
  HostFrameQueue_Destroy(batch.queue);
  puts("frame_pipeline_test: PASS");
  return 0;
}
