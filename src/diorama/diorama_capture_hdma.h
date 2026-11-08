#ifndef AR_DIORAMA_CAPTURE_HDMA_H
#define AR_DIORAMA_CAPTURE_HDMA_H

#include <stdbool.h>
#include <stdint.h>

/* Independent Voxel sources require stable screen membership and color math
 * for the whole scanout. The caller checks every active HDMA channel and keeps
 * the legacy winner capture if the runner cannot provide a complete DMA view.
 * This checks B-bus destinations only; direct/indirect tables and transfer
 * direction cannot make an unsafe destination eligible.
 *
 * Reject INIDISP ($00: forced blank/brightness), BGMODE ($05: native priority),
 * W12SEL..WOBJLOG ($23..$2b), TM/TS/TMW/TSW ($2c..$2f), CGWSEL/CGADSUB/COLDATA
 * ($30..$32), and SETINI ($33: screen mode). Scroll, mosaic, and palette-only
 * HDMA remain eligible. COLDATA is relevant even with a subscreen addend when
 * that screen has no visible winner.
 */
static inline bool DioramaCaptureHdma_ChannelSafe(uint8_t b_address,uint8_t mode) {
  /* Each mode touches one of three contiguous register sets: {0}, {0,1}, or
   * {0,1,2,3}. Repeated writes/order do not change this safety predicate. */
  static const uint8_t last_offset[8]={0,1,0,1,3,1,0,1};
  if(mode>=8)return false;
  for(unsigned offset=0;offset<=last_offset[mode];++offset) {
    const uint8_t target=(uint8_t)(b_address+offset);
    if(target==0x00 || target==0x05 || (target>=0x23 && target<=0x33))return false;
  }
  return true;
}

#endif
