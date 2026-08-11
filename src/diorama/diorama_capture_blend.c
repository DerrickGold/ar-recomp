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

uint8_t DioramaCaptureBlend_FullAddSubscreenSources(
    uint8_t cgwsel, uint8_t cgadsub,
    uint8_t screen_main, uint8_t screen_sub) {
  enum { kVisualSourceMask = 0x1f };
  if (cgwsel != kCgwselAddendIsSubscreen) return 0;
  if (cgadsub & (kCgadsubHalf | kCgadsubSubtract)) return 0;
  if (screen_main & screen_sub & kVisualSourceMask) return 0;
  /* At least one visible main-screen source must actually select colour math;
   * otherwise the subscreen is configured but never contributes. */
  if (!(screen_main & cgadsub & kCgadsubLayerMask)) return 0;
  return screen_sub & kVisualSourceMask;
}

bool DioramaCaptureBlend_LayerUsesFixedColorSubtract(
    uint8_t cgwsel, uint8_t cgadsub, uint16_t fixed_color,
    uint8_t layer_bit) {
  if (!layer_bit || !fixed_color || cgwsel != 0) return false;
  if (!(cgadsub & layer_bit)) return false;
  return (cgadsub & (kCgadsubHalf | kCgadsubSubtract)) ==
      kCgadsubSubtract;
}
