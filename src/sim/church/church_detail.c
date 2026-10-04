#include "sim/church/church_detail.h"
#include <math.h>
#include <float.h>

void ChurchFocus_Begin(ChurchFocus *f, float left, float right, float bottom, float top) {
  *f = (ChurchFocus){.left = left, .right = right, .bottom = bottom, .top = top};
  for (unsigned i = 0; i < kChurchFocusWidth * kChurchFocusHeight; i++)
    f->near_depth[i] = FLT_MAX;
}
static float Cross(float x, float y, float ax, float ay, float bx, float by) {
  return (x - ax) * (by - ay) - (y - ay) * (bx - ax);
}
static void Triangle(ChurchFocus *f, const Scene3DClipPoint p[3]) {
  float x[3], y[3];
  for (int i = 0; i < 3; i++) {
    /* The source is only outside the doorway; anything behind the camera is
     * irrelevant. Straddling near triangles are conservatively ignored. */
    if (p[i].w <= .4f) return;
    x[i] = (p[i].x / p[i].w - f->left) * kChurchFocusWidth / (f->right - f->left);
    y[i] = (p[i].y / p[i].w - f->bottom) * kChurchFocusHeight / (f->top - f->bottom);
  }
  const float area = Cross(x[0], y[0], x[1], y[1], x[2], y[2]);
  if (fabsf(area) < .0001f) return;
  int x0 = (int)fmaxf(0, floorf(fminf(x[0], fminf(x[1], x[2]))));
  int y0 = (int)fmaxf(0, floorf(fminf(y[0], fminf(y[1], y[2]))));
  int x1 = (int)fminf(kChurchFocusWidth - 1, ceilf(fmaxf(x[0], fmaxf(x[1], x[2]))));
  int y1 = (int)fminf(kChurchFocusHeight - 1, ceilf(fmaxf(y[0], fmaxf(y[1], y[2]))));
  for (int row = y0; row <= y1; row++)
    for (int col = x0; col <= x1; col++) {
      float a = Cross(col + .5f, row + .5f, x[1], y[1], x[2], y[2]) / area;
      float b = Cross(col + .5f, row + .5f, x[2], y[2], x[0], y[0]) / area;
      float c = 1 - a - b;
      if (a < -.001f || b < -.001f || c < -.001f) continue;
      float depth = 1 / (a / p[0].w + b / p[1].w + c / p[2].w);
      float *nearest = &f->near_depth[row * kChurchFocusWidth + col];
      if (depth < *nearest) *nearest = depth;
    }
}
void ChurchFocus_Quad(ChurchFocus *f, const Scene3DClipPoint p[4]) {
  const Scene3DClipPoint a[3] = {p[0], p[1], p[2]}, b[3] = {p[0], p[2], p[3]};
  Triangle(f, a);
  Triangle(f, b);
}
float ChurchFocus_Blur(const ChurchFocus *f, float x, float y) {
  const float u = fminf(kChurchFocusWidth - 1,
                        fmaxf(0, (x - f->left) / (f->right - f->left) * kChurchFocusWidth));
  const float v = fminf(kChurchFocusHeight - 1,
                        fmaxf(0, (y - f->bottom) / (f->top - f->bottom) * kChurchFocusHeight));
  /* Nearest-depth dilation keeps a distant blur from eating near silhouettes. */
  float depth = FLT_MAX;
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
      int ix = (int)u + dx, iy = (int)v + dy;
      if (ix >= 0 && ix < kChurchFocusWidth && iy >= 0 && iy < kChurchFocusHeight)
        depth = fminf(depth, f->near_depth[iy * kChurchFocusWidth + ix]);
    }
  float t = fminf(1, fmaxf(0, (depth - 75) / 260));
  return .60f + .28f * t * t * (3 - 2 * t);
}
ArRenderColorF ChurchDetail_Stone(ArRenderColorF lit, unsigned grain) {
  static const unsigned palette[] = {0x312921, 0x524231, 0x73634a, 0x948463, 0xad9c7b,
                                     0xbdae8c, 0xcebd9c, 0xe0cfa8, 0xf1ddb0, 0xffe9bd};
  float best = FLT_MAX;
  ArRenderColorF result = lit;
  const float noise = ((int)(grain % 5) - 2) * .003f;
  for (unsigned i = 0; i < sizeof(palette) / sizeof(*palette); i++) {
    unsigned rgb = palette[i];
    ArRenderColorF c = {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
                        (rgb & 255) / 255.0f, lit.a};
    float dr = c.r - lit.r - noise, dg = c.g - lit.g - noise, db = c.b - lit.b - noise;
    float d = dr * dr + dg * dg + db * db;
    if (d < best) {
      best = d;
      result = c;
    }
  }
  return result;
}
unsigned ChurchDetail_Phase(double seconds, unsigned frames, unsigned rate) {
  if (!frames || !rate || !isfinite(seconds) || seconds <= 0) return 0;
  return (unsigned)fmod(floor(seconds * rate), frames);
}
