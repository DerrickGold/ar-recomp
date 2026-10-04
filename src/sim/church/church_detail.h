#ifndef AR_CHURCH_DETAIL_H
#define AR_CHURCH_DETAIL_H
#include "render/scene3d_math.h"
#include "render/render_device.h"
/* Fixed, small CPU depth proxy, prepared only when captured geometry changes.
 * Coordinates are clip x/w times aspect and clip y/w: independent of output size. */
enum { kChurchFocusWidth = 32, kChurchFocusHeight = 48 };
typedef struct ChurchFocus {
  float near_depth[kChurchFocusWidth * kChurchFocusHeight];
  float left, right, bottom, top;
} ChurchFocus;
void ChurchFocus_Begin(ChurchFocus *focus, float left, float right, float bottom, float top);
void ChurchFocus_Quad(ChurchFocus *focus, const Scene3DClipPoint points[4]);
float ChurchFocus_Blur(const ChurchFocus *focus, float x, float y);
ArRenderColorF ChurchDetail_Stone(ArRenderColorF lit, unsigned grain);
unsigned ChurchDetail_Phase(double seconds, unsigned frames, unsigned rate);
#endif
