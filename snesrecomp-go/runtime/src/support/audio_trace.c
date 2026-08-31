#include "audio_audit_internal.h"

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

static AudioTraceEvent *s_events;
static AudioTraceStats s_stats;
static int s_producer;
static uint64_t s_open_drop = UINT64_MAX;
static uint8_t s_spc_read_last[4];
static uint8_t s_cpu_read_last[4];
static uint8_t s_spc_write_last[4];
static uint8_t s_spc_read_fresh[4];
static uint8_t s_cpu_read_fresh[4];
static uint8_t s_cpu_write_pending[4];
static uint8_t s_cpu_write_pending_value[4];
typedef struct PortWriteSource {
    uint32_t source_block;
    const char *function_name;
    uint8_t value;
} PortWriteSource;
enum { kPortWriteSourceCapacity = 128 };
static PortWriteSource s_port_write_sources[4][kPortWriteSourceCapacity];
static uint32_t s_port_source_head[4];
static uint32_t s_port_source_tail[4];
static _Atomic int s_enabled = -1;

int audio_trace_enabled(void) {
    int enabled = atomic_load_explicit(&s_enabled, memory_order_relaxed);
    if (enabled >= 0) return enabled;
    const char *audit = getenv("SNESRECOMP_APU_AUDIT_PREFIX");
    const int detected = audit != NULL && audit[0] != '\0';
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
    AudioTraceEvent *events = s_events;
    if (events == NULL) {
        events = (AudioTraceEvent *)calloc(
            AUDIO_TRACE_EVENT_RING, sizeof(*events));
        if (events == NULL) {
            fprintf(stderr,
                    "[apu-audit] cannot allocate retained event storage\n");
            audio_trace_set_enabled(0);
            return;
        }
    }
    RtlApuLock();
    s_events = events;
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_events, 0,
           (size_t)AUDIO_TRACE_EVENT_RING * sizeof(*s_events));
    memset(s_spc_read_last, 0, sizeof(s_spc_read_last));
    memset(s_cpu_read_last, 0, sizeof(s_cpu_read_last));
    memset(s_spc_write_last, 0, sizeof(s_spc_write_last));
    memset(s_spc_read_fresh, 0, sizeof(s_spc_read_fresh));
    memset(s_cpu_read_fresh, 0, sizeof(s_cpu_read_fresh));
    memset(s_cpu_write_pending, 0, sizeof(s_cpu_write_pending));
    memset(s_cpu_write_pending_value, 0,
           sizeof(s_cpu_write_pending_value));
    memset(s_port_write_sources, 0, sizeof(s_port_write_sources));
    memset(s_port_source_head, 0, sizeof(s_port_source_head));
    memset(s_port_source_tail, 0, sizeof(s_port_source_tail));
    s_producer = AUDIO_TRACE_PRODUCER_UNKNOWN;
    s_open_drop = UINT64_MAX;
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

void audio_trace_set_producer(int producer) {
    if (!audio_trace_enabled()) return;
    s_producer = producer;
}

void audio_trace_on_sample(int16_t left, int16_t right, int dropped,
                           uint32_t ring_fill) {
    (void)left;
    (void)right;
    if (!audio_trace_enabled() || s_events == NULL) return;
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
}

void audio_trace_on_reg_write(uint8_t address, uint8_t value) {
    if (!audio_trace_enabled() || s_events == NULL) return;
    AudioTraceEvent *event = push_event(AUDIO_TRACE_EV_REG);
    event->addr = address;
    event->val = value;
    ++s_stats.reg_writes;
    if (address == 0x4cu && value != 0u) ++s_stats.kon_writes;
    s_open_drop = UINT64_MAX;
}

