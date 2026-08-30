#ifndef SNESRECOMP_GAME_APU_SYNC_H
#define SNESRECOMP_GAME_APU_SYNC_H

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

/* Writes <prefix>.aram, .dsp, .written, and .audio.jsonl for the Go
 * `apu-audit` command. Set SNESRECOMP_APU_AUDIT_PREFIX before APU creation so
 * write provenance and historical events are collected from reset onward. */
int RtlCaptureApuAudit(const char *prefix);

#ifdef __cplusplus
}
#endif

#endif
