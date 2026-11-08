#include "support/test_assert.h"
#include "diorama/diorama_scenery_uniforms.h"
#include "diorama_owned_heat_oracle.h"
#include <stdio.h>
#include <string.h>

static void Close(float actual,double expected,double tolerance) {
  if(fabs(actual-expected)>tolerance)fprintf(stderr,"owned heat actual%.9g expected%.12g tolerance%.4g\n",actual,expected,tolerance);
  assert(isfinite(actual)&&fabs(actual-expected)<=tolerance);
}
int main(void) {
  const DioramaHeat heat={.source={400,800,1024,2048},
      .bounds={{70,120,180,240},{95,155,160,205},{0,0,1,1},{20,20,20,80}},
      .motion={{3.2f,.713f,0,0},{4.1f,1.23f,0,0},{0,8,0,0},{10,8,0,0}}};
  unsigned samples=0,moved=0,wrapped=0,clamped=0;
  for(unsigned y=0;y<=36;++y)for(unsigned x=0;x<=36;++x) {
    const ArRenderPointF world={65+x*3.5f,115+y*3.75f};
    const OwnedHeatOraclePoint d=OwnedHeatOracleOffset(&heat,world.x,world.y);
    const ArRenderPointF actual=DioramaHeat_WorldOffset(&heat,world);
    Close(actual.x,d.x,.000025);Close(actual.y,d.y,.000005);
    for(unsigned mode=1;mode<=2;++mode)for(unsigned phase=0;phase<4;++phase) {
      const unsigned scale=mode==1?1:4;const DioramaHeatOwned policy={scale/512.f,scale/2048.f,(float)mode,(float)scale};
      const float u=(phase&1)?.125f:15.875f,v=(phase&2)?.125f:15.875f;
      const ArRenderPointF uv={(u+16*3)*policy.texel_u,(v+16*19)*policy.texel_v};
      const ArRenderPointF moved_uv=DioramaHeat_OwnedUv(&heat,policy,world,uv);
      const OwnedHeatOraclePoint expected=OwnedHeatOracleUv(&heat,policy,
          (OwnedHeatOraclePoint){world.x,world.y},(OwnedHeatOraclePoint){uv.x,uv.y});
      Close(moved_uv.x,expected.x,.000002);Close(moved_uv.y,expected.y,.000002);
      const bool active=d.x!=0||d.y!=0;
      if(active) {
        ++moved;const float inset=.5f/scale;
        const float localx=moved_uv.x/policy.texel_u-48,localy=moved_uv.y/policy.texel_v-304;
        assert(localx>=inset-.0001f&&localx<=16-inset+.0001f);
        assert(localy>=inset-.0001f&&localy<=16-inset+.0001f);
        if(mode==1&&(u+d.x<0||u+d.x>=16||v+d.y<0||v+d.y>=16))++wrapped;
        if(mode==2&&(u+d.x<inset||u+d.x>16-inset||v+d.y<inset||v+d.y>16-inset))++clamped;
      } else assert(!memcmp(&moved_uv,&uv,sizeof(uv)));
      /* Repack the identical material far away in the atlas. Receiver phase
       * and native-pixel displacement must be independent of slot identity. */
      const ArRenderPointF shift={16*7*policy.texel_u,-16*11*policy.texel_v};
      const ArRenderPointF repacked={uv.x+shift.x,uv.y+shift.y};
      const ArRenderPointF repacked_uv=DioramaHeat_OwnedUv(&heat,policy,world,repacked);
      Close(repacked_uv.x-shift.x,moved_uv.x,.000002);Close(repacked_uv.y-shift.y,moved_uv.y,.000002);
      DioramaHeat other_capture=heat;other_capture.source[0]=-10000;other_capture.source[1]=10000;
      other_capture.source[2]=64;other_capture.source[3]=64;
      const ArRenderPointF other=DioramaHeat_OwnedUv(&other_capture,policy,world,uv);
      assert(!memcmp(&other,&moved_uv,sizeof(other)));++samples;
    }
  }
  assert(samples==10952 && moved>7000 && wrapped>100 && clamped>100);
  DioramaHeat off=heat;memset(off.motion,0,sizeof(off.motion));
  for(unsigned mode=0;mode<3;++mode)for(unsigned i=0;i<17;++i) {
    const DioramaHeatOwned p={1/512.f,1/2048.f,(float)mode,mode==2?4:1};
    const ArRenderPointF uv={i/16.f,(i-8.f)/16.f},world={120,180};
    ArRenderPointF result=DioramaHeat_OwnedUv(&off,p,world,uv);
    assert(!memcmp(&result,&uv,sizeof(uv)));
    if(!mode){result=DioramaHeat_OwnedUv(&heat,p,world,uv);assert(!memcmp(&result,&uv,sizeof(uv)));}
  }
  /* Legacy capture path is still the same world wave in capture texels. */
  for(unsigned i=0;i<64;++i) {
    const ArRenderPointF world={80+i,140+i*.7f};
    const ArRenderPointF uv={(world.x-heat.source[0])/heat.source[2],(world.y-heat.source[1])/heat.source[3]};
    const ArRenderPointF result=DioramaHeat_Uv(&heat,uv);
    const OwnedHeatOraclePoint d=OwnedHeatOracleOffset(&heat,world.x,world.y);
    Close((result.x-uv.x)*heat.source[2],d.x,.00008);
    Close((result.y-uv.y)*heat.source[3],d.y,.00008);
  }
  DioramaSceneryLightEffectParams params={.shade={.8f,.7f,.6f,1},.heat=heat};
  DioramaSceneryUniforms before,after;DioramaSceneryUniforms_Init(&before,&params,1280,800);
  params.heat_owned=(DioramaHeatOwned){1/512.f,1/2048.f,1,1};
  DioramaSceneryUniforms_Init(&after,&params,1280,800);
  assert(!memcmp(before.values,after.values,176*sizeof(float)));
  assert(!memcmp(after.values+176,&params.heat_owned,sizeof(params.heat_owned)));
  puts("owned heat: independent world-wave oracle, atlas repacking,16/64 slot bounds, overlap, capture invariance, zero identity and uniform ABI passed");
  return 0;
}
