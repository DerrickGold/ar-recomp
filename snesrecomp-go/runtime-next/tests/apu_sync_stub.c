#include "apu_sync.h"

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
void RtlApuWrite(uint16_t address, uint8_t value) {
    (void)address;
    (void)value;
}
void rtl_accumulate_apu_catchup(void) {}
