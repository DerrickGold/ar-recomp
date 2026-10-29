#include "render/cloud_material.h"
#include "render/cloud_mesh.h"
#include "render/cloud_motion.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Samples captured from the independent pre-consolidation bakers, 2026-10-28.
 * A one-unit channel tolerance permits libm rounding on other targets. Full
 * byte comparisons of the original atlases live in the review run. */
static void CheckColor(uint32_t a,uint32_t b) {
  for(unsigned i=0;i<4;++i)
    assert(abs((int)((a>>(i*8))&255)-(int)((b>>(i*8))&255))<=1);
}
static void TestMasks(void) {
  static CloudVolumeScratch scratch;
  enum { kSize=128,kPitch=131 };
  uint8_t body[kSize*kPitch],lit[kSize*kPitch];
  const unsigned variants[]={0,3,7};
  const unsigned xy[][2]={{32,64},{64,64},{94,56},{64,40},{40,88}};
  const uint8_t expected[3][5][2]={
    {{219,78},{238,121},{232,86},{154,137},{211,57}},
    {{241,114},{240,60},{226,103},{198,137},{153,29}},
    {{219,94},{231,86},{219,118},{131,125},{211,42}}};
  for(unsigned v=0;v<3;++v) {
    memset(body,0xa5,sizeof(body));memset(lit,0xa5,sizeof(lit));
    CloudVolumeMaterial material;CloudMaterial_Billows(variants[v],&material);
    assert(CloudMaterial_BakeMasks(&material,&kCloudBillowLight,&scratch,body,lit,kPitch,kSize,kSize));
    for(unsigned i=0;i<5;++i) {
      const unsigned at=xy[i][1]*kPitch+xy[i][0];
      assert(abs(body[at]-expected[v][i][0])<=1);
      assert(abs(lit[at]-expected[v][i][1])<=1);
    }
    for(unsigned y=0;y<kSize;++y)for(unsigned x=0;x<kPitch;++x) {
      const unsigned at=y*kPitch+x;
      if(x>=kSize)assert(body[at]==0xa5&&lit[at]==0xa5);
      else if(!x||!y||x==kSize-1||y==kSize-1)assert(!body[at]&&!lit[at]);
    }
    /* Runtime tint strength must not be baked into geometry or mask opacity. */
    CloudMaskLight dark=kCloudBillowLight;dark.ambient=0;dark.diffuse=0;
    uint8_t unlit[kSize*kPitch],zero[kSize*kPitch];
    assert(CloudMaterial_BakeMasks(&material,&dark,&scratch,unlit,zero,kPitch,kSize,kSize));
    for(unsigned y=0;y<kSize;++y)for(unsigned x=0;x<kSize;++x) {
      const unsigned at=y*kPitch+x;
      const float combined=lit[at]+body[at]*(1-lit[at]/255.f);
      assert(zero[at]==0&&fabsf(combined-unlit[at])<=1.5f);
    }
    const uint8_t before=body[64*kPitch+64];
    assert(!CloudMaterial_BakeMasks(&material,&dark,NULL,body,lit,kPitch,kSize,kSize));
    material.lobes[0].rx=0;
    assert(!CloudMaterial_BakeMasks(&material,&dark,&scratch,body,lit,kPitch,kSize,kSize));
    assert(body[64*kPitch+64]==before);
  }
}
static size_t Tile(CloudSliceLayout l,unsigned slice,unsigned x,unsigned y,unsigned pitch) {
  return (slice/l.columns*l.height+y)*pitch+slice%l.columns*l.width+x;
}
static void TestSlices(void) {
  const CloudSliceLayout layout={96,48,16,4};
  const CloudSliceLight light={.direction={0,-.90f,-.43589f},.ambient={.58f,.70f,.85f},
    .diffuse={.42f,.29f,.13f},.step=.1f,.extinction=8,.optical_step=.55f,.probes=6,.enabled=true};
  enum { kPitch=387,kHeight=240 };
  uint32_t *pixels=malloc(kPitch*kHeight*sizeof(*pixels));assert(pixels);
  const uint32_t expected[4][5]={
    {0x64d1ddec,0x64b7cbe4,0x41e4eaf1,0xfac4d4e8,0xf6e4eaf1},
    {0x64d1ddec,0x64aac2e0,0x64a8c1df,0xfce6ebf2,0xf8cddaea},
    {0x64bccee5,0x64b8cbe4,0x64e4eaf1,0xfbd8e2ee,0xf5fbfaf9},
    {0x1edfe6f0,0x64b2c7e2,0x64b3c8e2,0xebb9cce4,0xfbcddaea}};
  const unsigned xyz[][3]={{6,40,24},{8,48,24},{10,60,22},{16,40,24},{16,60,22}};
  for(unsigned bank=0;bank<4;++bank) {
    memset(pixels,0xa5,kPitch*kHeight*sizeof(*pixels));
    CloudVolumeMaterial material;CloudMaterial_Crown(bank,&material);
    assert(CloudMaterial_BakeSlices(&material,&light,layout,pixels,kPitch));
    for(unsigned i=0;i<5;++i)
      CheckColor(pixels[Tile(layout,xyz[i][0],xyz[i][1],xyz[i][2],kPitch)],expected[bank][i]);
    for(unsigned y=0;y<kHeight;++y)for(unsigned x=384;x<kPitch;++x)
      assert(pixels[y*kPitch+x]==0xa5a5a5a5);
    for(unsigned slice=0;slice<=16;++slice) {
      CloudTexelBounds bounds;
      const uint32_t *tile=pixels+Tile(layout,slice,0,0,kPitch);
      assert(CloudMaterial_Bounds(tile,kPitch,96,48,&bounds));
      for(int y=0;y<48;++y)for(int x=0;x<96;++x)
        if(tile[y*kPitch+x]>>24)assert(x>=bounds.x0&&x<=bounds.x1&&y>=bounds.y0&&y<=bounds.y1);
    }
    CloudSliceLight bad=light;bad.direction[1]=NAN;
    const uint32_t before=pixels[Tile(layout,16,40,24,kPitch)];
    assert(!CloudMaterial_BakeSlices(&material,&bad,layout,pixels,kPitch));
    assert(pixels[Tile(layout,16,40,24,kPitch)]==before);
    /* Density presets and render representation are independent: the same
     * billowed material also supports real slices and an arbitrary layout. */
    CloudMaterial_Billows(bank,&material);
    assert(CloudMaterial_BakeSlices(&material,&light,(CloudSliceLayout){32,24,8,3},pixels,kPitch));
  }
  free(pixels);
}
static void TestSheetAndMotion(void) {
  for(int i=0;i<512;i+=7) {
    assert(CloudMaterial_SheetTexel(0,i,512,5)==CloudMaterial_SheetTexel(511,i,512,5));
    assert(CloudMaterial_SheetTexel(i,0,512,5)==CloudMaterial_SheetTexel(i,511,512,5));
  }
  CloudBankMotion motion={.x=-256,.y=-64,.width=2048,.height=128,.drift=-34,
    .amplitude=3,.rolling=.8f,.intensity=1.15f,.strands=24,.volume=true};
  for(unsigned i=0;i<motion.strands;++i) {
    CloudBankPatch first[2];CloudMotion_Bank(&motion,i,i*37,first);
    for(unsigned t=0;t<12;++t) {
      CloudBankMotion at=motion;at.phase=t*.5f;at.wind_phase=sinf(at.phase);
      CloudBankPatch p[2];CloudMotion_Bank(&at,i,i*37,p);
      assert(p[0].x==p[1].x&&p[0].y==p[1].y&&p[0].rx==p[1].rx&&p[0].ry==p[1].ry);
      assert(p[0].rx==first[0].rx&&p[0].ry==first[0].ry);
      assert(p[0].x-p[0].rx>=at.x&&p[0].x+p[0].rx<=at.x+at.width);
      assert(p[0].y-p[0].ry>=at.y&&p[0].y+p[0].ry<=at.y+at.height);
    }
  }
}
int main(void) {
  TestMasks();TestSlices();TestSheetAndMotion();
  puts("cloud_material_test: PASS");
}
