#include "diorama_astral_rift.h"
#include "diorama_effect_backend.h"
#include "render/cloud_material.h"
#include "render/cloud_mesh.h"
#include "render/cloud_motion.h"
#include <stdlib.h>

enum { kDustVariants=6, kDustCell=96, kDustWidth=kDustCell*kDustVariants };
static ArRenderTexture s_dust;

static bool PrepareDust(ArRenderDevice *device) {
  if(ArRenderTexture_IsValid(s_dust))return true;
  uint32_t *pixels=malloc(kDustWidth*kDustCell*sizeof(*pixels));
  if(!pixels)return false;
  for(unsigned cell=0;cell<kDustVariants;++cell)
    for(unsigned y=0;y<kDustCell;++y)for(unsigned x=0;x<kDustCell;++x) {
      const float density=CloudMaterial_Wisps(x/(kDustCell-1.f),y/(kDustCell-1.f),
          0x73b18ea5u^(cell+1)*0x9e3779b9u);
      pixels[y*kDustWidth+cell*kDustCell+x]=((uint32_t)(density*255+.5f)<<24)|0xffffffu;
    }
  const ArRenderTextureDesc desc={.width=kDustWidth,.height=kDustCell,
      .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Static,
      .filter=kArRenderFilter_Linear,.blend=kArRenderBlendMode_Add};
  const bool ok=ArRenderDevice_CreateTexture(device,&desc,&s_dust)&&
      ArRenderDevice_UpdateTexture(device,s_dust,NULL,pixels,kDustWidth*sizeof(*pixels));
  free(pixels);
  if(!ok){ArRenderDevice_DestroyTexture(device,s_dust);s_dust=ArRenderTexture_Invalid();}
  return ok;
}

bool DioramaAstralRift_Prepare(ArRenderDevice *device) {
  return DioramaEffectBackend_IsAvailable(device, kDioramaEffect_NativeBloom)&&PrepareDust(device);
}
void DioramaAstralRift_Reset(ArRenderDevice *device) {
  ArRenderDevice_DestroyTexture(device,s_dust);s_dust=ArRenderTexture_Invalid();
}
bool DioramaAstralRift_Attach(bool enabled, ArRenderDevice *device, DioramaScene *scene) {
  if (!enabled || !scene || !scene->render || scene->map_group != 7 ||
      scene->map_number != 8 || scene->layer_section != kDioramaLayerSection_Room ||
      !DioramaAstralRift_Prepare(device)) return false;
  const DioramaRoomOverride *room = DioramaLayerOrder_Find(scene->render->layers, 7, 8);
  scene->native_bloom = room && room->native_bloom.set ? room->native_bloom :
      DioramaNativeBloom_Default();
  return true;
}

