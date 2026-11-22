#include "platform/sdl/diagnostic_writer_sdl.h"

#include <SDL3/SDL.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#endif

typedef struct DiagnosticSlot {
  _Alignas(max_align_t) atomic_size_t sequence;
  DiagnosticWrite write;
  size_t size;
} DiagnosticSlot;

struct DiagnosticWriter {
  SDL_Thread *thread;
  SDL_Semaphore *ready, *finished;
  FILE *output;
  unsigned char *slots;
  size_t capacity, stride, item_size;
  atomic_size_t tail, dropped;
  atomic_bool stopping, wake_pending, abandoned;
  atomic_int owners;
  DiagnosticWriterOptions options;
  bool failed;
};

static atomic_bool s_abandoned_io;

bool DiagnosticWriter_AbandonedIO(void) {
  return atomic_load_explicit(&s_abandoned_io, memory_order_relaxed);
}

static DiagnosticSlot *SlotAt(DiagnosticWriter *writer, size_t position) {
  return (void *)(writer->slots + (position & (writer->capacity - 1)) * writer->stride);
}

static void ReleaseWriter(DiagnosticWriter *writer) {
  if (atomic_fetch_sub_explicit(&writer->owners, 1, memory_order_acq_rel) != 1) return;
  SDL_DestroySemaphore(writer->ready);
  SDL_DestroySemaphore(writer->finished);
  free(writer->slots);
  free(writer);
}

static void WakeWriter(DiagnosticWriter *writer) {
  if (!atomic_exchange_explicit(&writer->wake_pending, true, memory_order_acq_rel))
    SDL_SignalSemaphore(writer->ready);
}

static int SDLCALL RunWriter(void *context) {
  DiagnosticWriter *writer = context;
#ifndef _WIN32
  /* A downstream console pipe can disappear while gameplay continues. */
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &signals, NULL);
#endif
  size_t head = 0;
  bool pending_flush = false, limited = false;
  for (;;) {
    DiagnosticSlot *slot = SlotAt(writer, head);
    if (atomic_load_explicit(&slot->sequence, memory_order_acquire) == head + 1) {
      if (writer->options.byte_limit && !writer->failed && !limited) {
        const long position = ftell(writer->output);
        writer->failed = position < 0;
        limited = position >= writer->options.byte_limit;
        if (limited) {
          fputs("[diagnostics] automatic log size limit reached\n", writer->output);
          pending_flush = true;
        }
      }
      if (limited || writer->failed) {
        atomic_fetch_add_explicit(&writer->dropped, 1, memory_order_relaxed);
      } else {
        slot->write(writer->output, slot + 1, slot->size);
        writer->failed = writer->output && ferror(writer->output);
        pending_flush = true;
      }
      atomic_store_explicit(&slot->sequence, head + writer->capacity, memory_order_release);
      ++head;
      continue;
    }
    if (pending_flush && writer->options.flush_when_idle && writer->output) {
      writer->failed |= fflush(writer->output) == EOF;
      pending_flush = false;
    }
    if (atomic_load_explicit(&writer->stopping, memory_order_acquire) &&
        head == atomic_load_explicit(&writer->tail, memory_order_relaxed))
      break;
    /* Drain an earlier wake and recheck publication after arming the next
     * wake. A burst of records needs only one semaphore signal. */
    SDL_TryWaitSemaphore(writer->ready);
    atomic_exchange_explicit(&writer->wake_pending, false, memory_order_acq_rel);
    if (atomic_load_explicit(&slot->sequence, memory_order_acquire) != head + 1 &&
        !atomic_load_explicit(&writer->stopping, memory_order_acquire))
      SDL_WaitSemaphore(writer->ready);
  }
  const size_t dropped = atomic_load_explicit(&writer->dropped, memory_order_relaxed);
  if (writer->output) {
    if (writer->options.report_drops && dropped && !limited && !writer->failed)
      fprintf(writer->output, "[diagnostics] dropped=%zu\n", dropped);
    writer->failed |= fflush(writer->output) == EOF || ferror(writer->output);
    if (writer->options.close_output) writer->failed |= fclose(writer->output) == EOF;
  }
  if (writer->options.finalize) {
    const bool complete = !dropped && !writer->failed &&
        !atomic_load_explicit(&writer->abandoned, memory_order_acquire);
    writer->failed |= !writer->options.finalize(writer->options.context, complete);
  }
  SDL_SignalSemaphore(writer->finished);
  ReleaseWriter(writer);
  return 0;
}

