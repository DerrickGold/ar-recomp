#include "sim/world_nav/sim_world_navigation_sky_clouds.h"
#include "render/cloud_material.h"
#include <math.h>
#include <stddef.h>

_Static_assert(kSimSkyCloudWidth-1<=UINT8_MAX&&kSimSkyCloudHeight-1<=UINT8_MAX,
    "slice-local support bounds must fit their portable coordinates");
static size_t Texel(int bank,int slice,int pitch) {
  return (size_t)(bank*kSimSkyCloudRows*kSimSkyCloudHeight+
      slice/kSimSkyCloudColumns*kSimSkyCloudHeight)*pitch+
      slice%kSimSkyCloudColumns*kSimSkyCloudWidth;
}
bool SimWorldNavigationSkyClouds_Bounds(const uint32_t *pixels,int pitch,
    int bank,int slice,SimSkyCloudBounds *bounds) {
  if(!pixels||!bounds||pitch<kSimSkyCloudAtlasWidth||
      (size_t)pitch>SIZE_MAX/sizeof(*pixels)/kSimSkyCloudAtlasHeight||
      bank<0||bank>=kSimSkyCloudBanks||slice<0||slice>kSimSkyCloudSlices)return false;
  CloudTexelBounds b;
  if(!CloudMaterial_Bounds(pixels+Texel(bank,slice,pitch),pitch,kSimSkyCloudWidth,kSimSkyCloudHeight,&b))return false;
  *bounds=(SimSkyCloudBounds){b.x0,b.y0,b.x1,b.y1};
  return true;
}
bool SimWorldNavigationSkyClouds_Bake(uint32_t *out,int pitch,const float light[3],bool lighting) {
  if(!out||!light||pitch<kSimSkyCloudAtlasWidth||
      (size_t)pitch>SIZE_MAX/sizeof(*out)/kSimSkyCloudAtlasHeight)return false;
  const CloudSliceLight sun={.direction={light[0],light[1],light[2]},
      .ambient={.58f,.70f,.85f},.diffuse={.42f,.29f,.13f},
      .step=.1f,.extinction=8,.optical_step=.55f,.probes=6,.enabled=lighting};
  const CloudSliceLayout layout={kSimSkyCloudWidth,kSimSkyCloudHeight,kSimSkyCloudSlices,kSimSkyCloudColumns};
  for(unsigned bank=0;bank<kSimSkyCloudBanks;++bank) {
    CloudVolumeMaterial material;
    CloudMaterial_Crown(bank,&material);
    if(!CloudMaterial_BakeSlices(&material,&sun,layout,out+Texel(bank,0,pitch),pitch))return false;
  }
  return true;
}
