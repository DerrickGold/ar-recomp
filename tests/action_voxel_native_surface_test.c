#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "action/action_voxel_native_surface.inc"
int main(void) {
  VoxelNativeSurfaceColumn columns[16]={0};uint16_t pixels[256];
  for(unsigned x=3;x<7;++x) {
    columns[x].length=16;
    for(unsigned d=0;d<16;++d)columns[x].colors[d]=(uint16_t)(0x8000|x*32+d);
  }
  for(unsigned side=0;side<4;++side) {
    assert(NativeSurfacePack(columns,3,4,side,16,true,pixels));
    for(unsigned x=0;x<16;++x)for(unsigned d=0;d<16;++d) {
      const unsigned source=x<3?3:x>6?6:x;
      const unsigned at=side==0?d*16+x:side==1?x*16+15-d:side==2?(15-d)*16+x:x*16+d;
      assert(pixels[at]==(uint16_t)(0x8000|source*32+d));
    }
  }
  columns[3].length=2;
  assert(NativeSurfacePack(columns,3,1,0,16,true,pixels));
  for(unsigned d=0;d<16;++d)assert(pixels[d*16+3]==columns[3].colors[d?1:0]);
  memset(pixels,0x52,sizeof(pixels));uint16_t unchanged[256];memcpy(unchanged,pixels,sizeof(pixels));
  assert(!NativeSurfacePack(columns,3,1,0,8,false,pixels));assert(!memcmp(unchanged,pixels,sizeof(pixels)));
  columns[3].length=8;assert(NativeSurfacePack(columns,3,1,0,8,false,pixels));
  static const unsigned rows[16]={0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7};
  for(unsigned d=0;d<16;++d)assert(pixels[d*16+3]==columns[3].colors[rows[d]]);
  assert(!NativeSurfacePack(columns,15,2,0,8,true,pixels));
  assert(!NativeSurfacePack(columns,3,1,4,8,true,pixels));
  assert(!NativeSurfacePack(columns,3,1,0,0,true,pixels));
  columns[3].colors[4]=0;assert(!NativeSurfacePack(columns,3,1,0,8,false,pixels));
  /* Ice endpoints are variable native profiles, never a chosen flat donor. */
  memset(columns,0,sizeof(columns));bool terminal[16]={0};uint16_t exact8[256];
  for(unsigned length=1;length<=8;++length) {
    columns[3].length=(uint8_t)length;terminal[3]=length<8;
    for(unsigned d=0;d<length;++d)columns[3].colors[d]=(uint16_t)(0x8100+d*19);
    assert(NativeSurfacePackIceTip(columns,terminal,3,1,0,pixels));
    assert(pixels[3]==columns[3].colors[0]);assert(pixels[15*16+3]==columns[3].colors[length-1]);
    for(unsigned d=1;d<16;++d)assert(pixels[d*16+3]>=pixels[(d-1)*16+3]);
    if(length==8) {assert(NativeSurfacePack(columns,3,1,0,8,false,exact8));assert(!memcmp(exact8,pixels,sizeof(pixels)));}
    else {terminal[3]=false;assert(!NativeSurfacePackIceTip(columns,terminal,3,1,0,pixels));}
  }
  terminal[3]=true;columns[3].length=0;assert(!NativeSurfacePackIceTip(columns,terminal,3,1,0,pixels));
  columns[3].length=3;columns[3].colors[1]=0;assert(!NativeSurfacePackIceTip(columns,terminal,3,1,0,pixels));
  return 0;
}
