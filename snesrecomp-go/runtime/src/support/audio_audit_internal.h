#ifndef SNESRECOMP_AUDIO_AUDIT_INTERNAL_H
#define SNESRECOMP_AUDIO_AUDIT_INTERNAL_H

#include <stdint.h>

/* Private recorder used only to build the `snesbuild apu-audit` evidence
 * bundle. Hosts observe audio through the public runner event and audio-trace
 * subscriptions; this state is not a host ABI. */

#define AUDIO_TRACE_EVENT_RING (1u << 19)

enum {
    AUDIO_TRACE_EV_REG = 1,
    AUDIO_TRACE_EV_DROP,
    AUDIO_TRACE_EV_CONSUME,
    AUDIO_TRACE_EV_CPU_PORT_WRITE,
    AUDIO_TRACE_EV_SPC_PORT_READ,
    AUDIO_TRACE_EV_SPC_PORT_WRITE,
    AUDIO_TRACE_EV_CPU_PORT_READ,
    AUDIO_TRACE_EV_CPU_PORT_APPLY
};

enum {
    AUDIO_TRACE_PRODUCER_UNKNOWN = 0,
    AUDIO_TRACE_PRODUCER_CPU,
    AUDIO_TRACE_PRODUCER_AUDIO
};

typedef struct AudioTraceEvent {
    uint64_t sample_idx;
    uint32_t aux;
    uint32_t source_block;
    const char *function_name;
    uint8_t type;
    uint8_t addr;
    uint8_t val;
    uint8_t producer;
} AudioTraceEvent;

typedef struct AudioTraceStats {
    uint64_t produced;
    uint64_t produced_cpu;
    uint64_t produced_audio;
    uint64_t dropped;
    uint64_t drop_runs;
    uint64_t consumed;
    uint64_t consume_calls;
    uint64_t reg_writes;
    uint64_t kon_writes;
    uint32_t occupancy_highwater;
    uint64_t event_count;
    uint64_t pace_baseline_cycles;
    uint64_t pace_accumulate_calls;
    uint32_t pace_consumer_active;
    uint64_t cpu_port_writes;
    uint64_t cpu_port_applies;
    uint64_t spc_port_reads_seen;
    uint64_t spc_port_reads_logged;
    uint64_t spc_port_writes;
    uint64_t cpu_port_reads_logged;
    uint64_t cpu_port_overwrites[4];
    uint64_t cpu_port_same_value_rewrites[4];
} AudioTraceStats;

int audio_trace_enabled(void);
void audio_trace_set_enabled(int enabled);
void audio_trace_reset(void);
void audio_trace_on_sample(int16_t left, int16_t right, int dropped,
                           uint32_t ring_fill);
void audio_trace_on_reg_write(uint8_t address, uint8_t value);
void audio_trace_on_consume(uint64_t read_index, uint32_t count,
                            uint32_t available_after);
void audio_trace_set_producer(int producer);
void audio_trace_on_pace(int consumer_active, uint32_t baseline_cycles);
void audio_trace_on_cpu_port_write(uint8_t port, uint8_t value);
void audio_trace_on_cpu_port_write_at(uint8_t port, uint8_t value,
                                      uint32_t source_block,
                                      const char *function_name);
void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t value);
void audio_trace_on_spc_port_read(uint8_t port, uint8_t value);
void audio_trace_on_spc_port_write(uint8_t port, uint8_t value);
void audio_trace_on_cpu_port_read(uint8_t port, uint8_t value);
uint64_t audio_trace_wall_ms(void);
uint64_t audio_trace_wall_ns(void);
void audio_trace_get_stats(AudioTraceStats *output);
uint32_t audio_trace_copy_events(uint64_t first_index, uint32_t maximum,
                                 AudioTraceEvent *output, uint64_t *oldest);
int audio_trace_dump_jsonl(const char *path);

#endif
