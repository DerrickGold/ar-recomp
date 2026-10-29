#ifndef AR_DIORAMA_CLOUD_LIGHT_H
#define AR_DIORAMA_CLOUD_LIGHT_H
/* Cloud material uniforms and shared sampling for sky artwork and cloud geometry. */
#include "render/render_device.h"
#include <math.h>

enum { kDioramaCloudLights=4 };
/* std140 vec4 layout, shared by the sky and ordinary BG2 material. */
typedef struct DioramaCloudLightRegion {
  float bounds[4]; /* world x,y,width,height */
  float core[4]; /* edge, halo, streak and reflection strengths */
  float shape[4]; /* falloff, cloud cover, edge and reflection positions */
  float motion[4]; /* slope, phase, seed, opening count */
  float color[4]; /* luminous edge RGB, overall strength */
  float spill[4]; /* amber spill RGB, softness */
} DioramaCloudLightRegion;
typedef struct DioramaCloudLight {
  float source[4]; /* sampled texture's world origin XY, width/height */
  float meta[4]; /* count, maximum background dimming, reserved */
  DioramaCloudLightRegion regions[kDioramaCloudLights];
  float background_dim[kDioramaCloudLights]; /* strongest overlapping region wins */
} DioramaCloudLight;
typedef struct CloudLightSample { float edge,halo,streak,echo; } CloudLightSample;
typedef struct CloudLightOpening { float centre,width,power,offset,reach; } CloudLightOpening;
#define CLOUD_INLINE static inline
#define CLOUD_CLAMP(x,a,b) fminf(b,fmaxf(a,x))
#define CLOUD_MAX fmaxf
#define CLOUD_EXP expf
#define CLOUD_SIN sinf
#define CLOUD_FLOAT(x) ((float)(x))
#define CLOUD_UINT_TYPE uint32_t
#define CLOUD_UINT(x) ((uint32_t)(x))
#define CLOUD_OPENING(a,b,c,d,e) ((CloudLightOpening){a,b,c,d,e})
#define CLOUD_SAMPLE(a,b,c,d) ((CloudLightSample){a,b,c,d})
#include "render/cloud_light_math.inc"
#undef CLOUD_INLINE
#undef CLOUD_CLAMP
#undef CLOUD_MAX
#undef CLOUD_EXP
#undef CLOUD_SIN
#undef CLOUD_FLOAT
#undef CLOUD_UINT_TYPE
#undef CLOUD_UINT
#undef CLOUD_OPENING
#undef CLOUD_SAMPLE
static inline float DioramaCloudLight_Seed(uint32_t seed) {
  /* Float uniforms exactly represent this entire range on all backends. */
  return (float)(CloudLightHash(seed)&0x00ffffffu);
}
static inline bool DioramaCloudLight_Active(const DioramaCloudLight *light) {
  return light->meta[0]>0;
}
static inline CloudLightSample DioramaCloudLight_Sample(
    const DioramaCloudLightRegion *r,float x,float y) {
  return CloudLightWeights((x-r->bounds[0])/r->bounds[2],(y-r->bounds[1])/r->bounds[3],
      r->bounds[3]/r->bounds[2],r->motion[0],r->motion[1],r->motion[2],(int)r->motion[3],
      r->spill[3],r->shape[0],r->shape[1],r->shape[2],r->shape[3]);
}

#endif
