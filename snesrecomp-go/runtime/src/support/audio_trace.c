#include "snesrecomp/host/audio_trace.h"

#include "snesrecomp/game/apu_sync.h"
#include "snes/snes.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static int16_t s_pcm[AUDIO_TRACE_PCM_RING * 2u];
static AudioTraceEvent s_events[AUDIO_TRACE_EVENT_RING];
static AudioTraceSnap s_snapshots[AUDIO_TRACE_SNAP_RING];
static AudioTraceStats s_stats;
static int s_producer;
static uint64_t s_open_drop = UINT64_MAX;
static uint64_t s_last_snapshot_ms;
static uint32_t s_largest_consume;
static uint8_t s_spc_read_last[4];
static uint8_t s_cpu_read_last[4];
static uint8_t s_spc_write_last[4];
static uint8_t s_spc_read_fresh[4];
static uint8_t s_cpu_read_fresh[4];
static uint8_t s_cpu_write_pending[4];
static _Atomic int s_enabled = -1;

int audio_trace_enabled(void) {
    int enabled = atomic_load_explicit(&s_enabled, memory_order_relaxed);
    if (enabled >= 0) return enabled;
    const char *setting = getenv("SNESRECOMP_AUDIO_TRACE");
    const int detected = setting != NULL && setting[0] != '\0' &&
                         setting[0] != '0';
    int expected = -1;
    if (atomic_compare_exchange_strong_explicit(
            &s_enabled, &expected, detected,
            memory_order_relaxed, memory_order_relaxed)) {
        return detected;
    }
    return expected;
}

void audio_trace_set_enabled(int enabled) {
    atomic_store_explicit(&s_enabled, enabled != 0,
                          memory_order_relaxed);
}

uint64_t audio_trace_wall_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter)) {
        return 0u;
    }
    const uint64_t hz = (uint64_t)frequency.QuadPart;
    const uint64_t ticks = (uint64_t)counter.QuadPart;
    return ticks / hz * 1000000000ull +
           ticks % hz * 1000000000ull / hz;
#else
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0u;
    return (uint64_t)time.tv_sec * 1000000000ull + (uint64_t)time.tv_nsec;
#endif
}

uint64_t audio_trace_wall_ms(void) {
    return audio_trace_wall_ns() / 1000000ull;
}

void audio_trace_reset(void) {
    RtlApuLock();
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_events, 0, sizeof(s_events));
    memset(s_snapshots, 0, sizeof(s_snapshots));
    memset(s_spc_read_last, 0, sizeof(s_spc_read_last));
    memset(s_cpu_read_last, 0, sizeof(s_cpu_read_last));
    memset(s_spc_write_last, 0, sizeof(s_spc_write_last));
    memset(s_spc_read_fresh, 0, sizeof(s_spc_read_fresh));
    memset(s_cpu_read_fresh, 0, sizeof(s_cpu_read_fresh));
    memset(s_cpu_write_pending, 0, sizeof(s_cpu_write_pending));
    s_producer = AUDIO_TRACE_PRODUCER_UNKNOWN;
    s_open_drop = UINT64_MAX;
    s_last_snapshot_ms = audio_trace_wall_ms();
    s_largest_consume = 0u;
    audio_trace_set_enabled(1);
    RtlApuUnlock();
}

static AudioTraceEvent *push_event(uint8_t type) {
    AudioTraceEvent *event =
        &s_events[s_stats.event_count & (AUDIO_TRACE_EVENT_RING - 1u)];
    ++s_stats.event_count;
    memset(event, 0, sizeof(*event));
    event->sample_idx = s_stats.produced;
    event->type = type;
    event->producer = (uint8_t)s_producer;
    return event;
}

static void take_snapshot_if_due(uint32_t occupancy) {
    const uint64_t now = audio_trace_wall_ms();
    if (now - s_last_snapshot_ms < 1000u) return;
    s_last_snapshot_ms = now;
    AudioTraceSnap *snapshot =
        &s_snapshots[s_stats.snap_count & (AUDIO_TRACE_SNAP_RING - 1u)];
    ++s_stats.snap_count;
    snapshot->wall_ms = now;
    snapshot->produced = s_stats.produced;
    snapshot->dropped = s_stats.dropped;
    snapshot->consumed = s_stats.consumed;
    snapshot->occupancy = occupancy;
}

void audio_trace_set_producer(int producer) {
    if (!audio_trace_enabled()) return;
    s_producer = producer;
}

