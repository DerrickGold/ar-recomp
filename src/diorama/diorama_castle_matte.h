#ifndef AR_DIORAMA_CASTLE_MATTE_H
#define AR_DIORAMA_CASTLE_MATTE_H
#include <stdbool.h>
#include <math.h>

/* This does not generate or extend masonry. The compiler must
 * independently prove the terminal native run is opaque black, low priority,
 * noncolliding, unchanged by authored paint/backing controls, and connected to
 * the existing enclosed void. Only the reviewed Castle02/07 profile opts in.
 * Coordinates are world pixels on the existing rear plane, not screen pixels.
 */
typedef struct CastleTerminalMatteRect {float x0,y0,x1,y1;} CastleTerminalMatteRect;
typedef struct CastleTerminalMatteRuns {
  float world_right,world_bottom;
  float bottom_start,right_start;
  bool bottom_opaque_black,right_opaque_black;
} CastleTerminalMatteRuns;

static inline unsigned CastleTerminalMatte_Resolve(unsigned group,unsigned room,
    bool active_voxel,CastleTerminalMatteRuns source,CastleTerminalMatteRect view,
    CastleTerminalMatteRect output[2]) {
  if(!output)return 0;
  output[0]=output[1]=(CastleTerminalMatteRect){0};
  if(!active_voxel||group!=2||room!=7||
      !isfinite(source.world_right)||!isfinite(source.world_bottom)||
      !isfinite(source.bottom_start)||!isfinite(source.right_start)||
      !isfinite(view.x0)||!isfinite(view.x1)||!isfinite(view.y0)||!isfinite(view.y1)||
      view.x0>=view.x1||view.y0>=view.y1)return 0;
  unsigned count=0;
  /* Linear filtering fades the final native texel toward transparent outside
   * the map. Cover that one texel only where the compiler proved an opaque
   * black terminal run. Never overlap colored art or unrelated interior.
   * The corner remains owned once, by the bottom continuation. */
  const bool extend_bottom=source.bottom_opaque_black&&
      source.bottom_start<source.world_right&&view.y1>source.world_bottom;
  const float bottom_join=source.world_bottom-1;
  if(extend_bottom) {
    CastleTerminalMatteRect r={fmaxf(view.x0,source.bottom_start),
      fmaxf(view.y0,bottom_join),
      source.right_opaque_black?view.x1:fminf(view.x1,source.world_right),view.y1};
    if(r.x0<r.x1&&r.y0<r.y1)output[count++]=r;
  }
  if(source.right_opaque_black&&source.right_start<source.world_bottom&&
      view.x1>source.world_right) {
    CastleTerminalMatteRect r={fmaxf(view.x0,source.world_right-1),
      fmaxf(view.y0,source.right_start),view.x1,
      fminf(view.y1,extend_bottom?bottom_join:source.world_bottom)};
    if(r.x0<r.x1&&r.y0<r.y1)output[count++]=r;
  }
  return count;
}
#endif
