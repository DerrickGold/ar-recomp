#include "diorama_capture_blend.h"

bool DioramaCaptureBlend_IsHalfAddWithSubscreen(uint8_t cgwsel,
                                               uint8_t cgadsub) {
  return (cgwsel & kCgwselAddendIsSubscreen) != 0 &&
         (cgadsub & kCgadsubHalf) != 0 &&
         (cgadsub & kCgadsubSubtract) == 0;
}

bool DioramaCaptureBlend_LayerIsHalfAdded(uint8_t cgwsel, uint8_t cgadsub,
                                         uint8_t screen_sub,
                                         uint8_t layer_bit) {
  if (!layer_bit) return false;
  if (!DioramaCaptureBlend_IsHalfAddWithSubscreen(cgwsel, cgadsub))
    return false;
  if (!(cgadsub & layer_bit)) return false;
  /* Half-added with itself is the identity -- see the header. */
  if (screen_sub & layer_bit) return false;
  return true;
}
