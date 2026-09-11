#ifndef SNESRECOMP_GAME_AUDIO_TIMING_H
#define SNESRECOMP_GAME_AUDIO_TIMING_H

#include <stdint.h>

/* Nominal S-SMP crystal and S-DSP output clock. NTSC non-interlaced ticks
 * use the mean of the 357368/357364-master-cycle fields. Keep the fractional
 * APU cycle between ticks; a tick is not an integral number of PCM frames.
 * Ratio: 1024000 / (236250000 / 11) = 5632 / 118125. */
#define RTL_AUDIO_NATIVE_RATE UINT32_C(32000)
#define RTL_AUDIO_CYCLES_PER_SECOND UINT32_C(1024000)
#define RTL_AUDIO_CYCLES_PER_SAMPLE UINT32_C(32)
#define RTL_APU_TICK_CYCLE_NUMERATOR (UINT64_C(357366) * UINT64_C(5632))
#define RTL_APU_TICK_CYCLE_DENOMINATOR UINT32_C(118125)
#define RTL_NTSC_FRAME_INTERVAL_NS UINT64_C(16639263)
#define RTL_APU_TICK_MAX_FRAMES UINT32_C(533)
#define RTL_APU_PRODUCTION_CHUNK_CYCLES UINT32_C(256)

/* remainder is serialized runner state, initially zero and always < den. */
static inline uint32_t RtlAudioNextTickCycles(uint32_t *remainder) {
    const uint64_t numerator = RTL_APU_TICK_CYCLE_NUMERATOR + *remainder;
    *remainder = (uint32_t)(numerator % RTL_APU_TICK_CYCLE_DENOMINATOR);
    return (uint32_t)(numerator / RTL_APU_TICK_CYCLE_DENOMINATOR);
}

#endif
