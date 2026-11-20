#ifndef AR_DIORAMA_TRUNK_SETTINGS_H
#define AR_DIORAMA_TRUNK_SETTINGS_H
#include <stdbool.h>
#include <math.h>

/* A room-owned enclosure. Ordinary BG fill, skybox and plane-shape policies
 * do not describe this connected surface and cannot override it. */
typedef struct DioramaTrunkSettings {
  bool set, enabled;
  float depth, recess, wrap, gust, frost, bark;
} DioramaTrunkSettings;

static inline bool DioramaTrunk_Room(unsigned group, unsigned room) {
  return group == 6 && (room == 6 || room == 7);
}

static inline DioramaTrunkSettings DioramaTrunk_Default(void) {
  return (DioramaTrunkSettings){.enabled=true, .depth=.38f, .recess=.24f,
      .wrap=1, .gust=1, .frost=1, .bark=1};
}
static inline float DioramaTrunk_BranchDepth(float recess) {
  return fminf(.065f,fmaxf(0,recess)*.3f);
}
static inline float DioramaTrunk_RimDepth(float recess,bool voxel) {
  return fmaxf(0,recess)*(voxel?.5f:1);
}
static inline float DioramaTrunk_RimFront(float recess,float x,bool voxel) {
  if(!voxel)return 0;
  /* Two-pixel facets share exact endpoints with contour caps and frost. The
   * native inner/outer edges meet the rear plane; the trunk center stays out. */
  const float left=floorf(x/2)*2,t=(x-left)/2,center=x<512?336:688;
  const float a=(left-center)/16,b=(left+2-center)/16;
  const float round=(1-t)*sqrtf(fmaxf(0,1-a*a))+t*sqrtf(fmaxf(0,1-b*b));
  return -fmaxf(0,recess)+DioramaTrunk_RimDepth(recess,true)*round;
}
#endif
