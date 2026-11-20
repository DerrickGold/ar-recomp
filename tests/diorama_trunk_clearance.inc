/* A shelf touching a solid bank needs an internal cap, not just a shorter
 * back depth. Verify the submitted source surfaces and the closing cap. */
#include "action/action_trunk_front.h"
typedef struct TrunkClearanceProbe {
  Recorder *recorder;
  unsigned lips,returns,bodies,guards,closures,full_bank;
  uint64_t closure_columns;
} TrunkClearanceProbe;
static bool TrunkClearanceUpdate(void *ctx,ArRenderTexture texture,const ArRenderRectI *rect,
    const void *pixels,int pitch) {
  Recorder *r=ctx;
  if(r->textures[texture.value].width==32&&
      (r->textures[texture.value].height==32||r->textures[texture.value].height==64)) {
    assert(!rect&&pixels&&pitch==128);++r->uploads;return true;
  }
  return Update(ctx,texture,rect,pixels,pitch);
}
static const int kTrunkClearanceRects[][4]={{160,64,208,144},{144,176,192,192},{128,352,208,368}};
static bool TrunkClearanceDepth(void *ctx,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  TrunkClearanceProbe *probe=ctx;Recorder *r=probe->recorder;
  const DioramaProjection *p=r->projection;const DioramaCapture *capture=r->capture;
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  if(draw->plane==SR_PPU_OVERLAY_BG1&&!draw->native_actor) {
    if(draw->clip_w) {
      assert(draw->lighting);const ArRenderRectF uv=draw->lighting->attributes_uv;
      for(int tri=0;tri<draw->index_count;tri+=3) {
        float native[3][2],cx=0,cy=0;
        for(unsigned j=0;j<3;++j) {
          const ArRenderPointF t=draw->vertices[draw->indices[tri+j]].tex_coord;
          native[j][0]=ox+(uv.x+t.x*uv.w)*640;
          native[j][1]=oy+(uv.y+t.y*uv.h)*SR_PPU_SURFACE_MAX_HEIGHT;
          cx+=native[j][0]/3;cy+=native[j][1]/3;
        }
        int contact=-1;
        for(unsigned rect=0;rect<3;++rect) {
          const int *b=kTrunkClearanceRects[rect];
          if(cx>=b[0]&&cx<b[2]&&cy>=b[1]&&cy<b[3])contact=rect?b[1]:cy<128?64:128;
        }
        if(contact<0)continue;
        for(unsigned j=0;j<3;++j) {
          const unsigned at=draw->indices[tri+j];
          const float t=fminf(1,fmaxf(0,(native[j][1]-contact-4)/4));
          float rim=fminf(1,fmaxf(0,(native[j][1]-112)/8));rim=rim*rim*(3-2*rim);
          const float fraction=.75f*t*t*(3-2*t)*rim,z=-.24f-.08f*fraction;
          Scene3DClipPoint clip;
          assert(Scene3D_TransformToClip(p->matrix,((native[j][0]-ox)/capture->width-.5f)*p->aspect_x,
              (.5f-(native[j][1]-oy)/capture->height)*p->height_scale,z,&clip));
          assert(fabsf(draw->clip_w[at]-clip.w)<.000002f);
          assert(fabsf(draw->depths[at]-(clip.z/clip.w*.5f+.5f))<.000002f);
          assert(fabsf(draw->vertices[at].position.x-(clip.x/clip.w*.5f+.5f)*p->output_width)<.01f);
          assert(fabsf(draw->vertices[at].position.y-(.5f-clip.y/clip.w*.5f)*p->output_height)<.01f);
          if(fraction==0)++probe->lips;else if(fraction==.75f)++probe->bodies;else ++probe->returns;
          if(contact==64&&native[j][1]<112)++probe->full_bank;
          if(native[j][1]<oy||native[j][1]>=oy+capture->height)++probe->guards;
        }
      }
    } else if(r->textures[draw->texture.value].width==32&&r->textures[draw->texture.value].height==64) {
      /* Recognize the new cap at Y=128 from independently projected native
       * endpoints. The source mask is solid on both sides of this join. */
      for(int first=0;first+3<draw->vertex_count;first+=4) {
        bool all=true,front=false,body=false;unsigned first_x=208;
        for(unsigned j=0;j<4;++j) {
          bool found=false;
          for(unsigned x=160;x<=208;++x)for(unsigned end=0;end<2;++end) {
            const float z=end?-.30f:-.24f;Scene3DPoint xy;float depth;
            assert(Scene3D_ProjectWorldPointWithDepth(p->matrix,((x-ox)/capture->width-.5f)*p->aspect_x,
                (.5f-(128-oy)/capture->height)*p->height_scale,z,p->output_width,p->output_height,&xy,&depth));
            const unsigned at=first+j;
            if(fabsf(xy.x-draw->vertices[at].position.x)<.005f&&
                fabsf(xy.y-draw->vertices[at].position.y)<.005f&&fabsf(depth-draw->depths[at])<.000002f) {
              found=true;first_x=first_x<x?first_x:x;if(end)body=true;else front=true;
            }
          }
          all&=found;
        }
        if(all&&front&&body){++probe->closures;if(first_x<208)probe->closure_columns|=UINT64_C(1)<<(first_x-160);}
      }
    }
  }
  return DepthGeometry(r,device,draw);
}
static unsigned TrunkClearanceFixture(unsigned angle) {
  s_count=0;static ActionTrunkAssets trunk;memset(&trunk,0,sizeof(trunk));
  trunk.room=6;trunk.height=512;trunk.side_width=1024;trunk.side_clearance=true;trunk.revision=9100+angle;
  for(unsigned y=15;y<=512/8;++y)memset(trunk.side_clearance_grid+y*129,255,129);
  for(unsigned rect=0;rect<3;++rect) {
    const int *b=kTrunkClearanceRects[rect];
    for(int y=b[1];y<b[3];++y)for(int x=b[0];x<b[2];++x)
      trunk.side_masks[kActionTrunkSideSolid][y*128+x/8]|=1u<<(x&7);
    for(int x=b[0];x<b[2];++x)trunk.side_contacts[b[1]*128+x/8]|=1u<<(x&7);
  }
  for(int x=160;x<208;++x)trunk.side_contacts[128*128+x/8]|=1u<<(x&7);
  /* A native black scallop at the first source row must not make a slit
   * across the top. Its native source mask stays absent; only the cap closes. */
  memcpy(trunk.side_masks[kActionTrunkSideSnow],trunk.side_masks[kActionTrunkSideSolid],sizeof(trunk.side_masks[0]));
  trunk.side_masks[kActionTrunkSideSolid][128*128+184/8]&=~(1u<<(184&7));
  trunk.side_masks[kActionTrunkSideSnow][128*128+184/8]&=~(1u<<(184&7));
  ActionTrunk_PrepareSideFronts(&trunk);
  assert(ActionTrunk_SideFrontCell(&trunk,160,64)==kActionTrunkFrontNative);
  assert(ActionTrunk_SideFrontCell(&trunk,160,112)==kActionTrunkFrontPixels);
  assert(ActionTrunk_SideFrontCell(&trunk,160,128)==kActionTrunkFrontPixels);
  assert(ActionTrunk_SideFrontCell(&trunk,160,136)==kActionTrunkFrontRecessed);
  Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true};
  ArRenderBackendOps ops=kOps;ops.update_texture=TrunkClearanceUpdate;
  ArRenderDevice device;assert(ArRenderDevice_Init(&device,&ops,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count],present=255;
  for(unsigned i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  const DioramaCapture capture={.width=256,.height=224,.bg1_camera_x=128,.camera_y=95,.textures=textures,.pixels=pixels};
  DioramaTrunkGuardView guard={.capture_generation=1,.source_revision=trunk.revision};
  assert(DioramaTrunkGuard_Resolve(128,96,256,224,(DioramaTrunkGuardBounds){0,0,1024,512},&guard.layout));
  for(unsigned i=0;i<3;++i)assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=128,
    .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&guard.textures[i]));
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
    .camera={.tilt_x=.45f,.tilt_y=angle?-.5f:.5f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  TrunkClearanceProbe probe={.recorder=&r};const DioramaDepthSink sink={.submit=TrunkClearanceDepth,.userdata=&probe};
  DioramaScene scene={.render=&options,.map_group=6,.map_number=6,.trunk=&trunk,.voxel_mode=true,
    .trunk_settings=DioramaTrunk_Default(),.trunk_guard=&guard,.depth_sink=&sink};
  DioramaProjection projection;r.capture=&capture;r.projection=&projection;
  for(unsigned frame=0;frame<2;++frame) {
    const unsigned creates=r.creates;probe.lips=probe.returns=probe.bodies=probe.guards=probe.closures=probe.full_bank=0;
    probe.closure_columns=0;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(probe.lips&&probe.returns&&probe.bodies&&probe.guards&&probe.closures&&probe.full_bank);
    assert(probe.closure_columns==((UINT64_C(1)<<48)-1));
    if(frame)assert(r.creates==creates);
  }
  Diorama_ResetCompositorResources(&device);
  for(unsigned i=0;i<3;++i)Destroy(&r,guard.textures[i]);
  for(unsigned i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
