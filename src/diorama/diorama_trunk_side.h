#ifndef AR_DIORAMA_TRUNK_SIDE_H
#define AR_DIORAMA_TRUNK_SIDE_H
#include <math.h>

/* Side passages meet the existing shaft at its actor/rear-rim plane. Their
 * additional depth opens over two native cells, in immutable world space. */
static inline float DioramaTrunk_SideDepth(float recess, float world_x) {
  const float opening = world_x < 320 ? (320-world_x)/32 :
      world_x > 704 ? (world_x-704)/32 : 0;
  return fminf(.18f, fmaxf(0,recess)*.75f) * fminf(1,fmaxf(0,opening));
}
#endif