void audio_trace_on_consume(uint64_t read_index, uint32_t count,
                            uint32_t available_after) {
    if (!audio_trace_enabled() || s_events == NULL) return;
    (void)read_index;
    AudioTraceEvent *event = push_event(AUDIO_TRACE_EV_CONSUME);
    event->aux = available_after;
    s_stats.consumed += count;
    ++s_stats.consume_calls;
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

static void push_port_source(uint8_t port, uint8_t value,
                             uint32_t source_block,
                             const char *function_name) {
    uint32_t tail = s_port_source_tail[port];
    if (tail - s_port_source_head[port] >= kPortWriteSourceCapacity)
        ++s_port_source_head[port];
    PortWriteSource *source =
        &s_port_write_sources[port][tail & (kPortWriteSourceCapacity - 1u)];
    source->source_block = source_block;
    source->function_name = function_name;
    source->value = value;
    s_port_source_tail[port] = tail + 1u;
}

static PortWriteSource pop_port_source(uint8_t port, uint8_t value) {
    PortWriteSource source = {0};
    uint32_t head = s_port_source_head[port];
    const uint32_t tail = s_port_source_tail[port];
    for (uint32_t cursor = head; cursor != tail; ++cursor) {
        const PortWriteSource candidate = s_port_write_sources[port][
            cursor & (kPortWriteSourceCapacity - 1u)];
        if (candidate.value != value) continue;
        source = candidate;
        s_port_source_head[port] = cursor + 1u;
        return source;
    }
    if (head != tail) {
        source = s_port_write_sources[port][
            head & (kPortWriteSourceCapacity - 1u)];
        s_port_source_head[port] = head + 1u;
    }
    if (source.value != value) source.value = value;
    return source;
}

static bool base16_milestone(uint64_t hits) {
    if (hits == 1u) return true;
    while (hits > 1u && hits % 16u == 0u) hits /= 16u;
    return hits == 1u;
}

void audio_trace_on_cpu_port_write(uint8_t port, uint8_t value) {
    audio_trace_on_cpu_port_write_at(port, value, 0u, NULL);
}

void audio_trace_on_cpu_port_write_at(uint8_t port, uint8_t value,
                                      uint32_t source_block,
                                      const char *function_name) {
    if (!audio_trace_enabled() || s_events == NULL) return;
    port &= 3u;
    ++s_stats.cpu_port_writes;
    AudioTraceEvent *event =
        push_port_event(AUDIO_TRACE_EV_CPU_PORT_WRITE, port, value);
    event->source_block = source_block & 0x00ffffffu;
    event->function_name = function_name;
    push_port_source(port, value, event->source_block, function_name);
}

void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t value) {
    if (!audio_trace_enabled() || s_events == NULL) return;
    port &= 3u;
    const PortWriteSource source = pop_port_source(port, value);
    ++s_stats.cpu_port_applies;
    if (s_cpu_write_pending[port]) {
        if (s_cpu_write_pending_value[port] == value) {
            ++s_stats.cpu_port_same_value_rewrites[port];
        } else {
            const uint64_t hits = ++s_stats.cpu_port_overwrites[port];
            if (base16_milestone(hits)) {
                fprintf(stderr,
                        "[apu-port-overwrite] port=%u old=$%02X new=$%02X "
                        "frame=%d block=$%06X fn=%s hits=%llu "
                        "(different value applied before an SPC read)\n",
                        port, s_cpu_write_pending_value[port], value,
                        snes_frame_counter, source.source_block,
                        source.function_name != NULL ? source.function_name : "?",
                        (unsigned long long)hits);
            }
        }
    }
    s_cpu_write_pending[port] = 1u;
    s_cpu_write_pending_value[port] = value;
    s_spc_read_fresh[port] = 1u;
    AudioTraceEvent *event =
        push_port_event(AUDIO_TRACE_EV_CPU_PORT_APPLY, port, value);
    event->source_block = source.source_block;
    event->function_name = source.function_name;
}

void audio_trace_on_spc_port_read(uint8_t port, uint8_t value) {
    if (!audio_trace_enabled() || s_events == NULL) return;
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
    if (!audio_trace_enabled() || s_events == NULL) return;
    port &= 3u;
    ++s_stats.spc_port_writes;
    s_cpu_read_fresh[port] = 1u;
    if (s_spc_write_last[port] == value) return;
    s_spc_write_last[port] = value;
    (void)push_port_event(AUDIO_TRACE_EV_SPC_PORT_WRITE, port, value);
}

void audio_trace_on_cpu_port_read(uint8_t port, uint8_t value) {
    if (!audio_trace_enabled() || s_events == NULL) return;
    port &= 3u;
    if (!s_cpu_read_fresh[port] && s_cpu_read_last[port] == value) return;
    s_cpu_read_fresh[port] = 0u;
    s_cpu_read_last[port] = value;
    ++s_stats.cpu_port_reads_logged;
    (void)push_port_event(AUDIO_TRACE_EV_CPU_PORT_READ, port, value);
}

void audio_trace_get_stats(AudioTraceStats *output) {
    if (output == NULL) return;
    RtlApuLock();
    *output = s_stats;
    RtlApuUnlock();
}