void audio_trace_on_sample(int16_t left, int16_t right, int dropped,
                           uint32_t ring_fill) {
    if (!audio_trace_enabled()) return;
    const uint32_t position =
        (uint32_t)s_stats.produced & (AUDIO_TRACE_PCM_RING - 1u);
    s_pcm[position * 2u] = left;
    s_pcm[position * 2u + 1u] = right;
    if (dropped) {
        if (s_open_drop != UINT64_MAX &&
            s_stats.event_count - s_open_drop <= AUDIO_TRACE_EVENT_RING) {
            ++s_events[s_open_drop & (AUDIO_TRACE_EVENT_RING - 1u)].aux;
        } else {
            s_open_drop = s_stats.event_count;
            push_event(AUDIO_TRACE_EV_DROP)->aux = 1u;
            ++s_stats.drop_runs;
        }
        ++s_stats.dropped;
    } else {
        s_open_drop = UINT64_MAX;
    }
    ++s_stats.produced;
    if (s_producer == AUDIO_TRACE_PRODUCER_CPU) ++s_stats.produced_cpu;
    if (s_producer == AUDIO_TRACE_PRODUCER_AUDIO) ++s_stats.produced_audio;
    if (ring_fill > s_stats.occupancy_highwater) {
        s_stats.occupancy_highwater = ring_fill;
    }
    take_snapshot_if_due(ring_fill);
}

void audio_trace_on_reg_write(uint8_t address, uint8_t value) {
    if (!audio_trace_enabled()) return;
    AudioTraceEvent *event = push_event(AUDIO_TRACE_EV_REG);
    event->addr = address;
    event->val = value;
    ++s_stats.reg_writes;
    if (address == 0x4cu && value != 0u) ++s_stats.kon_writes;
    s_open_drop = UINT64_MAX;
}

void audio_trace_on_consume(uint64_t read_index, uint32_t count,
                            uint32_t available_after) {
    if (!audio_trace_enabled()) return;
    (void)read_index;
    AudioTraceEvent *event = push_event(AUDIO_TRACE_EV_CONSUME);
    event->aux = available_after;
    s_stats.consumed += count;
    ++s_stats.consume_calls;
    if (count > s_largest_consume) s_largest_consume = count;
    s_open_drop = UINT64_MAX;
}

void audio_trace_on_pace(int consumer_active, uint32_t baseline_cycles) {
    if (!audio_trace_enabled()) return;
    s_stats.pace_consumer_active = consumer_active != 0;
    s_stats.pace_baseline_cycles += baseline_cycles;
    ++s_stats.pace_accumulate_calls;
}

static AudioTraceEvent *push_port_event(uint8_t type, uint8_t port,
                                        uint8_t value) {
    AudioTraceEvent *event = push_event(type);
    event->addr = port;
    event->val = value;
    event->aux = (uint32_t)snes_frame_counter;
    return event;
}

void audio_trace_on_cpu_port_write(uint8_t port, uint8_t value) {
    if (!audio_trace_enabled()) return;
    port &= 3u;
    ++s_stats.cpu_port_writes;
    (void)push_port_event(AUDIO_TRACE_EV_CPU_PORT_WRITE, port, value);
}

void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t value) {
    if (!audio_trace_enabled()) return;
    port &= 3u;
    if (s_cpu_write_pending[port]) ++s_stats.cpu_port_overwrites[port];
    s_cpu_write_pending[port] = value != 0u;
    s_spc_read_fresh[port] = 1u;
    (void)push_port_event(AUDIO_TRACE_EV_CPU_PORT_APPLY, port, value);
}

void audio_trace_on_spc_port_read(uint8_t port, uint8_t value) {
    if (!audio_trace_enabled()) return;
    port &= 3u;
    ++s_stats.spc_port_reads_seen;
    s_cpu_write_pending[port] = 0u;
    if (!s_spc_read_fresh[port] && s_spc_read_last[port] == value) return;
    s_spc_read_fresh[port] = 0u;
    s_spc_read_last[port] = value;
    ++s_stats.spc_port_reads_logged;
    (void)push_port_event(AUDIO_TRACE_EV_SPC_PORT_READ, port, value);
}

void audio_trace_on_spc_port_write(uint8_t port, uint8_t value) {
    if (!audio_trace_enabled()) return;
    port &= 3u;
    ++s_stats.spc_port_writes;
    s_cpu_read_fresh[port] = 1u;
    if (s_spc_write_last[port] == value) return;
    s_spc_write_last[port] = value;
    (void)push_port_event(AUDIO_TRACE_EV_SPC_PORT_WRITE, port, value);
}

void audio_trace_on_cpu_port_read(uint8_t port, uint8_t value) {
    if (!audio_trace_enabled()) return;
    port &= 3u;
    if (!s_cpu_read_fresh[port] && s_cpu_read_last[port] == value) return;
    s_cpu_read_fresh[port] = 0u;
    s_cpu_read_last[port] = value;
    ++s_stats.cpu_port_reads_logged;
    (void)push_port_event(AUDIO_TRACE_EV_CPU_PORT_READ, port, value);
}

