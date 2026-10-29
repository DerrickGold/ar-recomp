#ifndef AR_MIST_RAINBOW_H
#define AR_MIST_RAINBOW_H
#include <math.h>
#include "render_types.h"
typedef struct RainbowDirection { float x,y,z; } RainbowDirection;
#define RAINBOW_INLINE static inline
#define RAINBOW_VEC3 RainbowDirection
#define RAINBOW_VEC4 ArRenderColorF
#define RAINBOW_V3(x,y,z) ((RainbowDirection){x,y,z})
#define RAINBOW_V4(r,g,b,a) ((ArRenderColorF){r,g,b,a})
#define RAINBOW_EXP expf
#define RAINBOW_SQRT sqrtf
#define RAINBOW_MAX fmaxf
#include "mist_rainbow_math.inc"
#undef RAINBOW_INLINE
#undef RAINBOW_VEC3
#undef RAINBOW_VEC4
#undef RAINBOW_V3
#undef RAINBOW_V4
#undef RAINBOW_EXP
#undef RAINBOW_SQRT
#undef RAINBOW_MAX
#endif