static const char *event_type_name(uint8_t type) {
    switch (type) {
        case AUDIO_TRACE_EV_REG: return "dsp_write";
        case AUDIO_TRACE_EV_DROP: return "pcm_drop";
        case AUDIO_TRACE_EV_CONSUME: return "pcm_consume";
        case AUDIO_TRACE_EV_CPU_PORT_WRITE: return "cpu_port_write";
        case AUDIO_TRACE_EV_SPC_PORT_READ: return "spc_port_read";
        case AUDIO_TRACE_EV_SPC_PORT_WRITE: return "spc_port_write";
        case AUDIO_TRACE_EV_CPU_PORT_READ: return "cpu_port_read";
        case AUDIO_TRACE_EV_CPU_PORT_APPLY: return "apu_port_apply";
        default: return "unknown";
    }
}

static void write_json_string(FILE *file, const char *value) {
    if (value == NULL) value = "";
    fputc('"', file);
    while (*value != '\0') {
        const unsigned char byte = (unsigned char)*value++;
        if (byte == '"' || byte == '\\') {
            fputc('\\', file);
            fputc((int)byte, file);
        } else if (byte >= 0x20u) {
            fputc((int)byte, file);
        }
    }
    fputc('"', file);
}

static bool is_apu_audit_event(uint8_t type) {
    return type == AUDIO_TRACE_EV_REG ||
           type == AUDIO_TRACE_EV_CPU_PORT_WRITE ||
           type == AUDIO_TRACE_EV_CPU_PORT_APPLY ||
           type == AUDIO_TRACE_EV_SPC_PORT_READ;
}

int audio_trace_dump_jsonl(const char *path) {
    FILE *file;
    uint64_t oldest, total, selected = 0u;
    if (path == NULL || path[0] == '\0' || s_events == NULL) return -1;
    file = fopen(path, "wb");
    if (file == NULL) return -1;
    RtlApuLock();
    total = s_stats.event_count;
    oldest = total > AUDIO_TRACE_EVENT_RING
        ? total - AUDIO_TRACE_EVENT_RING : 0u;
    for (uint64_t index = oldest; index < total; ++index) {
        const AudioTraceEvent *event =
            &s_events[index & (AUDIO_TRACE_EVENT_RING - 1u)];
        if (is_apu_audit_event(event->type)) ++selected;
    }
    fprintf(file,
            "{\"format\":\"snesrecomp-audio-trace\",\"version\":3,"
            "\"event_count\":%llu,\"trace_overflow\":%s,"
            "\"cpu_port_writes\":%llu,\"apu_port_applies\":%llu,"
            "\"spc_port_reads\":%llu,"
            "\"cpu_port_overwrites\":[%llu,%llu,%llu,%llu],"
            "\"cpu_port_same_value_rewrites\":[%llu,%llu,%llu,%llu]}\n",
            (unsigned long long)selected, oldest != 0u ? "true" : "false",
            (unsigned long long)s_stats.cpu_port_writes,
            (unsigned long long)s_stats.cpu_port_applies,
            (unsigned long long)s_stats.spc_port_reads_seen,
            (unsigned long long)s_stats.cpu_port_overwrites[0],
            (unsigned long long)s_stats.cpu_port_overwrites[1],
            (unsigned long long)s_stats.cpu_port_overwrites[2],
            (unsigned long long)s_stats.cpu_port_overwrites[3],
            (unsigned long long)s_stats.cpu_port_same_value_rewrites[0],
            (unsigned long long)s_stats.cpu_port_same_value_rewrites[1],
            (unsigned long long)s_stats.cpu_port_same_value_rewrites[2],
            (unsigned long long)s_stats.cpu_port_same_value_rewrites[3]);
    uint64_t audit_index = 0u;
    for (uint64_t index = oldest; index < total; ++index) {
        const AudioTraceEvent *event =
            &s_events[index & (AUDIO_TRACE_EVENT_RING - 1u)];
        if (!is_apu_audit_event(event->type)) continue;
        fprintf(file,
                "{\"kind\":\"event\",\"index\":%llu,"
                "\"frame\":%u,\"type\":\"%s\","
                "\"address\":%u,\"value\":%u,"
                "\"source_block\":%u,\"function\":",
                (unsigned long long)audit_index++, event->aux,
                event_type_name(event->type), event->addr, event->val,
                event->source_block);
        write_json_string(file, event->function_name);
        fputs("}\n", file);
    }
    RtlApuUnlock();
    if (fclose(file) != 0) return -1;
    return 0;
}

uint32_t audio_trace_copy_events(uint64_t first_index, uint32_t maximum,
                                 AudioTraceEvent *output, uint64_t *oldest) {
    RtlApuLock();
    if (s_events == NULL) {
        if (oldest != NULL) *oldest = 0u;
        RtlApuUnlock();
        return 0u;
    }
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
