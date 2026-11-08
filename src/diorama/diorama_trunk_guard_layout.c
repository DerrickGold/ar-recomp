#include "diorama_trunk_guard.h"

#include <limits.h>
#include <string.h>

static bool End(int start, int size, int *end) {
  const int64_t result = (int64_t)start + size;
  if (size <= 0 || result > INT_MAX || result < INT_MIN) return false;
  *end = (int)result;
  return true;
}

bool DioramaTrunkGuard_Resolve(int world_x, int primary_y, int width,
    int primary_height, DioramaTrunkGuardBounds source,
    DioramaTrunkGuardLayout *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  int right, primary_end, source_right, source_bottom, bottom_end;
  if (width > kDioramaTrunkGuardWidth ||
      !End(world_x, width, &right) ||
      !End(primary_y, primary_height, &primary_end) ||
      !End(primary_end, kDioramaTrunkGuardRows, &bottom_end) ||
      primary_y < INT_MIN + kDioramaTrunkGuardRows ||
      !End(source.x, source.width, &source_right) ||
      !End(source.y, source.height, &source_bottom)) return false;
  (void)bottom_end;
  *out = (DioramaTrunkGuardLayout){
    .primary_y = primary_y, .primary_height = primary_height, .source = source
  };
  for (unsigned i = 0; i < kDioramaTrunkGuardStrips; ++i) {
    DioramaTrunkGuardStrip *strip = &out->strips[i];
    strip->world_x = world_x;
    strip->world_y = i ? primary_end : primary_y - kDioramaTrunkGuardRows;
    strip->width = width;
    strip->atlas_y = (int)i * kDioramaTrunkGuardRows;
    const int x0 = world_x > source.x ? world_x : source.x;
    const int x1 = right < source_right ? right : source_right;
    const int y0 = strip->world_y > source.y ? strip->world_y : source.y;
    const int end_y = strip->world_y + kDioramaTrunkGuardRows;
    const int y1 = end_y < source_bottom ? end_y : source_bottom;
    if (x1 > x0 && y1 > y0)
      strip->valid = (DioramaTrunkGuardBounds){
        x0 - world_x, y0 - strip->world_y, x1 - x0, y1 - y0
      };
  }
  return true;
}