void audio_trace_sample_clocks(uint64_t *produced, uint64_t *consumed) {
    if (produced != NULL) *produced = s_stats.produced;
    if (consumed != NULL) *consumed = s_stats.consumed;
}

uint32_t audio_trace_consume_quantum(void) {
    return s_largest_consume > 534u ? s_largest_consume : 534u;
}

void audio_trace_get_stats(AudioTraceStats *output) {
    if (output == NULL) return;
    RtlApuLock();
    *output = s_stats;
    RtlApuUnlock();
}

uint32_t audio_trace_copy_events(uint64_t first_index, uint32_t maximum,
                                 AudioTraceEvent *output, uint64_t *oldest) {
    RtlApuLock();
    const uint64_t total = s_stats.event_count;
    const uint64_t first_available = total > AUDIO_TRACE_EVENT_RING
        ? total - AUDIO_TRACE_EVENT_RING : 0u;
    if (oldest != NULL) *oldest = first_available;
    if (first_index < first_available) first_index = first_available;
    uint32_t copied = 0u;
    while (copied < maximum && first_index + copied < total) {
        if (output != NULL) {
            output[copied] = s_events[(first_index + copied) &
                                      (AUDIO_TRACE_EVENT_RING - 1u)];
        }
        ++copied;
    }
    RtlApuUnlock();
    return copied;
}

uint32_t audio_trace_copy_snaps(uint64_t first_index, uint32_t maximum,
                                AudioTraceSnap *output, uint64_t *oldest) {
    RtlApuLock();
    const uint64_t total = s_stats.snap_count;
    const uint64_t first_available = total > AUDIO_TRACE_SNAP_RING
        ? total - AUDIO_TRACE_SNAP_RING : 0u;
    if (oldest != NULL) *oldest = first_available;
    if (first_index < first_available) first_index = first_available;
    uint32_t copied = 0u;
    while (copied < maximum && first_index + copied < total) {
        if (output != NULL) {
            output[copied] = s_snapshots[(first_index + copied) &
                                         (AUDIO_TRACE_SNAP_RING - 1u)];
        }
        ++copied;
    }
    RtlApuUnlock();
    return copied;
}

static bool write_u16(FILE *file, uint16_t value) {
    const uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes);
}

static bool write_u32(FILE *file, uint32_t value) {
    const uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24)
    };
    return fwrite(bytes, 1u, sizeof(bytes), file) == sizeof(bytes);
}

int audio_trace_dump_wav(const char *path, int64_t start_index, uint64_t count,
                         uint64_t *output_start, uint64_t *output_count) {
    if (path == NULL) return -1;
    RtlApuLock();
    const uint64_t total = s_stats.produced;
    RtlApuUnlock();
    const uint64_t oldest = total > AUDIO_TRACE_PCM_RING
        ? total - AUDIO_TRACE_PCM_RING : 0u;
    uint64_t start = start_index < 0 ? oldest : (uint64_t)start_index;
    if (start < oldest) start = oldest;
    if (start > total) start = total;
    const uint64_t available = total - start;
    if (count == 0u || count > available) count = available;
    if (count > (UINT32_MAX - 36u) / 4u) count = (UINT32_MAX - 36u) / 4u;

    FILE *file = fopen(path, "wb");
    if (file == NULL) return -1;
    const uint32_t data_bytes = (uint32_t)(count * 4u);
    bool ok = fwrite("RIFF", 1u, 4u, file) == 4u &&
              write_u32(file, 36u + data_bytes) &&
              fwrite("WAVEfmt ", 1u, 8u, file) == 8u &&
              write_u32(file, 16u) && write_u16(file, 1u) &&
              write_u16(file, 2u) && write_u32(file, 32000u) &&
              write_u32(file, 128000u) && write_u16(file, 4u) &&
              write_u16(file, 16u) &&
              fwrite("data", 1u, 4u, file) == 4u &&
              write_u32(file, data_bytes);
    for (uint64_t index = 0u; ok && index < count; ++index) {
        const uint32_t position =
            (uint32_t)(start + index) & (AUDIO_TRACE_PCM_RING - 1u);
        ok = write_u16(file, (uint16_t)s_pcm[position * 2u]) &&
             write_u16(file, (uint16_t)s_pcm[position * 2u + 1u]);
    }
    if (fclose(file) != 0) ok = false;
    if (!ok) return -1;
    if (output_start != NULL) *output_start = start;
    if (output_count != NULL) *output_count = count;
    return 0;
}
