#include "platform/sdl/diagnostic_writer_sdl.h"
#include "app/frame_trace.h"
#include "host/atomic_replace.h"
#include "snesrecomp/support/utf8_fs.h"
#include "support/test_assert.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct BlockedRecord {
  unsigned value;
  SDL_Semaphore *entered, *release;
} BlockedRecord;

static void WriteBlockedRecord(FILE *output, const void *data, size_t size) {
  const BlockedRecord *record = data;
  assert(size == sizeof(*record));
  if (record->entered) {
    SDL_SignalSemaphore(record->entered);
    SDL_WaitSemaphore(record->release);
  }
  assert(fwrite(&record->value, sizeof(record->value), 1, output) == 1);
}

static void CheckBlockedSink(void) {
  SDL_Semaphore *entered = SDL_CreateSemaphore(0), *release = SDL_CreateSemaphore(0);
  FILE *output = tmpfile();
  assert(output && entered && release);
  DiagnosticWriter *writer = DiagnosticWriter_Create(output, 4, sizeof(BlockedRecord),
      (DiagnosticWriterOptions){.flush_when_idle = true});
  assert(writer);
  BlockedRecord first = {0, entered, release};
  assert(DiagnosticWriter_Submit(writer, WriteBlockedRecord, &first, sizeof(first)));
  assert(SDL_WaitSemaphoreTimeout(entered, 2000));
  first.value = 100; /* Worker owns the copied record. */
  for (unsigned i = 1; i <= 5; ++i) {
    const BlockedRecord record = {.value = i};
    assert(DiagnosticWriter_Submit(writer, WriteBlockedRecord, &record, sizeof(record)) == (i < 4));
  }
  SDL_SignalSemaphore(release);
  const DiagnosticWriterResult result = DiagnosticWriter_Finish(writer, -1);
  assert(result.dropped == 2 && !result.failed);
  rewind(output);
  for (unsigned i = 0; i < 4; ++i) {
    unsigned value = 99;
    assert(fread(&value, sizeof(value), 1, output) == 1 && value == i);
  }
  assert(fgetc(output) == EOF);
  fclose(output);
  SDL_DestroySemaphore(entered);
  SDL_DestroySemaphore(release);
}

enum { kRecordsPerProducer = 2048 };
typedef struct Producer {
  DiagnosticWriter *writer;
  unsigned id, accepted;
} Producer;

static void WriteId(FILE *output, const void *data, size_t size) {
  assert(size == sizeof(unsigned) * 2);
  assert(fwrite(data, 1, size, output) == size);
}

static int SDLCALL SubmitRecords(void *context) {
  Producer *producer = context;
  for (unsigned i = 0; i < kRecordsPerProducer; ++i) {
    const unsigned record[] = {producer->id, i};
    producer->accepted += DiagnosticWriter_Submit(producer->writer, WriteId, record, sizeof(record));
  }
  return 0;
}

static void CheckConcurrentProducers(void) {
  FILE *output = tmpfile();
  assert(output);
  DiagnosticWriter *writer = DiagnosticWriter_Create(output, 8192, sizeof(unsigned) * 2,
      (DiagnosticWriterOptions){0});
  assert(writer);
  Producer producers[] = {{writer, 0, 0}, {writer, 1, 0}};
  SDL_Thread *a = SDL_CreateThread(SubmitRecords, "first-producer", &producers[0]);
  SDL_Thread *b = SDL_CreateThread(SubmitRecords, "second-producer", &producers[1]);
  assert(a && b);
  SDL_WaitThread(a, NULL);
  SDL_WaitThread(b, NULL);
  const DiagnosticWriterResult result = DiagnosticWriter_Finish(writer, -1);
  assert(!result.failed);
  assert(result.dropped + producers[0].accepted + producers[1].accepted == 2 * kRecordsPerProducer);
  rewind(output);
  unsigned count[2] = {0}, next[2] = {0};
  unsigned record[2];
  while (fread(record, sizeof(record), 1, output) == 1) {
    assert(record[0] < 2 && record[1] >= next[record[0]] && record[1] < kRecordsPerProducer);
    next[record[0]] = record[1] + 1;
    ++count[record[0]];
  }
  assert(count[0] && count[1] && count[0] == producers[0].accepted && count[1] == producers[1].accepted);
  fclose(output);
}

static void WriteBytes(FILE *output, const void *data, size_t size) {
  assert(fwrite(data, 1, size, output) == size);
}

static void CheckByteLimit(void) {
  FILE *output = tmpfile();
  assert(output);
  DiagnosticWriter *writer = DiagnosticWriter_Create(output, 16, 8,
      (DiagnosticWriterOptions){.byte_limit = 16});
  assert(writer);
  for (unsigned i = 0; i < 4; ++i)
    assert(DiagnosticWriter_Submit(writer, WriteBytes, "payload\n", 8));
  const DiagnosticWriterResult result = DiagnosticWriter_Finish(writer, -1);
  assert(result.dropped == 2 && !result.failed && !result.timed_out);
  rewind(output);
  char text[128] = {0};
  assert(fread(text, 1, sizeof(text) - 1, output) > 16);
  assert(!strcmp(text, "payload\npayload\n[diagnostics] automatic log size limit reached\n"));
  fclose(output);
}

