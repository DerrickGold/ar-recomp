#ifndef AR_CLOUD_MATERIAL_H
#define AR_CLOUD_MATERIAL_H

/* Shared CPU cloud baker. Materials describe density; representations choose
 * paired alpha masks (two batched quads) or lit depth slices plus a flat tile.
 * Call at preparation time. Rendering, cache lifetime and anchoring belong to
 * the caller. No GPU API, allocation, global scratch or game state. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { kCloudVolumeMaxLobes=14, kCloudVolumeGrid=40,
       kCloudVolumeVoxels=kCloudVolumeGrid*kCloudVolumeGrid*kCloudVolumeGrid };
typedef struct CloudVolumeLobe { float x,y,z,rx,ry,rz; } CloudVolumeLobe;
typedef struct CloudVolumeOctave {
  float scale[3],offset[3],weight;
  uint32_t seed;
} CloudVolumeOctave;
typedef enum CloudVolumeDensity {
  kCloudVolumeDensity_Billow, /* seeded, feathered density with added turbulence */
  kCloudVolumeDensity_Crown,  /* eroded lobes with a flattened underside */
} CloudVolumeDensity;
typedef struct CloudVolumeMaterial {
  CloudVolumeLobe lobes[kCloudVolumeMaxLobes];
  unsigned lobe_count;
  CloudVolumeDensity density;
  CloudVolumeOctave noise[3];
  float shape_floor,shape_cutoff,noise_bias,noise_gain;
  float ramp_start,ramp_end,density_gain;
  float feather[3]; /* left/right, top, bottom (billows) */
  float floor_start,floor_fade; /* crown underside */
} CloudVolumeMaterial;

/* Existing artwork as explicit material presets. Callers may adjust a copy.
 * Variant is deterministic and never comes from the camera or frame clock. */
void CloudMaterial_Billows(unsigned variant,CloudVolumeMaterial *out);
void CloudMaterial_Crown(unsigned variant,CloudVolumeMaterial *out);
/* Sampling requires a prepared finite material and finite coordinates. Bake
 * entry points validate once before their hot loops. Sheet samples require
 * size >= 2 and 1..12 octaves; callers fix these at texture preparation. */
float CloudMaterial_Density(const CloudVolumeMaterial *material,float x,float y,float z);
float CloudMaterial_Wisps(float u,float v,uint32_t seed);
uint32_t CloudMaterial_SheetTexel(int x,int y,int size,unsigned octaves);

typedef struct CloudVolumeScratch {
  float density[kCloudVolumeVoxels],optical[kCloudVolumeVoxels];
} CloudVolumeScratch;
typedef struct CloudMaskLight {
  float shear_x,shear_z,depth_step,opacity_step,ambient,diffuse,extinction;
} CloudMaskLight;
extern const CloudMaskLight kCloudBillowLight;
/* Views into a caller-owned A8 atlas, pitch in bytes. Body followed by light
 * under alpha-over reconstructs density without double opacity at the edges.
 * Scratch is reusable across variants, but must have only one writer. */
bool CloudMaterial_BakeMasks(const CloudVolumeMaterial *material,const CloudMaskLight *light,
    CloudVolumeScratch *scratch,uint8_t *body,uint8_t *lit,int pitch,int width,int height);

typedef struct CloudSliceLayout { int width,height,slices,columns; } CloudSliceLayout;
typedef struct CloudSliceLight {
  float direction[3],ambient[3],diffuse[3];
  float step,extinction,optical_step;
  unsigned probes;
  bool enabled;
} CloudSliceLight;
/* One bank of ARGB slices, followed by a precomposited tile. One transparent
 * gutter texel per tile, unused tiles transparent. Pitch in uint32_t pixels. */
bool CloudMaterial_BakeSlices(const CloudVolumeMaterial *material,const CloudSliceLight *light,
    CloudSliceLayout layout,uint32_t *out,int pitch);
typedef struct CloudTexelBounds { int x0,y0,x1,y1; } CloudTexelBounds;
/* Conservative bilinear footprint with one-texel padding. Empty => all zero. */
bool CloudMaterial_Bounds(const uint32_t *pixels,int pitch,int width,int height,
    CloudTexelBounds *bounds);
#endif
