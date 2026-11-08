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

uint8_t DioramaCaptureBlend_IndependentSubscreenSources(bool voxel_ready,
    uint8_t cgwsel, uint8_t cgadsub,
    uint8_t screen_main, uint8_t screen_sub) {
  if(!voxel_ready)return 0;
  const uint8_t sources=DioramaCaptureBlend_FullAddSubscreenSources(
      cgwsel,cgadsub,screen_main,screen_sub);
  if(!sources || (sources&~0x13u) || (screen_main&0x18u))return 0;
  const unsigned main_world=screen_main&0x03u;
  if(!main_world || (main_world&cgadsub)!=main_world)return 0;
  return sources;
}