DiagnosticWriter *DiagnosticWriter_Create(FILE *output, size_t capacity, size_t item_size,
                                          DiagnosticWriterOptions options) {
  const size_t alignment = _Alignof(max_align_t);
  if (capacity < 2 || (capacity & (capacity - 1)) || !item_size ||
      item_size > SIZE_MAX - sizeof(DiagnosticSlot) - alignment)
    return NULL;
  const size_t stride = sizeof(DiagnosticSlot) + (item_size + alignment - 1) / alignment * alignment;
  if (capacity > SIZE_MAX / stride) return NULL;
  DiagnosticWriter *writer = calloc(1, sizeof(*writer));
  if (!writer) return NULL;
  writer->output = output;
  writer->options = options;
  writer->capacity = capacity;
  writer->stride = stride;
  writer->item_size = item_size;
  atomic_init(&writer->tail, 0);
  atomic_init(&writer->dropped, 0);
  atomic_init(&writer->stopping, false);
  atomic_init(&writer->wake_pending, false);
  atomic_init(&writer->abandoned, false);
  atomic_init(&writer->owners, 2);
  writer->slots = malloc(capacity * stride);
  writer->ready = SDL_CreateSemaphore(0);
  writer->finished = SDL_CreateSemaphore(0);
  if (writer->slots && writer->ready && writer->finished) {
    for (size_t i = 0; i < capacity; ++i)
      atomic_init(&SlotAt(writer, i)->sequence, i);
    writer->thread = SDL_CreateThread(RunWriter, "diagnostic-writer", writer);
  }
  if (writer->thread) return writer;
  SDL_DestroySemaphore(writer->ready);
  SDL_DestroySemaphore(writer->finished);
  free(writer->slots);
  free(writer);
  return NULL;
}

bool DiagnosticWriter_Submit(DiagnosticWriter *writer, DiagnosticWrite write,
                             const void *data, size_t size) {
  size_t position = atomic_load_explicit(&writer->tail, memory_order_relaxed);
  if (size <= writer->item_size) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      DiagnosticSlot *slot = SlotAt(writer, position);
      const size_t sequence = atomic_load_explicit(&slot->sequence, memory_order_acquire);
      if (sequence == position) {
        if (!atomic_compare_exchange_weak_explicit(&writer->tail, &position, position + 1,
                                                   memory_order_relaxed, memory_order_relaxed))
          continue;
        slot->write = write;
        slot->size = size;
        memcpy(slot + 1, data, size);
        atomic_store_explicit(&slot->sequence, position + 1, memory_order_release);
        WakeWriter(writer);
        return true;
      }
      if (sequence - position > SIZE_MAX / 2) break; /* slot still owned by reader */
      position = atomic_load_explicit(&writer->tail, memory_order_relaxed);
    }
  }
  atomic_fetch_add_explicit(&writer->dropped, 1, memory_order_relaxed);
  return false;
}

void DiagnosticWriter_RequestStop(DiagnosticWriter *writer) {
  if (writer && !atomic_exchange_explicit(&writer->stopping, true, memory_order_acq_rel))
    WakeWriter(writer);
}

DiagnosticWriterResult DiagnosticWriter_Finish(DiagnosticWriter *writer, int timeout_ms) {
  if (!writer) return (DiagnosticWriterResult){0};
  DiagnosticWriter_RequestStop(writer);
  if (writer->options.close_output && timeout_ms >= 0 &&
      !SDL_WaitSemaphoreTimeout(writer->finished, timeout_ms)) {
    atomic_store_explicit(&s_abandoned_io, true, memory_order_relaxed);
    atomic_store_explicit(&writer->abandoned, true, memory_order_release);
    const DiagnosticWriterResult result = {
      .dropped = atomic_load_explicit(&writer->dropped, memory_order_relaxed),
      .timed_out = true,
    };
    SDL_DetachThread(writer->thread);
    ReleaseWriter(writer);
    return result;
  }
  SDL_WaitThread(writer->thread, NULL);
  const DiagnosticWriterResult result = {
    .dropped = atomic_load_explicit(&writer->dropped, memory_order_relaxed),
    .failed = writer->failed,
  };
  ReleaseWriter(writer);
  return result;
}