static void WriteStalled(FILE *output, const void *data, size_t size) {
  const BlockedRecord *record = data;
  WriteBlockedRecord(output, data, size);
  SDL_SignalSemaphore(record->entered);
}

static void CheckTimedFinish(void) {
  SDL_Semaphore *entered = SDL_CreateSemaphore(0), *release = SDL_CreateSemaphore(0);
  FILE *output = tmpfile();
  assert(output && entered && release);
  DiagnosticWriter *writer = DiagnosticWriter_Create(output, 4, sizeof(BlockedRecord),
      (DiagnosticWriterOptions){.close_output = true});
  assert(writer);
  const BlockedRecord record = {3, entered, release};
  assert(DiagnosticWriter_Submit(writer, WriteStalled, &record, sizeof(record)));
  assert(SDL_WaitSemaphoreTimeout(entered, 2000));
  const DiagnosticWriterResult result = DiagnosticWriter_Finish(writer, 20);
  assert(result.timed_out && DiagnosticWriter_AbandonedIO());
  SDL_SignalSemaphore(release);
  assert(SDL_WaitSemaphoreTimeout(entered, 2000));
  SDL_DestroySemaphore(entered);
  SDL_DestroySemaphore(release);
}

static void CheckTraceCollision(const char *directory) {
  assert(SDL_CreateDirectory(directory));
  char path[1024];
  assert(snprintf(path, sizeof(path), "%s/shared.csv", directory) < (int)sizeof(path));
  char partial[1024];
  assert(snprintf(partial, sizeof(partial), "%s.partial", path) < (int)sizeof(partial));
  FILE *abandoned = sr_fopen(partial, "wbx");
  assert(abandoned && fputs("interrupted capture\n", abandoned) >= 0 && fclose(abandoned) == 0);
  FrameTrace *incomplete = FrameTrace_Open(path, kFrameTrace_Stream);
  assert(incomplete);
  const unsigned char oversized[sizeof(FrameStreamTraceRow) + 1] = {0};
  FrameTrace_Record(incomplete, oversized, sizeof(oversized));
  FrameTrace_CloseAll(&incomplete, 1);
  FrameTrace *stream = FrameTrace_Open(path, kFrameTrace_Stream);
  assert(stream);
  assert(!FrameTrace_Open(path, kFrameTrace_Pacing));
  char alias[1024];
  assert(snprintf(alias, sizeof(alias), "%s/./shared.csv", directory) < (int)sizeof(alias));
  assert(!FrameTrace_Open(alias, kFrameTrace_Pacing));
  const FrameStreamTraceRow row = {.start_ns = 123, .complete_ns = 456};
  FrameTrace_Record(stream, &row, sizeof(row));
  FrameTrace_CloseAll(&stream, 1);

  FILE *output = sr_fopen(path, "rb");
  assert(output);
  char line[256];
  assert(fgets(line, sizeof(line), output) && !strncmp(line, "start_ns,complete_ns,", 21));
  assert(fgets(line, sizeof(line), output) && !strncmp(line, "123,456,", 8));
  assert(!fgets(line, sizeof(line), output));
  fclose(output);
  abandoned = sr_fopen(partial, "rb");
  assert(abandoned && fgets(line, sizeof(line), abandoned) &&
      !strcmp(line, "interrupted capture\n"));
  fclose(abandoned);
}

static void CheckIndependentStaging(const char *directory) {
  char path[1024];
  assert(snprintf(path, sizeof(path), "%s/published.txt", directory) < (int)sizeof(path));
  assert(AtomicReplace_WriteBytes(path, "old\n", 4, true));
  char *first, *second;
  FILE *a = AtomicReplace_OpenTemporary(path, "tmp", &first);
  FILE *b = AtomicReplace_OpenTemporary(path, "tmp", &second);
  assert(a && b && strcmp(first, second));
  assert(!AtomicReplace_SameFile(first, second));
  assert(fputs("first\n", a) >= 0 && fclose(a) == 0);
  assert(fputs("second\n", b) >= 0 && fclose(b) == 0);
  assert(AtomicReplaceFile(first, path));
  FILE *output = sr_fopen(path, "rb");
  char line[16];
  assert(output && fgets(line, sizeof(line), output) && !strcmp(line, "first\n"));
  assert(fclose(output) == 0);
  assert(AtomicReplaceFile(second, path));
  output = sr_fopen(path, "rb");
  assert(output && fgets(line, sizeof(line), output) && !strcmp(line, "second\n"));
  assert(fclose(output) == 0);
  free(first);
  free(second);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  CheckBlockedSink();
  CheckConcurrentProducers();
  CheckByteLimit();
  CheckTraceCollision(argv[1]);
  CheckIndependentStaging(argv[1]);
  CheckTimedFinish();
  return 0;
}
