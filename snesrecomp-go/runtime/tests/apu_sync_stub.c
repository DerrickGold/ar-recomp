#include "snesrecomp/game/apu_sync.h"

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void RtlApuWrite(uint16_t address, uint8_t value) {
    (void)address;
    (void)value;
}
void rtl_accumulate_apu_catchup(void) {}
int RtlCaptureApuAudit(const char *prefix) {
    (void)prefix;
    return 1;
}
