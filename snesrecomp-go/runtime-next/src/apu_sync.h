#ifndef SNESRECOMP_NEXT_APU_SYNC_H
#define SNESRECOMP_NEXT_APU_SYNC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host-provided synchronization around the runtime's APU state. */
void RtlApuLock(void);
void RtlApuUnlock(void);

/* Runtime-owned helpers used by the SNES bus implementation. */
void RtlApuWrite(uint16_t address, uint8_t value);
void rtl_accumulate_apu_catchup(void);

#ifdef __cplusplus
}
#endif

#endif
