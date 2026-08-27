/**
 * @file audio_trace.h
 * @brief Optional host-side PCM and APU event tracing.
 * @ingroup sr_host
 */
#ifndef SNESRECOMP_HOST_AUDIO_TRACE_H
#define SNESRECOMP_HOST_AUDIO_TRACE_H

#include <stdint.h>

/** @addtogroup sr_host
 *  @{
 */

#define AUDIO_TRACE_PCM_RING (1u << 22)
#define AUDIO_TRACE_EVENT_RING (1u << 19)
#define AUDIO_TRACE_SNAP_RING (1u << 12)

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
    uint8_t type;
    uint8_t addr;
    uint8_t val;
    uint8_t producer;
} AudioTraceEvent;

typedef struct AudioTraceSnap {
    uint64_t wall_ms;
    uint64_t produced;
    uint64_t dropped;
    uint64_t consumed;
    uint32_t occupancy;
} AudioTraceSnap;

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
    uint64_t snap_count;
    uint64_t pace_baseline_cycles;
    uint64_t pace_accumulate_calls;
    uint32_t pace_consumer_active;
    uint64_t cpu_port_writes;
    uint64_t spc_port_reads_seen;
    uint64_t spc_port_reads_logged;
    uint64_t spc_port_writes;
    uint64_t cpu_port_reads_logged;
    uint64_t cpu_port_overwrites[4];
} AudioTraceStats;

void audio_trace_reset(void);
void audio_trace_on_sample(int16_t left, int16_t right, int dropped,
                           uint32_t ring_fill);
void audio_trace_on_reg_write(uint8_t address, uint8_t value);
void audio_trace_on_consume(uint64_t read_index, uint32_t count,
                            uint32_t available_after);
void audio_trace_set_producer(int producer);
void audio_trace_on_pace(int consumer_active, uint32_t baseline_cycles);
void audio_trace_on_cpu_port_write(uint8_t port, uint8_t value);
void audio_trace_on_cpu_port_apply(uint8_t port, uint8_t value);
void audio_trace_on_spc_port_read(uint8_t port, uint8_t value);
void audio_trace_on_spc_port_write(uint8_t port, uint8_t value);
void audio_trace_on_cpu_port_read(uint8_t port, uint8_t value);
void audio_trace_sample_clocks(uint64_t *produced, uint64_t *consumed);
uint64_t audio_trace_wall_ms(void);
uint64_t audio_trace_wall_ns(void);
uint32_t audio_trace_consume_quantum(void);
void audio_trace_get_stats(AudioTraceStats *output);
uint32_t audio_trace_copy_events(uint64_t first_index, uint32_t maximum,
                                 AudioTraceEvent *output, uint64_t *oldest);
uint32_t audio_trace_copy_snaps(uint64_t first_index, uint32_t maximum,
                                AudioTraceSnap *output, uint64_t *oldest);
int audio_trace_dump_wav(const char *path, int64_t start_index, uint64_t count,
                         uint64_t *output_start, uint64_t *output_count);

/** @} */

#endif
