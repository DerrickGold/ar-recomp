#include "snesrecomp/host/audio_trace.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef AUDIO_TRACE_TEST_WAV
#define AUDIO_TRACE_TEST_WAV "runtime-audio-trace.wav"
#endif
#ifndef AUDIO_TRACE_TEST_JSONL
#define AUDIO_TRACE_TEST_JSONL "runtime-audio-trace.jsonl"
#endif

static int failures;
static unsigned lock_depth;
int snes_frame_counter = 17;

void RtlApuLock(void) { ++lock_depth; }
void RtlApuUnlock(void) {
    if (lock_depth == 0u) ++failures;
    else --lock_depth;
}

static void check(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "runtime audio trace failed: %s\n", message);
    ++failures;
}

static uint16_t little_u16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

static void test_samples_and_events(void) {
    audio_trace_reset();
    audio_trace_set_producer(AUDIO_TRACE_PRODUCER_CPU);
    audio_trace_on_sample(100, -100, 0, 1u);
    audio_trace_on_sample(200, -200, 1, 2u);
    audio_trace_on_sample(300, -300, 1, 3u);
    audio_trace_on_reg_write(0x4cu, 3u);
    audio_trace_on_consume(0u, 700u, 2u);
    audio_trace_on_pace(1, 123u);

    AudioTraceStats stats;
    audio_trace_get_stats(&stats);
    check(stats.produced == 3u && stats.produced_cpu == 3u &&
          stats.dropped == 2u && stats.drop_runs == 1u,
          "sample and coalesced-drop counters");
    check(stats.reg_writes == 1u && stats.kon_writes == 1u &&
          stats.consumed == 700u && stats.consume_calls == 1u,
          "register and consumption counters");
    check(stats.pace_consumer_active == 1u &&
          stats.pace_baseline_cycles == 123u,
          "pacing counters");
    check(audio_trace_consume_quantum() == 700u,
          "consume quantum follows largest callback");

    AudioTraceEvent events[4];
    uint64_t oldest = UINT64_MAX;
    const uint32_t copied = audio_trace_copy_events(0u, 4u, events, &oldest);
    check(copied == 3u && oldest == 0u &&
          events[0].type == AUDIO_TRACE_EV_DROP && events[0].aux == 2u &&
          events[0].producer == AUDIO_TRACE_PRODUCER_CPU,
          "event copy exposes consolidated drop run");
}

static void test_port_gating(void) {
    audio_trace_on_cpu_port_write_at(5u, 0x10u, 0x000aaau, "stale");
    audio_trace_on_cpu_port_write_at(5u, 0x11u, 0x00d583u, "upload\"fn");
    audio_trace_on_cpu_port_apply(5u, 0x11u);
    audio_trace_on_cpu_port_apply(5u, 0x22u);
    audio_trace_on_cpu_port_apply(5u, 0x22u);
    audio_trace_on_spc_port_read(5u, 0x22u);
    audio_trace_on_spc_port_read(5u, 0x22u);
    audio_trace_on_spc_port_write(5u, 0x33u);
    audio_trace_on_spc_port_write(5u, 0x33u);
    audio_trace_on_cpu_port_read(5u, 0x33u);
    audio_trace_on_cpu_port_read(5u, 0x33u);
    AudioTraceStats stats;
    audio_trace_get_stats(&stats);
    check(stats.cpu_port_writes == 2u && stats.cpu_port_applies == 3u &&
          stats.cpu_port_overwrites[1] == 1u &&
          stats.cpu_port_same_value_rewrites[1] == 1u,
          "changed overwrite and same-value rewrite counted separately");
    check(stats.spc_port_reads_seen == 2u &&
          stats.spc_port_reads_logged == 1u &&
          stats.spc_port_writes == 2u &&
          stats.cpu_port_reads_logged == 1u,
          "steady port polling is elided while raw totals remain");
}

static void test_jsonl_dump(void) {
    check(audio_trace_dump_jsonl(AUDIO_TRACE_TEST_JSONL) == 0,
          "JSONL evidence dump");
    FILE *file = fopen(AUDIO_TRACE_TEST_JSONL, "rb");
    char contents[8192];
    const size_t count = file == NULL ? 0u :
        fread(contents, 1u, sizeof(contents) - 1u, file);
    if (file != NULL) fclose(file);
    contents[count] = '\0';
    check(strstr(contents, "\"format\":\"snesrecomp-audio-trace\"") != NULL &&
          strstr(contents, "\"type\":\"apu_port_apply\"") != NULL &&
          strstr(contents, "\"source_block\":54659") != NULL &&
          strstr(contents, "upload\\\"fn") != NULL,
          "JSONL contains correlated and escaped port evidence");
    check(remove(AUDIO_TRACE_TEST_JSONL) == 0, "remove JSONL fixture");
}

static void test_wav_dump(void) {
    uint64_t start = UINT64_MAX, count = 0u;
    check(audio_trace_dump_wav(AUDIO_TRACE_TEST_WAV, -1, 0u,
                               &start, &count) == 0 &&
          start == 0u && count == 3u, "WAV dump range");
    FILE *file = fopen(AUDIO_TRACE_TEST_WAV, "rb");
    uint8_t bytes[56];
    const size_t read = file == NULL ? 0u : fread(bytes, 1u, sizeof(bytes), file);
    if (file != NULL) fclose(file);
    check(read == sizeof(bytes) && memcmp(bytes, "RIFF", 4u) == 0 &&
          memcmp(bytes + 8u, "WAVEfmt ", 8u) == 0 &&
          little_u16(bytes + 44u) == 100u &&
          (int16_t)little_u16(bytes + 46u) == -100,
          "WAV header and little-endian PCM payload");
    check(remove(AUDIO_TRACE_TEST_WAV) == 0, "remove WAV fixture");
}

static void test_disabled_fast_path(void) {
    AudioTraceStats before, after;
    audio_trace_get_stats(&before);
    audio_trace_set_enabled(0);
    audio_trace_set_producer(AUDIO_TRACE_PRODUCER_AUDIO);
    audio_trace_on_sample(400, -400, 1, 4u);
    audio_trace_on_reg_write(0x4cu, 1u);
    audio_trace_on_consume(3u, 32u, 0u);
    audio_trace_on_pace(0, 9u);
    audio_trace_on_cpu_port_write(0u, 1u);
    audio_trace_on_cpu_port_apply(0u, 1u);
    audio_trace_on_spc_port_read(0u, 1u);
    audio_trace_on_spc_port_write(0u, 1u);
    audio_trace_on_cpu_port_read(0u, 1u);
    audio_trace_get_stats(&after);
    check(memcmp(&before, &after, sizeof(before)) == 0,
          "disabled recorder leaves all trace state untouched");
}

int main(void) {
    test_samples_and_events();
    test_port_gating();
    test_jsonl_dump();
    test_wav_dump();
    test_disabled_fast_path();
    check(lock_depth == 0u, "query locks remain balanced");
    if (failures != 0) {
        fprintf(stderr, "runtime audio trace: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime audio trace: PASS");
    return 0;
}