bool DioramaAstralRift_DrawDust(ArRenderDevice *device,const DioramaScene *scene,
    const DioramaCapture *capture,const DioramaProjection *projection) {
  const DioramaNativeBloom *style=&scene->native_bloom;
  if(style->dust<=0||scene->native_bloom_dimming>=1||!projection||!projection->valid)return true;
  if(!PrepareDust(device))return false;
  /* Resident bands retain raw, sometimes offscreen slots for the compute
   * renderer. CPU point projection needs only the fitted visible intervals. */
  DioramaProjection fitted=*projection;
  if(fitted.bg2_skybox.resident.meta[0]>0) {
    unsigned count=0;
    for(unsigned i=0;i<fitted.bg2_skybox.count;++i) {
      const DioramaSkyboxBandProjection band=fitted.bg2_skybox.bands[i];
      if(band.x1>band.x0&&band.y1>band.y0&&band.output_y1>band.output_y0)
        fitted.bg2_skybox.bands[count++]=band;
    }
    fitted.bg2_skybox.count=count;fitted.bg2_skybox.active_band=-1;
    projection=&fitted;
  }
  /* Sparse native-coordinate veils. Camera/viewport never seed placement or
   * time. Shared cloud density and orbit kernels supply the same soft edges
   * and deterministic motion as the environmental cloud system. */
  static const float patches[kDustVariants][6]={
    {-66,38,150,44,-.28f,0}, {126,66,178,52,-.38f,1}, {344,20,156,46,-.24f,0},
    {-28,158,170,43,-.36f,1}, {244,157,184,52,-.28f,0}, {458,122,150,42,-.42f,1}};
  ArRenderVertex2D vertices[kDustVariants*kCloudPatchVertices];
  bool visible[kDustVariants*kCloudPatchVertices];
  int32_t indices[kDustVariants*6*4*6];unsigned count=0;
  const float phase=(scene->native_bloom_clock/4096.f)*6.2831853f*style->dust_speed;
  const float ox=(capture->width-256)*.5f-capture->bg2_camera_x;
  const float oy=capture->authentic_y0-capture->bg2_camera_y;
  for(unsigned p=0;p<kDustVariants;++p) {
    const float *s=patches[p];
    const CloudOrbitMotion motion={.x=s[0],.dx=18+p*2,.x_phase=p*1.71f,
        .y=s[1],.dy=8,.y_rate=.5f,.y_phase=p*.83f,.alpha=.8f};
    const CloudOrbitSample sample=CloudMotion_Orbit(motion,phase*(p&1?2:1));
    const uint32_t rgb=s[5]>0?style->dust_rose:style->dust_blue;
    const ArRenderColorF tint={((rgb>>16)&255)/255.f,((rgb>>8)&255)/255.f,
        (rgb&255)/255.f,style->dust*sample.alpha*(1-scene->native_bloom_dimming)};
    ArRenderVertex2D *v=vertices+p*kCloudPatchVertices;
    CloudMesh_Patch(v,0,0,s[2]*style->dust_scale,s[3]*style->dust_scale,tint,0);
    const float c=cosf(s[4]),sn=sinf(s[4]);
    for(unsigned row=0;row<5;++row)for(unsigned col=0;col<7;++col) {
      ArRenderVertex2D *vertex=&v[row*7+col];
      const float x=vertex->position.x,y=vertex->position.y;
      /* Very slow differential shear gives the veil a curling edge; there is
       * no per-frame noise bake, texture upload, or breathing opacity. */
      const float edge=((int)row-2)*.5f;
      const float curl=5*sinf(phase*2+p+col*.5f)*(1-edge*edge);
      const float cx=sample.x+ox+c*x-sn*(y+curl),cy=sample.y+oy+sn*x+c*(y+curl);
      const bool mapped=projection->bg2_skybox.count?
          Diorama_ProjectSkyboxAnchorPoint(projection,s[1]+oy,cx,cy,&vertex->position):
          Diorama_ProjectCapturedBg2Point(projection,cx,cy,&vertex->position,NULL,NULL);
      visible[p*kCloudPatchVertices+row*7+col]=mapped;
      if(!mapped)vertex->position=(ArRenderPointF){0};
      /* Draw APIs use local viewport coordinates; projection points include
       * the output offset. One convention for letterbox, zoom and orbit. */
      vertex->position.x-=projection->output_x;vertex->position.y-=projection->output_y;
      vertex->tex_coord=(ArRenderPointF){(p*kDustCell+.5f+col*(kDustCell-1)/6.f)/kDustWidth,
          (.5f+row*(kDustCell-1)/4.f)/kDustCell};
    }
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<6;++col) {
      const int a=p*kCloudPatchVertices+row*7+col,b=a+7;
      const int32_t quad[6]={a,a+1,b+1,a,b+1,b};
      for(unsigned i=0;i<6;i+=3)
        if(visible[quad[i]]&&visible[quad[i+1]]&&visible[quad[i+2]])
          for(unsigned j=0;j<3;++j)indices[count++]=quad[i+j];
    }
  }
  const ArRenderDrawState state={.flags=kArRenderDrawState_Blend,.blend=kArRenderBlendMode_Add};
  return !count||ArRenderDevice_DrawGeometryWithState(device,s_dust,vertices,
      kDustVariants*kCloudPatchVertices,indices,count,&state);
}
