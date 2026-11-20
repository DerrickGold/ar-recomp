#ifndef AR_ACTION_NATIVE_CONTACT_H
#define AR_ACTION_NATIVE_CONTACT_H
#include <stdbool.h>

/* Native $00:90A9/$9084 surface height within a sixteen-pixel metatile.
 * Matching neighbours select the paired 1:2 ramps, with upper priority;
 * otherwise a ramp is 1:1. Sixteen denotes an empty collision column. */
static inline bool ActionNativeContact_IsRamp(unsigned collision) {
  return collision==6||collision==9;
}
static inline unsigned ActionNativeContact_Top(unsigned collision,unsigned left,unsigned right,unsigned x) {
  x&=15;
  if(ActionNativeContact_IsRamp(collision)) {
    const unsigned phase=collision==6?(x&14):((~x)&14);
    const bool upper=collision==6?left==6:right==9;
    const bool lower=collision==6?right==6:left==9;
    return 16-(upper?8+phase/2:lower?phase/2:phase);
  }
  if(collision&(1u<<(x/8)))return 0;
  return collision&(1u<<(2+x/8))?8:16;
}
#endif
