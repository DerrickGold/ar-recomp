/* Runs the actual compositor against a validating command recorder. The same
 * fixture is linked natively and as WASM; shader math/pixels are a separate gate. */
#include "diorama/diorama.h"
#include "diorama/diorama_depth_pass.h"
#include "diorama/diorama_voxel_cull.h"
#include "diorama/diorama_astral_rift.h"
#include "action/action_voxel_internal.h"
#include "action/action_voxel_sprites.h"
#include "action/action_trunk.h"
#include "action/action_ground_cover.h"
#include "render/scenery_shading.h"
#include "render/scene3d_math.h"
#include "diorama/diorama_effect_backend.h"
#include "diorama/diorama_performance.h"
#include "support/test_assert.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACE_CAPACITY 524288
static float s_trace[TRACE_CAPACITY];
static unsigned s_count;
static DioramaLayerOrderTable s_layers;
static bool s_depth_fixture;
static bool s_jungle_water_fixture;
static void (*s_waterfall_cloud_observe)(const DioramaSceneryLightEffectParams *);

typedef struct CapMesh {
  ArRenderVertex2D vertices[2048];
  bool after_actor[512];
  unsigned count;
} CapMesh;
typedef struct RootMesh {
  ArRenderVertex2D vertices[4096];
  bool after_actor[1024], front[1024];
  unsigned count;
} RootMesh;
typedef struct SkyBatchProbe {
  bool grouped, failed;
  unsigned attempts, sky_calls, effect_calls, actor_calls;
  unsigned fail_begin, fail_end, fail_draw;
} SkyBatchProbe;
typedef struct AuthoredCapProbe {
  DioramaVoxelPickFace faces[512];
  bool submitted[512], after_actor[512];
  unsigned count, automatic_caps, explicit_caps, material_caps, transparent_explicit, transparent_front;
  unsigned priorities[2];
  uint32_t atlas[512*16];
  ArRenderTexture texture;
  uint8_t automatic_art;
  bool uploaded;
} AuthoredCapProbe;
typedef struct NativeAffineProbe {
  DioramaVoxelPickFace faces[512];
  ArRenderVertex2D vertices[512][4];
  bool submitted[512],after_actor[512];
  unsigned count;
  uint32_t atlas[512*16];
  uint32_t crisp_pixels[2048*64];
  ArRenderTexture texture,native_texture,rear_texture,crisp_texture,face_textures[512];
  unsigned crisp_height;
  int source_shift;
  bool uploaded,crisp_uploaded;
} NativeAffineProbe;
typedef struct VoxelCullProbe {
  DioramaVoxelPickFace faces[128];
  bool submitted[128];
  unsigned count;
} VoxelCullProbe;
typedef struct TrunkSideProbe {
  ArRenderTexture material;
  int snow_start;
  unsigned caps[2], snow_caps, bark_caps, taper_vertices, full_vertices;
  unsigned far_vertices, high_vertices, atlas_uploads;
  unsigned one_pixel_risers, two_pixel_risers, white_walls, blue_walls, undersides;
} TrunkSideProbe;
typedef struct GroundCoverProbe {
  bool shared_depth,dedicated,empty,unsupported,snow_receiver,pressure_transform;
  unsigned kind,begins,ends,supports,caps,triangles,draw_triangles,vertices;
  unsigned before_actor,after_actor;
  unsigned lip_triangles,top_vertices;
  float lip_feather_area;
  float x0,x1,gap0,gap1,floor_y,z0,z1,seen_z0,seen_z1;
  unsigned char snow_columns[48];
  float pressure_xyz[8192][3];
  ArRenderVertex2D mesh[8192];
} GroundCoverProbe;
typedef struct Recorder {
  ArRenderTargetState state;
  ArRenderTextureDesc textures[128];
  unsigned next, live, draws, creates, draws_at_failed_restore;
  int width, height;
  bool shaders, fail_draw, fail_restore;
  int bound_effect;
  const DioramaCapture *capture;
  const DioramaProjection *projection;
  unsigned checked_object_meshes, exposure_samples;
  unsigned light_preparations;
  unsigned periodic_cloud_bindings;
  unsigned batch_begins, batch_ends;
  bool baked_uploads;
  unsigned uploads, fail_create_at, fail_upload_at;
  bool batch_open, fail_light, fail_submit;
  SkyBatchProbe sky_batch;
  DioramaSkyboxProjection light_skybox;
  float exposure_u0, exposure_u1;
  uint32_t skybox_inputs;
  ArRenderTexture water_texture, actor_texture, front_texture;
  ArRenderTexture copied_sources[128];
  bool actor_seen, front_seen;
  unsigned water_before_actor, water_after_actor;
  unsigned pool_vertices;
  bool pool, lava;
  ArRenderTexture grass_texture, thorn_texture;
  unsigned grass_vertices, thorn_vertices;
  bool thorn_high;
  ArRenderTexture prop_texture;
  unsigned prop_before_actor,prop_after_actor;
  int prop_priority;
  bool prop_lit;
  bool check_subscreen, fail_subscreen_draw;
  bool subscreen_sources[128];
  unsigned subscreen_draws, subscreen_adds;
  unsigned depth_draws, depth_vertices, depth_actor_vertices;
  const ActionTrunkAssets *trunk;
  unsigned depth_trunk_vertices;
  bool fail_depth;
  ArRenderTexture fold_front_texture, fold_rear_texture;
  ArRenderVertex2D fold_vertices[16];
  ArRenderTexture fold_sources[4];
  bool fold_after_actor[4];
  unsigned fold_faces;
  ArRenderTexture cap_texture;
  CapMesh *cap_mesh;
  bool owned_cap;
  uint32_t owned_cap_pixel;
  ArRenderTexture root_texture;
  RootMesh *root_mesh;
  const ActionVoxelGeometry *root_voxel;
  unsigned root_depth_vertices;
  bool perspective_capture;
  ArRenderVertex2D perspective_vertices[32];
  float perspective_w[32];
  bool perspective_after_actor[8];
  unsigned perspective_count;
  ArRenderTexture cloud_textures[2];
  unsigned cloud_vertices[2], cloud_depth_vertices[2];
  AuthoredCapProbe *authored_caps;
  NativeAffineProbe *native_affine;
  VoxelCullProbe *voxel_cull;
  TrunkSideProbe *trunk_side;
  GroundCoverProbe *ground_cover;
} Recorder;

static void CheckTrunkSideCaps(Recorder *,const ArRenderVertex2D *,int,const float *);
static void CheckGroundCoverDraw(Recorder *,const ArRenderVertex2D *,int,const float *);

static float Exposure(void *context,float u,float v,float inside_u,float inside_v) {
  (void)inside_u;(void)inside_v;
  Recorder *r=context;
  if(!r->exposure_samples)r->exposure_u0=r->exposure_u1=u;
  r->exposure_u0=fminf(r->exposure_u0,u);r->exposure_u1=fmaxf(r->exposure_u1,u);
  ++r->exposure_samples;
  return .6f+.2f*fmaxf(0,fminf(1,u+v));
}

static void Record(float value) {
  assert(isfinite(value) && s_count < TRACE_CAPACITY);
  s_trace[s_count++] = value;
}

static void Rectangle(const ArRenderRectF *r) {
  Record(r != NULL);
  if (!r) return;
  Record(r->x);
  Record(r->y);
  Record(r->w);
  Record(r->h);
}

static void IntRectangle(const ArRenderRectI *r) {
  const ArRenderRectF f = r ? (ArRenderRectF){r->x,r->y,r->w,r->h} : (ArRenderRectF){0};
  Rectangle(r ? &f : NULL);
}

static void Color(ArRenderColorF c) {
  Record(c.r);
  Record(c.g);
  Record(c.b);
  Record(c.a);
}

static void DrawState(const ArRenderDrawState *s) {
  Record(s != NULL);
  if (!s) return;
  Record(s->flags);
  Record(s->blend);
  Record(s->address_u);
  Record(s->address_v);
  Color(s->tint);
}

static bool Create(void *ctx, const ArRenderTextureDesc *desc, ArRenderTexture *texture) {
  Recorder *r = ctx;
  if (r->fail_create_at && r->creates+1==r->fail_create_at) return false;
  assert(++r->next < 128);
  r->textures[r->next] = *desc;
  r->live++;
  r->creates++;
  *texture = (ArRenderTexture){r->next};
  if(r->owned_cap && desc->width==512 && desc->height==2048)r->cap_texture=*texture;
  if(r->authored_caps && desc->width==512 && desc->height==2048)r->authored_caps->texture=*texture;
  if(r->native_affine && desc->width==512 && desc->height==2048)r->native_affine->texture=*texture;
  if(r->native_affine && desc->width==2048 && desc->usage==kArRenderTextureUsage_Static &&
      desc->filter==kArRenderFilter_Linear) {
    assert(desc->height>0 && desc->height<=64);
    r->native_affine->crisp_texture=*texture;r->native_affine->crisp_height=desc->height;
  }
  Record(1);
  Record(r->next);
  Record(desc->width);
  Record(desc->height);
  Record(desc->format);
  Record(desc->usage);
  Record(desc->filter);
  Record(desc->blend);
  return true;
}

static void Destroy(void *ctx, ArRenderTexture texture) {
  Recorder *r = ctx;
  assert(texture.value < 128 && r->textures[texture.value].width > 0);
  r->textures[texture.value].width = 0;
  r->live--;
  Record(2);
  Record(texture.value);
}

static bool Update(void *ctx, ArRenderTexture t, const ArRenderRectI *rect,
                   const void *pixels, int pitch) {
  Recorder *r=ctx;
  if(r->ground_cover) {
    const ArRenderTextureDesc *d=&r->textures[t.value];
    if((r->ground_cover->kind==1||r->ground_cover->pressure_transform)&&d->width==512&&d->height==2048) {
      assert(rect&&rect->x==0&&rect->y==0&&rect->w==512&&rect->h==16&&pixels&&pitch==512*4);
      ++r->uploads;return true;
    }
    if(r->ground_cover->pressure_transform&&d->width==2048&&d->height<=64&&d->usage==kArRenderTextureUsage_Static) {
      assert(pixels&&pitch==2048*4);
      assert(!rect||(rect->x==0&&rect->w==2048&&rect->y>=0&&rect->y+rect->h<=d->height));
      ++r->uploads;return true;
    }
    if(r->ground_cover->dedicated&&d->width==32&&(d->height==32||d->height==64)) {
      assert(!rect&&pixels&&pitch==32*4);++r->uploads;return true;
    }
  }
  if(r->native_affine && t.value==r->native_affine->crisp_texture.value) {
    NativeAffineProbe *probe=r->native_affine;
    const ArRenderRectI area=rect?*rect:(ArRenderRectI){0,0,2048,(int)probe->crisp_height};
    assert(pixels && area.x==0 && area.w==2048 && area.y>=0 && area.y+area.h<=(int)probe->crisp_height && pitch==2048*4);
    for(int y=0;y<area.h;++y)memcpy(probe->crisp_pixels+(area.y+y)*2048,
        (const uint8_t *)pixels+y*pitch,2048*4);
    probe->crisp_uploaded=true;++r->uploads;return true;
  }
  if(r->native_affine && r->textures[t.value].usage==kArRenderTextureUsage_Static &&
      r->textures[t.value].width==SR_PPU_SURFACE_MAX_WIDTH && r->textures[t.value].height==SR_PPU_SURFACE_MAX_HEIGHT) {
    assert(!rect && pixels && pitch==SR_PPU_SURFACE_MAX_WIDTH*4);
    for(unsigned i=0;i<SR_PPU_SURFACE_MAX_WIDTH*SR_PPU_SURFACE_MAX_HEIGHT;++i)
      assert(!((const uint32_t *)pixels)[i]);
    ++r->uploads;return true;
  }
  if(r->native_affine && t.value==r->native_affine->texture.value) {
    assert(rect && rect->x==0 && rect->y==0 && rect->w==512 && rect->h==16 && pitch==512*4);
    assert(pixels);memcpy(r->native_affine->atlas,pixels,sizeof(r->native_affine->atlas));
    r->native_affine->uploaded=true;++r->uploads;return true;
  }
  if(r->authored_caps && t.value==r->authored_caps->texture.value) {
    assert(rect && rect->x==0 && rect->y==0 && rect->w==512 && rect->h==16 && pitch==512*4);
    assert(pixels);memcpy(r->authored_caps->atlas,pixels,sizeof(r->authored_caps->atlas));
    r->authored_caps->uploaded=true;++r->uploads;return true;
  }
  if(r->owned_cap && r->textures[t.value].width==512 && r->textures[t.value].height==2048) {
    assert(rect && rect->x==0 && rect->y==0 && rect->w==512 && rect->h==16 && pitch==512*4);
    assert(pixels && *(const uint32_t *)pixels==r->owned_cap_pixel);++r->uploads;return true;
  }
  if(r->trunk && r->textures[t.value].width==32 && r->textures[t.value].height==32) {
    assert(!rect && pixels==r->trunk->bark && pitch==32*4);++r->uploads;return true;
  }
  if(r->trunk_side && r->textures[t.value].width==32 && r->textures[t.value].height==64) {
    assert(!rect && pixels && pitch==32*4);
    const uint32_t *atlas=pixels;
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<32;++x)
      assert(atlas[y*32+x]==(y<32?r->trunk->bark[y*32+x]:
          r->trunk->side_snow[(y&15)*16+(x&15)]));
    r->trunk_side->material=t;++r->trunk_side->atlas_uploads;++r->uploads;return true;
  }
  if (r->baked_uploads) {
    assert(t.value<128 && r->textures[t.value].usage==kArRenderTextureUsage_Static);
    assert(!rect && pixels && pitch==r->textures[t.value].width*4);
    ++r->uploads;
    return r->uploads != r->fail_upload_at;
  }
  if (t.value < 128 && r->textures[t.value].width == 1 &&
      r->textures[t.value].height == 1) {
    /* The cloud material reuses one immutable black illumination texel. */
    assert(rect && rect->x == 0 && rect->y == 0 && rect->w == 1 && rect->h == 1);
    assert(pixels && pitch == 4);
    assert(*(const uint32_t *)pixels == UINT32_C(0xff000000));
    ++r->uploads;
    return true;
  }
  assert(!"the compositor must not upload captured pixels");
  return false;
}

static bool Target(void *ctx, ArRenderTexture texture) {
  Recorder *r = ctx;
  assert(!r->batch_open);
  r->state.target = texture;
  Record(3);
  Record(texture.value);
  return true;
}

static bool Coordinates(void *ctx) { (void)ctx; Record(4); return true; }
static bool Size(void *ctx, int *w, int *h) {
  Recorder *r = ctx;
  unsigned t = (unsigned)r->state.target.value;
  *w = t ? r->textures[t].width : r->width;
  *h = t ? r->textures[t].height : r->height;
  assert(*w > 0 && *h > 0);
  return true;
}

static bool Viewport(void *ctx, const ArRenderRectI *rect) {
  Recorder *r = ctx;
  assert(!r->batch_open);
  if(rect && r->sky_batch.grouped && r->sky_batch.effect_calls)
    assert(r->projection->bg2_skybox.active_band==-1);
  r->state.viewport_set = rect != NULL;
  if (rect) r->state.viewport = *rect;
  Record(5);
  IntRectangle(rect);
  return true;
}

static bool Clip(void *ctx, const ArRenderRectI *rect) {
  Recorder *r = ctx;
  r->state.clip_enabled = rect != NULL;
  if (rect) r->state.clip = *rect;
  Record(6);
  IntRectangle(rect);
  return true;
}

static bool Capture(void *ctx, ArRenderTargetState *state) {
  *state = ((Recorder *)ctx)->state;
  state->valid = true;
  return true;
}

static bool Restore(void *ctx, const ArRenderTargetState *state) {
  Recorder *r = ctx;
  assert(!r->batch_open);
  Record(7);
  Record(state->target.value);
  if (r->fail_restore && (!r->check_subscreen || (r->subscreen_draws && !state->target.value))) {
    r->draws_at_failed_restore = r->draws;
    return false;
  }
  r->state = *state;
  return true;
}

static bool Clear(void *ctx, ArRenderColorF c) {
  (void)ctx;
  Record(8);
  Color(c);
  return true;
}

static bool Texture(void *ctx, ArRenderTexture t, const ArRenderRectF *src,
                    const ArRenderRectF *dst, const ArRenderDrawState *state) {
  Recorder *r = ctx;
  assert(t.value < 128 && r->textures[t.value].width > 0);
  if(r->state.target.value && (r->water_texture.value || r->grass_texture.value ||
      r->fold_front_texture.value || r->cap_texture.value || r->root_texture.value ||
      r->cloud_textures[0].value || r->perspective_capture || r->check_subscreen || r->authored_caps || r->native_affine || r->trunk_side))
    r->copied_sources[r->state.target.value]=r->copied_sources[t.value].value
        ?r->copied_sources[t.value]:t;
  Record(9);
  Record(t.value);
  Rectangle(src);
  Rectangle(dst);
  DrawState(state);
  r->draws++;
  return !r->fail_draw;
}

static void AuthoredCapObserve(void *context,const DioramaVoxelPickFace *face) {
  if(!face->authored)return;
  AuthoredCapProbe *probe=context;
  assert(probe->count<sizeof(probe->faces)/sizeof(*probe->faces));
  assert(face->priority<2 && face->plane==(face->priority?kDioramaPlane_Bg1Hi:kDioramaPlane_Bg1Far));
  probe->faces[probe->count++]=*face;
}
static void CheckAuthoredCapGeometry(AuthoredCapProbe *probe,const ArRenderVertex2D *vertices,int count,bool after_actor) {
  assert(probe->uploaded && count%4==0);
  for(int first=0;first<count;first+=4) {
    const ArRenderVertex2D *v=vertices+first;
    unsigned found=probe->count;
    for(unsigned f=0;f<probe->count && found==probe->count;++f)if(!probe->submitted[f]) {
      bool same=true;
      for(unsigned j=0;j<4;++j)same&=fabsf(v[j].position.x-probe->faces[f].screen_xy[j*2])<.001f &&
          fabsf(v[j].position.y-probe->faces[f].screen_xy[j*2+1])<.001f;
      if(same)found=f;
    }
    assert(found<probe->count);probe->submitted[found]=true;probe->after_actor[found]=after_actor;
    const DioramaVoxelPickFace *face=&probe->faces[found];++probe->priorities[face->priority];
    const bool front=face->kind==kDioramaVoxelFace_Front;
    const bool automatic=!front && (probe->automatic_art&(1u<<face->art_index));
    const bool material=face->art_source_size>1;
    if(material) {
      ++probe->material_caps;
      assert(face->art_source_x==512 && face->art_source_y==256);
      const bool side=face->kind==kDioramaVoxelFace_Left || face->kind==kDioramaVoxelFace_Right;
      for(unsigned j=0;j<4;++j) {
        assert(face->art_uv[j*2]>=.5f && face->art_uv[j*2]<=(side?15.5f:face->art_source_size-.5f));
        assert(face->art_uv[j*2+1]>=.5f && face->art_uv[j*2+1]<=(side?face->art_source_size-.5f:15.5f));
        if(side)assert(fabsf(face->art_uv[j*2+1]-(.5f+15*(face->world_xy[j*2+1]-96)/16))<.0001f);
      }
      /* Full patches preserve their phase where native actor-priority
       * clipping divides the cap, including one-pixel contour stairs. */
      for(unsigned other=0;other<found;++other)if(probe->submitted[other] &&
          probe->after_actor[other]!=after_actor && probe->faces[other].kind==face->kind)
        for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)
          if(fabsf(face->screen_xy[j*2]-probe->faces[other].screen_xy[k*2])<.001f &&
              fabsf(face->screen_xy[j*2+1]-probe->faces[other].screen_xy[k*2+1])<.001f) {
            assert(fabsf(face->art_uv[j*2]-probe->faces[other].art_uv[k*2])<.0001f);
            assert(fabsf(face->art_uv[j*2+1]-probe->faces[other].art_uv[k*2+1])<.0001f);
          }
    }
    if(!front) {
      if(automatic)++probe->automatic_caps;else ++probe->explicit_caps;
      /* Opposite depth vertices retain the same native contour texel only
       * for automatic art. Explicit chosen alpha keeps its repeating UVs. */
      if(automatic)for(unsigned i=0;i<2;++i) {
        assert(fabsf(face->art_uv[i*2]-face->art_uv[(3-i)*2])<.0001f);
        assert(fabsf(face->art_uv[i*2+1]-face->art_uv[(3-i)*2+1])<.0001f);
      }
    }
    /* Sample the actual submitted atlas UVs, not merely an observer flag.
     * A one-pixel diagonal has alpha holes throughout its native 16px art;
     * none may puncture the automatic extrusion at any depth sample. */
    for(unsigned iy=0;iy<5;++iy)for(unsigned ix=0;ix<5;++ix) {
      const float x=(ix+.5f)/5,y=(iy+.5f)/5;
      const float weights[]={(1-x)*(1-y),x*(1-y),x*y,(1-x)*y};
      float u=0,w=0;
      for(unsigned j=0;j<4;++j){u+=weights[j]*v[j].tex_coord.x;w+=weights[j]*v[j].tex_coord.y;}
      const int px=(int)floorf(u*512),py=(int)floorf(w*2048);
      assert(px>=0 && px<512 && py>=0 && py<16);
      const bool opaque=(probe->atlas[py*512+px]>>24)!=0;
      if(automatic || material)assert(opaque);
      else if(!opaque) {
        if(front)++probe->transparent_front;else ++probe->transparent_explicit;
      }
    }
  }
}
static bool Geometry(void *ctx, ArRenderTexture t, const ArRenderVertex2D *v,
                     int nv, const int32_t *indices, int ni, const ArRenderDrawState *state) {
  Recorder *r = ctx;
  assert(!t.value || (t.value < 128 && r->textures[t.value].width > 0));
  const ArRenderTexture source=r->copied_sources[t.value].value?r->copied_sources[t.value]:t;
  if(r->ground_cover&&!t.value&&!r->ground_cover->shared_depth)
    CheckGroundCoverDraw(r,v,nv,NULL);
  if(r->trunk_side && r->trunk_side->material.value && t.value==r->trunk_side->material.value)
    CheckTrunkSideCaps(r,v,nv,NULL);
  if(r->voxel_cull)for(unsigned f=0;f<r->voxel_cull->count;++f) {
    const DioramaVoxelPickFace *face=&r->voxel_cull->faces[f];
    for(int first=0;first+3<nv;first+=4) {
      bool same=true;
      for(unsigned j=0;j<4;++j)same&=fabsf(v[first+j].position.x-face->screen_xy[j*2])<.001f &&
          fabsf(v[first+j].position.y-face->screen_xy[j*2+1])<.001f;
      if(same)r->voxel_cull->submitted[f]=true;
    }
  }
  if(r->native_affine && (t.value==r->native_affine->texture.value ||
      t.value==r->native_affine->crisp_texture.value ||
      source.value==r->native_affine->native_texture.value ||
      (r->native_affine->rear_texture.value && source.value==r->native_affine->rear_texture.value))) {
    NativeAffineProbe *probe=r->native_affine;
    for(int first=0;first+3<nv;first+=4) {
      unsigned found=probe->count;
      for(unsigned f=0;f<probe->count && found==probe->count;++f)if(!probe->submitted[f]) {
        bool same=true;for(unsigned j=0;j<4;++j)
          same&=fabsf(v[first+j].position.x-probe->faces[f].screen_xy[j*2])<.001f &&
              fabsf(v[first+j].position.y-probe->faces[f].screen_xy[j*2+1])<.001f;
        if(same)found=f;
      }
      if(t.value==probe->texture.value || t.value==probe->crisp_texture.value)assert(found<probe->count);
      if(found==probe->count)continue;
      probe->submitted[found]=true;probe->after_actor[found]=r->actor_seen;
      probe->face_textures[found]=t;
      memcpy(probe->vertices[found],v+first,4*sizeof(*v));
    }
  }
  if(r->authored_caps && t.value==r->authored_caps->texture.value)
    CheckAuthoredCapGeometry(r->authored_caps,v,nv,r->actor_seen);
  for(unsigned high=0;high<2;++high)if(r->cloud_textures[high].value &&
      source.value==r->cloud_textures[high].value) {
    /* The semantic split supplies low cloud ink on the front output and
     * leaves high ink in native BG1Hi. Coincident geometry must not merge
     * their native slots: OBJ2 stays between the two actual draw commands. */
    assert(r->capture && r->projection && r->actor_seen==(high!=0));
    for(int i=0;i<nv;++i) {
      ArRenderPointF uv=v[i].tex_coord;
      if(source.value!=t.value) {
        uv.x*=r->capture->width/(float)SR_PPU_SURFACE_MAX_WIDTH;
        uv.y*=r->capture->height/(float)SR_PPU_SURFACE_MAX_HEIGHT;
      }
      ArRenderPointF low_point,high_point;
      const float x=uv.x*SR_PPU_SURFACE_MAX_WIDTH,y=uv.y*SR_PPU_SURFACE_MAX_HEIGHT;
      assert(Diorama_ProjectCapturedBg1FarPoint(r->projection,x,y,&low_point,NULL,NULL));
      assert(Diorama_ProjectCapturedBg1HighPoint(r->projection,x,y,&high_point,NULL,NULL));
      assert(fabsf(low_point.x-high_point.x)<.002f && fabsf(low_point.y-high_point.y)<.002f);
      assert(fabsf(v[i].position.x+r->projection->output_x-low_point.x)<.002f);
      assert(fabsf(v[i].position.y+r->projection->output_y-low_point.y)<.002f);
    }
    r->cloud_vertices[high]+=nv;
  }
  if(r->root_mesh && source.value==r->root_texture.value && nv%4==0 && ni==nv/4*6) {
    RootMesh *mesh=r->root_mesh;
    assert(mesh->count+nv<=sizeof(mesh->vertices)/sizeof(*mesh->vertices));
    for(int i=0;i<nv;++i) {
      ArRenderVertex2D vertex=v[i];
      /* Keep capture UVs comparable between direct contours and the compact
       * native-art source used for front faces. The fixture has no apron. */
      if(source.value!=t.value) {
        vertex.tex_coord.x*=r->capture->width/(float)SR_PPU_SURFACE_MAX_WIDTH;
        vertex.tex_coord.y*=r->capture->height/(float)SR_PPU_SURFACE_MAX_HEIGHT;
      }
      mesh->vertices[mesh->count+i]=vertex;
      if(i%4==0) {
        mesh->after_actor[(mesh->count+i)/4]=r->actor_seen;
        mesh->front[(mesh->count+i)/4]=fabsf(v[i].color.r-1)<.00001f;
      }
    }
    mesh->count+=nv;
  }
  /* This unlit cap fixture's native flat face has constant color. Generated
   * caps vary with depth, distinguishing them from its subdivided quads. */
  if(r->cap_mesh && source.value==r->cap_texture.value && nv%4==0 && ni==nv/4*6 &&
      fabsf(v[0].color.r-v[2].color.r)>.00001f) {
    CapMesh *mesh=r->cap_mesh;
    assert(mesh->count+nv<=sizeof(mesh->vertices)/sizeof(*mesh->vertices));
    memcpy(mesh->vertices+mesh->count,v,nv*sizeof(*v));
    for(int i=0;i<nv;i+=4)mesh->after_actor[(mesh->count+i)/4]=r->actor_seen;
    mesh->count+=nv;
  }
  if(r->fold_front_texture.value && nv==4 && ni==6 &&
      (source.value==r->fold_front_texture.value || source.value==r->fold_rear_texture.value)) {
    assert(r->fold_faces<4);
    memcpy(r->fold_vertices+r->fold_faces*4,v,4*sizeof(*v));
    r->fold_sources[r->fold_faces]=source;
    r->fold_after_actor[r->fold_faces]=r->actor_seen;
    ++r->fold_faces;
  }
  if(r->check_subscreen && r->subscreen_sources[source.value]) {
    assert(r->state.target.value && state && state->blend==kArRenderBlendMode_Alpha);
    ++r->subscreen_draws;
    if(r->fail_subscreen_draw)return false;
  }
  if(r->check_subscreen && state && state->blend==kArRenderBlendMode_AddPremultiplied) {
    assert(!r->state.target.value && nv==4 && ni==6);
    assert(!r->subscreen_sources[t.value]);
    ++r->subscreen_adds;
  }
  if(r->actor_texture.value && source.value==r->actor_texture.value)r->actor_seen=true;
  if(r->prop_texture.value && t.value==r->prop_texture.value) {
    assert(r->bound_effect==(r->prop_lit?kDioramaEffect_Count:-1));
    if(r->actor_seen)r->prop_after_actor+=nv;else r->prop_before_actor+=nv;
    for(int i=0;i<nv;++i)assert(v[i].tex_coord.x>=0 && v[i].tex_coord.x<=1 &&
        v[i].tex_coord.y>=0 && v[i].tex_coord.y<=1 && v[i].color.a==1);
  }
  if(r->front_texture.value && source.value==r->front_texture.value)r->front_seen=true;
  if(r->water_texture.value && source.value==r->water_texture.value) {
    if(r->pool && nv==4) {
      assert(r->actor_seen && ni==6);
      for(int i=0;i<4;++i) {
        const float expected=(r->lava?159+(i>=2?15.5f:.5f):171+(i>=2?3.5f:.5f))/SR_PPU_SURFACE_MAX_HEIGHT;
        assert(v[i].tex_coord.y==expected);
        assert(v[i].color.a==(r->lava?1.f:.85f));
      }
      r->pool_vertices+=4;
    }
    if(r->actor_seen)r->water_after_actor+=nv;
    else {
      r->water_before_actor+=nv;
      /* Only generated, translucent side curtains may precede actors.
       * The native high-priority water face must remain in its normal slot. */
      assert(nv==8 && ni==12);
      assert(!r->front_seen);
      for(int i=0;i<nv;++i)assert(v[i].color.a>0 && v[i].color.a<.5f);
    }
  }
  if(r->grass_texture.value && t.value==r->grass_texture.value && nv==16) {
    assert(!r->actor_seen && ni==24 && r->bound_effect==kDioramaEffect_DofEdge);
    for(int i=0;i<nv;++i) {
      assert(v[i].tex_coord.x>=r->exposure_u0 && v[i].tex_coord.x<=r->exposure_u1);
      assert(v[i].color.a==1);
    }
    r->grass_vertices+=nv;
  }
  if(r->thorn_texture.value && t.value==r->thorn_texture.value && nv==24) {
    assert(r->actor_seen==r->thorn_high && ni==36);
    for(int i=0;i<nv;++i)assert(v[i].color.a==1);
    r->thorn_vertices+=nv;
  }
  if (r->capture && nv == 4 && ni == 6)
    for (int plane = 0; plane < kDioramaPlane_Count; plane++)
      if (t.value == r->capture->textures[plane].value)
        r->skybox_inputs |= 1u << plane;
  if (r->capture && r->projection && r->bound_effect == kDioramaEffect_RimLight) {
    for (unsigned priority=0;priority<4;++priority) {
      const int plane = DioramaPlaneForObjectPriority(priority);
      if (t.value != r->capture->textures[plane].value) continue;
      float min_u=INFINITY, max_u=-INFINITY;
      for (int i=0;i<nv;++i) {
        min_u=fminf(min_u,v[i].tex_coord.x);
        max_u=fmaxf(max_u,v[i].tex_coord.x);
        /* Enlarging the drawable mesh must not shift/stretch the sprites or
         * disagree with the published transform used by attached effects. */
        const DioramaPlaneProjection *p=&r->projection->object_planes[priority];
        const float x=v[i].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH-
            r->capture->obj_apron-p->capture_offset.x;
        const float y=v[i].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT-p->capture_offset.y;
        ArRenderPointF expected;
        assert(Diorama_ProjectCapturedPoint(r->projection,x,y,priority,&expected,NULL,NULL));
        assert(fabsf(v[i].position.x+r->projection->output_x-expected.x)<.002f);
        assert(fabsf(v[i].position.y+r->projection->output_y-expected.y)<.002f);
      }
      assert(min_u == 0);
      assert(fabsf(max_u-(float)(r->capture->width+2*r->capture->obj_apron)/
          SR_PPU_SURFACE_MAX_WIDTH)<.00001f);
      ++r->checked_object_meshes;
    }
  }
  Record(10);
  Record(t.value);
  Record(nv);
  Record(ni);
  DrawState(state);
  for (int i = 0; i < nv; ++i) {
    Record(v[i].position.x);
    Record(v[i].position.y);
    Color(v[i].color);
    Record(v[i].tex_coord.x);
    Record(v[i].tex_coord.y);
  }
  for (int i = 0; i < ni; ++i) {
    assert(indices[i] >= 0 && indices[i] < nv);
    Record(indices[i]);
  }
  r->draws++;
  return !r->fail_draw;
}

static bool Present(void *ctx) { (void)ctx; assert(!"caller owns present"); return false; }
static const char *Error(void *ctx) { (void)ctx; return "injected recorder failure"; }
static const ArRenderBackendOps kOps = {
  .struct_size = sizeof(ArRenderBackendOps),
  .create_texture = Create, .destroy_texture = Destroy, .update_texture = Update,
  .set_render_target = Target, .use_output_coordinates = Coordinates,
  .get_output_size = Size, .set_viewport = Viewport, .set_clip_rect = Clip,
  .capture_render_target_state = Capture, .restore_render_target_state = Restore,
  .clear = Clear, .draw_texture = Texture, .draw_geometry = Geometry,
  .present = Present, .last_error = Error,
};

/* Record semantic shader parameters; this backend does not claim pixel parity. */
bool DioramaEffectBackend_IsAvailable(ArRenderDevice *device, DioramaEffectKind effect) {
  (void)effect;
  return ((Recorder *)device->context)->shaders;
}
static bool Bind(ArRenderDevice *device, int effect) {
  Recorder *r = device->context;
  assert(r->shaders && r->bound_effect == -1);
  r->bound_effect = effect;
  Record(11);
  Record(effect);
  return true;
}
bool DioramaEffectBackend_BindNativeBloom(ArRenderDevice *d, const DioramaNativeBloomEffectParams *p) {
  Bind(d,kDioramaEffect_NativeBloom);
  Record(p->step_x);Record(p->step_y);Record(p->threshold);Record(p->mode);
  Record(p->saturation);Record(p->contrast);Record(p->gain);
  return true;
}
bool DioramaEffectBackend_BindBlur(ArRenderDevice *d, const DioramaBlurEffectParams *p) {
  Bind(d, kDioramaEffect_Blur);
  Record(p->texel_width);
  Record(p->texel_height);
  Record(p->radius);
  return true;
}
bool DioramaEffectBackend_BindRimLight(ArRenderDevice *d, const DioramaRimLightEffectParams *p) {
  Bind(d, kDioramaEffect_RimLight);
  Record(p->texel_width);
  Record(p->texel_height);
  Record(p->strength);
  return true;
}
bool DioramaEffectBackend_BindDofEdge(ArRenderDevice *d, const DioramaDofEdgeEffectParams *p) {
  Bind(d, kDioramaEffect_DofEdge);
  Record(p->texel_width);
  Record(p->texel_height);
  Record(p->blur_radius);
  Record(p->u_min);
  Record(p->u_max);
  Record(p->v_min);
  Record(p->v_max);
  Record(p->edge_feather);
  Record(p->lower_content_v_max);
  return true;
}
bool DioramaEffectBackend_BindPrioritySurface(
    ArRenderDevice *d, const DioramaPrioritySurfaceEffectParams *p) {
  Bind(d, kDioramaEffect_PrioritySurface);
  Record(p->width);
  Record(p->height);
  Record(p->high_band);
  Record(p->additive);
  return true;
}
bool DioramaEffectBackend_Unbind(ArRenderDevice *d) {
  ((Recorder *)d->context)->bound_effect = -1;
  Record(12);
  return true;
}
bool DioramaEffectBackend_BindSceneryLight(ArRenderDevice *d,
    const DioramaSceneryLightEffectParams *p) {
  Recorder *r = d->context;
  if(s_waterfall_cloud_observe)s_waterfall_cloud_observe(p);
  assert(p && p->illumination.value < 128 &&
      r->textures[p->illumination.value].width > 0);
  assert(p->illumination.value != r->state.target.value);
  if (p->cloud_light.meta[0] > 0 && p->cloud_light.source[2] == 256 &&
      p->cloud_light.source[3] == 256) {
    assert(p->cloud_light.source[0] == 0 && p->cloud_light.source[1] == -1);
    assert(p->cloud_light.regions[0].bounds[0] == 360);
    ++r->periodic_cloud_bindings;
  }
  Bind(d, kDioramaEffect_Count);
  Record(p->illumination.value);
  Record(p->mode);
  Color(p->shade);
  Rectangle(&p->light_uv);
  if (r->state.target.value && !s_jungle_water_fixture) {
    /* Grouped thickness draws cover only the viewport. They must sample the
     * matching subregion of the light map, including letterbox offsets. */
    assert(fabsf(p->light_uv.x-8.0f/r->width)<.00001f);
    assert(fabsf(p->light_uv.y-10.0f/r->height)<.00001f);
    assert(fabsf(p->light_uv.w-(r->width-16.0f)/r->width)<.00001f);
    assert(fabsf(p->light_uv.h-(r->height-20.0f)/r->height)<.00001f);
  }
  return true;
}
void DioramaEffectBackend_Reset(ArRenderDevice *d) {
  assert(((Recorder *)d->context)->bound_effect == -1);
}

/* Timings are deliberately excluded from deterministic command comparison. */
DioramaPerformanceScope DioramaPerformance_Begin(DioramaPerformanceStage stage) {
  return (DioramaPerformanceScope){.stage = stage};
}
void DioramaPerformance_End(DioramaPerformanceScope scope) { (void)scope; }
void DioramaPerformance_SetViewport(int w, int h) { (void)w; (void)h; }
void DioramaPerformance_SetRasterViewport(int w, int h) { (void)w; (void)h; }
void DioramaPerformance_SetPlane(int plane) { (void)plane; }
void DioramaPerformance_AddDraw(bool ok, const ArRenderVertex2D *v, int nv,
                               const int32_t *i, int ni, ArRenderBlendMode blend) {
  (void)ok;
  (void)v;
  (void)nv;
  (void)i;
  (void)ni;
  (void)blend;
}

static void PlaneEffect(void *data, int plane, const DioramaProjection *projection) {
  Recorder *r = data;
  assert(r->batch_open);
  assert(r->bound_effect == -1 && projection->valid);
  assert(!r->state.target.value && !r->state.viewport_set);
  Record(13);
  Record(plane);
}
static bool SceneryLight(void *data, const DioramaProjection *projection) {
  Recorder *r = data;
  assert(r->batch_open);
  assert(r->bound_effect == -1 && projection->valid && r->state.target.value);
  assert(!r->state.viewport_set && !r->state.clip_enabled);
  const ArRenderTextureDesc *target = &r->textures[r->state.target.value];
  assert(target->width == r->width && target->height == r->height);
  ++r->light_preparations;
  r->light_skybox = projection->bg2_skybox;
  return !r->fail_light;
}

static bool SourceBatch(ArRenderDevice *device, bool begin) {
  Recorder *r = device->context;
  assert(r->batch_open != begin);
  r->batch_open = begin;
  if (begin) ++r->batch_begins;
  else ++r->batch_ends;
  return begin || !r->fail_submit;
}

static PresentationOutcome Backdrop(void *data, ArRenderRectI viewport) {
  Recorder *r = data;
  assert(r->bound_effect == -1 && r->state.viewport_set);
  assert(viewport.x == 0 && viewport.y == 0 && viewport.w > 0 && viewport.h > 0);
  assert(viewport.w == r->state.viewport.w && viewport.h == r->state.viewport.h);
  Record(15);
  Record(viewport.w);
  Record(viewport.h);
  return kPresentationOutcome_Complete;
}

static ArRenderTexture Skybox(void *data, ArRenderDevice *d, int source,
                             bool fill, uint32_t color, bool *failed) {
  (void)d;
  *failed = false;
  Record(14);
  Record(source);
  Record(fill);
  Record(color);
  return *(ArRenderTexture *)data;
}

/* Recover the camera ray's intersection with an independently published
 * actor plane from screen XY. This catches lost/averaged depth even when the
 * shared native/WASM stream would otherwise agree on the same wrong value. */
static float PlaneDepth(const DioramaProjection *p,float world_z,ArRenderPointF point) {
  const float *m=p->matrix;
  const double x=2.0*point.x/p->output_width-1,y=1-2.0*point.y/p->output_height;
  const double z=world_z;
  const double a=m[0]-x*m[3],b=m[4]-x*m[7],c=m[1]-y*m[3],d=m[5]-y*m[7];
  const double u=(x*m[11]-m[8])*z+x*m[15]-m[12],v=(y*m[11]-m[9])*z+y*m[15]-m[13];
  const double det=a*d-b*c;assert(fabs(det)>.000001);
  const double wx=(u*d-b*v)/det,wy=(a*v-u*c)/det;
  return (float)(.5+.5*(m[2]*wx+m[6]*wy+m[10]*z+m[14])/(m[3]*wx+m[7]*wy+m[11]*z+m[15]));
}
/* Independent camera-ray recovery: the two front corners must still lie on
 * actual native pixel endpoints at the actor plane, regardless of orbit or
 * capture origin. The rear corners then use that same world-space endpoint. */
static ArRenderPointF TrunkSideNativePoint(const Recorder *r,ArRenderPointF point) {
  const DioramaProjection *p=r->projection;const float *m=p->matrix;
  const double x=2.0*point.x/p->output_width-1,y=1-2.0*point.y/p->output_height,z=-.24;
  const double a=m[0]-x*m[3],b=m[4]-x*m[7],c=m[1]-y*m[3],d=m[5]-y*m[7];
  const double u=(x*m[11]-m[8])*z+x*m[15]-m[12],v=(y*m[11]-m[9])*z+y*m[15]-m[13];
  const double det=a*d-b*c;assert(fabs(det)>.000001);
  const double wx=(u*d-b*v)/det,wy=(a*v-u*c)/det;
  const float ox=r->capture->bg1_camera_x-(r->capture->width-256)*.5f;
  const float oy=r->capture->camera_y-r->capture->authentic_y0+1;
  const float nx=ox+(wx/p->aspect_x+.5)*r->capture->width;
  const float ny=oy+(.5-wy/p->height_scale)*r->capture->height;
  assert(fabsf(nx-roundf(nx))<.004f && fabsf(ny-roundf(ny))<.004f);
  return (ArRenderPointF){roundf(nx),roundf(ny)};
}
static float TrunkSideExpectedSpan(float x) {
  const float distance=x<320?320-x:x>704?x-704:0;
  return .18f*fminf(1,distance/32.f);
}
static void CheckTrunkSideCaps(Recorder *r,const ArRenderVertex2D *vertices,int count,const float *depths) {
  assert(count>0 && count%4==0);
  TrunkSideProbe *probe=r->trunk_side;
  for(int first=0;first<count;first+=4) {
    const ArRenderVertex2D *v=vertices+first;
    const ArRenderPointF a=TrunkSideNativePoint(r,v[0].position),b=TrunkSideNativePoint(r,v[1].position);
    const int dx=(int)(b.x-a.x),dy=(int)(b.y-a.y);
    assert(abs(dx)+abs(dy)==1);
    const unsigned side=dx==1?0:dy==1?1:dx==-1?2:3;
    const int x=(int)(side==2?b.x:side==1?a.x-1:a.x);
    const int y=(int)(side==3?b.y:side==2?a.y-1:a.y);
    assert(ActionTrunk_SideSolid(r->trunk,x,y,kActionTrunkSideSolid));
    const bool high=ActionTrunk_SideSolid(r->trunk,x,y,kActionTrunkSidePriority);
    /* These four positions are the known terrace's two 1px steps and two
     * 2px steps, not a reimplementation of the renderer's run detector. */
    const bool one_pixel=!high && y==110 &&
        ((side==1 && x==probe->snow_start+7)||(side==3 && x==probe->snow_start+32));
    const bool two_pixel=!high && (y==111 || y==112) &&
        ((side==1 && x==probe->snow_start+15)||(side==3 && x==probe->snow_start+24));
    const bool snow_riser=one_pixel||two_pixel;
    const bool white_wall=!high && (side&1) &&
        (x==probe->snow_start || x==probe->snow_start+47);
    const bool blue_wall=high && (side&1);
    if(white_wall)assert(ActionTrunk_SideSolid(r->trunk,x,y,kActionTrunkSideSnowFace));
    if(blue_wall)assert(ActionTrunk_SideSolid(r->trunk,x,y,kActionTrunkSideSnow) &&
        !ActionTrunk_SideSolid(r->trunk,x,y,kActionTrunkSideSnowFace));
    const bool vertical_snow=snow_riser||white_wall;
    const bool snow=(side==0 && ActionTrunk_SideSolid(r->trunk,x,y,kActionTrunkSideSnow))||vertical_snow;
    assert(r->actor_seen==high);
    if(!depths) {
      ++probe->caps[high];
      if(snow)++probe->snow_caps;else ++probe->bark_caps;
      if(one_pixel)++probe->one_pixel_risers;
      if(two_pixel)++probe->two_pixel_risers;
      if(white_wall)++probe->white_walls;
      if(blue_wall)++probe->blue_walls;
      if(!high && side==2)++probe->undersides;
    }
    if(vertical_snow)assert(fabsf(v[2].color.r-v[0].color.r*.86f)<.000001f &&
        fabsf(v[3].color.r-v[1].color.r*.86f)<.000001f);
    for(unsigned j=0;j<4;++j) {
      const ArRenderPointF native=(j==1||j==2)?b:a;
      const float span=TrunkSideExpectedSpan(native.x),z=-.24f-(j>=2?span:0);
      const float ox=r->capture->bg1_camera_x-(r->capture->width-256)*.5f;
      const float oy=r->capture->camera_y-r->capture->authentic_y0+1;
      Scene3DClipPoint clip;
      assert(Scene3D_TransformToClip(r->projection->matrix,
          ((native.x-ox)/r->capture->width-.5f)*r->projection->aspect_x,
          (.5f-(native.y-oy)/r->capture->height)*r->projection->height_scale,z,&clip));
      const float sx=(clip.x/clip.w*.5f+.5f)*r->projection->output_width;
      const float sy=(.5f-clip.y/clip.w*.5f)*r->projection->output_height;
      assert(fabsf(v[j].position.x-sx)<.003f && fabsf(v[j].position.y-sy)<.003f);
      if(depths)assert(fabsf(depths[first+j]-(clip.z/clip.w*.5f+.5f))<.000002f);
      const float along=(side&1)?native.y-y:native.x-x;
      const float across=j>=2?span/.18f:0;
      const float u=snow_riser?((x&31)+native.x-x)/32.f:
          (side&1)?(.5f+31*across)/32.f:((x&31)+along)/32.f;
      const float vv=(side&1)&&!snow_riser?(snow?.5f:0)+fminf(31.5f,(y&31)+along+.5f)/64.f:
          (snow?.5f:0)+(.5f+31*across)/64.f;
      assert(fabsf(v[j].tex_coord.x-u)<.000001f && fabsf(v[j].tex_coord.y-vv)<.000001f);
      if(!depths && j>=2) {
        if(span>0 && span<.1799f)++probe->taper_vertices;
        if(span>=.1799f)++probe->full_vertices;
      }
    }
  }
}
static bool DepthGeometry(void *context,ArRenderDevice *device,const DioramaDepthSubmission *draw) {
  Recorder *r=context;
  assert(draw && draw->depths && draw->vertex_count>0 && draw->index_count>0);
  ++r->depth_draws;r->depth_vertices+=draw->vertex_count;
  if(r->fail_depth)return false;
  const bool ground_cover=r->ground_cover&&!draw->texture.value;
  if(ground_cover) {
    assert(!draw->native_actor && draw->blend==kArRenderBlendMode_Alpha && draw->clip_w);
    assert(!draw->lighting && draw->vertex_count%3==0);
    assert(draw->plane==kDioramaPlane_Bg1Far||draw->plane==kDioramaPlane_Bg1Hi||draw->plane==SR_PPU_OVERLAY_BG1);
    CheckGroundCoverDraw(r,draw->vertices,draw->vertex_count,draw->depths);
    Record(22);Record(draw->plane);Record(false);Record(draw->vertex_count);
    for(int i=0;i<draw->vertex_count;++i) {
      assert(isfinite(draw->depths[i])&&draw->depths[i]>0&&draw->depths[i]<1);
      assert(isfinite(draw->clip_w[i])&&draw->clip_w[i]>0);
      Record(draw->depths[i]);Record(draw->clip_w[i]);
    }
    const ArRenderDrawState cover_state={.flags=kArRenderDrawState_Blend,.blend=kArRenderBlendMode_Alpha};
    return ArRenderDevice_DrawGeometryWithState(device,draw->texture,draw->vertices,draw->vertex_count,
        draw->indices,draw->index_count,&cover_state);
  }
  assert(draw->lighting);
  if(r->trunk_side && r->trunk_side->material.value && draw->texture.value==r->trunk_side->material.value)
    CheckTrunkSideCaps(r,draw->vertices,draw->vertex_count,draw->depths);
  const int priority=DioramaPlaneObjectPriority(draw->plane);
  if(draw->native_actor)assert(priority>=0 && draw->texture.value!=r->prop_texture.value && !draw->clip_w);
  if(draw->clip_w) {
    assert(!draw->native_actor && draw->vertex_count%4==0);
    for(int i=0;i<draw->vertex_count;++i)assert(isfinite(draw->clip_w[i]) && draw->clip_w[i]>0);
    if(r->perspective_capture) {
      assert(r->perspective_count+draw->vertex_count<=32);
      memcpy(r->perspective_vertices+r->perspective_count,draw->vertices,draw->vertex_count*sizeof(*draw->vertices));
      memcpy(r->perspective_w+r->perspective_count,draw->clip_w,draw->vertex_count*sizeof(*draw->clip_w));
      for(int i=0;i<draw->vertex_count;i+=4)r->perspective_after_actor[(r->perspective_count+i)/4]=r->actor_seen;
      r->perspective_count+=draw->vertex_count;
    }
  }
  Record(22);Record(draw->plane);Record(draw->native_actor);Record(draw->vertex_count);
  for(int i=0;i<draw->vertex_count;++i) {
    const float z=draw->depths[i];assert(isfinite(z) && z>0 && z<1);Record(z);
    for(unsigned high=0;high<2;++high)if(r->cloud_textures[high].value &&
        draw->plane==(high?kDioramaPlane_Bg1Hi:kDioramaPlane_Bg1Far)) {
      assert(!draw->native_actor && r->projection);
      assert(fabsf(z-PlaneDepth(r->projection,0,draw->vertices[i].position))<.000002f);
      ++r->cloud_depth_vertices[high];
    }
    if(draw->native_actor && draw->vertices[i].color.a==1) {
      assert(r->projection && r->projection->object_planes[priority].valid);
      assert(fabsf(z-PlaneDepth(r->projection,r->projection->object_planes[priority].z_world,
          draw->vertices[i].position))<.000002f);
      ++r->depth_actor_vertices;
    }
    if(r->trunk && draw->plane==kDioramaPlane_Bg1Far) {
      const ArRenderRectF uv=draw->lighting->attributes_uv;
      const float x=r->capture->bg1_camera_x-(r->capture->width-256)*.5f+
          (uv.x+draw->vertices[i].tex_coord.x*uv.w)*SR_PPU_SURFACE_MAX_WIDTH;
      const DioramaTrunkSettings settings=DioramaTrunk_Default();
      const float q=(x-512)/160;
      const bool side=r->trunk->side_width && (x<=320 || x>=704);
      const float world_z=side?-settings.recess-TrunkSideExpectedSpan(x):
          -settings.recess-settings.depth*fmaxf(0,1-q*q);
      assert(fabsf(z-PlaneDepth(r->projection,world_z,draw->vertices[i].position))<.000002f);
      ++r->depth_trunk_vertices;
      if(side && r->trunk_side)++r->trunk_side->far_vertices;
    }
    if(r->trunk_side && draw->plane==kDioramaPlane_Bg1Hi &&
        draw->texture.value!=r->trunk_side->material.value &&
        r->textures[draw->texture.value].width!=32) {
      const ArRenderRectF uv=draw->lighting->attributes_uv;
      const float x=r->capture->bg1_camera_x-(r->capture->width-256)*.5f+
          (uv.x+draw->vertices[i].tex_coord.x*uv.w)*SR_PPU_SURFACE_MAX_WIDTH;
      if(x<320-.001f || x>704+.001f) {
        assert(r->actor_seen && !draw->native_actor);
        assert(fabsf(z-PlaneDepth(r->projection,-.24f,draw->vertices[i].position))<.000002f);
        ++r->trunk_side->high_vertices;
      }
    }
    if(r->root_voxel && draw->plane==kDioramaPlane_Bg1Far &&
        draw->vertex_count%4==0 && fabsf(draw->vertices[i].color.r-1)<.00001f) {
      const ArRenderRectF uv=draw->lighting->attributes_uv;
      const int first=i/4*4;
      float cx=0,cy=0;
      for(int j=0;j<4;++j) {
        cx+=(uv.x+draw->vertices[first+j].tex_coord.x*uv.w)*SR_PPU_SURFACE_MAX_WIDTH*.25f;
        cy+=(uv.y+draw->vertices[first+j].tex_coord.y*uv.h)*SR_PPU_SURFACE_MAX_HEIGHT*.25f;
      }
      const int ox=r->capture->bg1_camera_x,oy=r->capture->camera_y+1;
      const int tx=(int)floorf((cx+ox)/8),ty=(int)floorf((cy+oy)/8);
      const VoxelTrunkCurve *curve=ActionVoxel_TrunkCurveForCell(r->root_voxel,tx,ty);
      const float world_y=oy+(uv.y+draw->vertices[i].tex_coord.y*uv.h)*SR_PPU_SURFACE_MAX_HEIGHT;
      const float depth=curve?ActionVoxel_TrunkFrontDepth(r->root_voxel,curve,world_y):
          ActionVoxel_FrontRecess(r->root_voxel,tx,ty);
      assert(fabsf(z-PlaneDepth(r->projection,-depth,draw->vertices[i].position))<.000002f);
      ++r->root_depth_vertices;
    }
  }
  /* Test adapter draws the same material through the recorder. The portable
   * handoff must preserve light/filter/priority parameters, not just vertices. */
  const DioramaSceneryLightEffectParams *p=draw->lighting;bool bound=false;
  if(ArRenderTexture_IsValid(p->illumination))bound=DioramaEffectBackend_BindSceneryLight(device,p);
  else if(p->mode==2)bound=DioramaEffectBackend_BindPrioritySurface(device,&p->priority);
  else if(p->rim_strength>0) {
    const DioramaRimLightEffectParams rim={p->texel_width,p->texel_height,p->rim_strength};
    bound=DioramaEffectBackend_BindRimLight(device,&rim);
  } else if(p->mode==1) {
    const DioramaDofEdgeEffectParams dof={.texel_width=p->texel_width,.texel_height=p->texel_height,
        .blur_radius=p->blur_radius,.u_max=1,.v_max=1,.lower_content_v_max=p->lower_content_v_max};
    bound=DioramaEffectBackend_BindDofEdge(device,&dof);
  }
  const ArRenderDrawState state={.flags=kArRenderDrawState_Blend|kArRenderDrawState_Address,
      .blend=draw->blend,.address_u=kArRenderTextureAddressMode_Clamp,.address_v=kArRenderTextureAddressMode_Clamp};
  const bool ok=ArRenderDevice_DrawGeometryWithState(device,draw->texture,draw->vertices,draw->vertex_count,
      draw->indices,draw->index_count,&state);
  return (!bound || DioramaEffectBackend_Unbind(device)) && ok;
}

static unsigned AstralFixture(unsigned scenario) {
  s_count=0;
  Recorder r={.width=640,.height=360,.bound_effect=-1,.shaders=scenario!=13,.baked_uploads=true};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,.skybox=kDioramaSky_Both};
  const DioramaRenderOptions before=options;
  DioramaScene scene={.render=&options,.map_group=7,.map_number=8};
  assert(!DioramaAstralRift_Attach(false,&device,&scene));
  scene.map_number=7;assert(!DioramaAstralRift_Attach(true,&device,&scene));
  scene.map_number=8;scene.layer_section=kDioramaLayerSection_DeathHeimCompletion;
  assert(!DioramaAstralRift_Attach(true,&device,&scene));
  scene.layer_section=kDioramaLayerSection_Room;
  assert(DioramaAstralRift_Attach(true,&device,&scene)==r.shaders);
  assert(scene.native_bloom.set==r.shaders);
  assert(!memcmp(&options,&before,sizeof(options))); /* Native captures/skybox stay intact. */
  assert(!scene.backdrop); /* Original backgrounds remain in their painter slots. */
  assert(r.creates==(r.shaders?1:0)&&r.uploads==(r.shaders?1:0)); /* One retained procedural atlas. */
  assert(DioramaAstralRift_Attach(true,&device,&scene)==r.shaders);
  assert(r.creates==(r.shaders?1:0)&&r.uploads==(r.shaders?1:0));
  if(r.shaders) {
    assert(scene.native_bloom.receivers==6&&scene.native_bloom.light>0);
    const DioramaCapture capture={.width=256,.height=224};
    const DioramaProjection projection={.valid=true,.output_width=640,.output_height=360,
      .bg2_skybox={.count=2,.active_band=-1,.resident={.meta={2}},
        .bands={{0},{0,0,256,224,0,1}}}};
    scene.native_bloom_dimming=1;
    assert(DioramaAstralRift_DrawDust(&device,&scene,&capture,&projection)&&!r.draws);
    scene.native_bloom_dimming=.5f;
    assert(DioramaAstralRift_DrawDust(&device,&scene,&capture,&projection)&&r.draws==1);
    scene.native_bloom.dust=0;
    assert(DioramaAstralRift_DrawDust(&device,&scene,&capture,&projection)&&r.draws==1);
    assert(r.creates==1&&r.uploads==1); /* No rebake during fades/toggles. */
  }
  DioramaAstralRift_Reset(&device);
  return s_count;
}

static unsigned VoxelWaterFixture(unsigned scenario) {
  s_count=0;
  const bool lava=scenario>=20;
  if(lava)scenario-=10;
  const bool pool=scenario>=10;
  if(pool)scenario-=10;
  Recorder r={.width=960,.height=600,.bound_effect=-1};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,
      (ArRenderCapabilities){.flags=UINT64_MAX,.maximum_texture_width=8192,
        .maximum_texture_height=8192,.maximum_render_target_width=8192,
        .maximum_render_target_height=8192}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];
  const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));
    pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=lava?4:pool?2:1;voxel->active_room=lava?4:pool?1:2;
  voxel->profile=lava?kActionVoxelProfile_AitosCaverns:
      pool?kActionVoxelProfile_BloodpoolMarsh:kActionVoxelProfile_FillmoreCave;
  voxel->settings=ActionVoxel_SettingsForRoom(voxel->active_group,voxel->active_room);
  voxel->width=1024;voxel->height=256;voxel->ready=true;
  voxel->waterfall_count=!pool;
  voxel->waterfalls[0]=(VoxelWaterfall){.x=scenario&4?640:80,.y=0,
      .width=32,.height=192,.high_priority=1};
  if(pool) {
    voxel->water_surface_count=1;
    voxel->water_surfaces[0]=(VoxelWaterSurface){.x=scenario&4?640:80,.y=160,
        .width=64,.high_priority=1,.background=0,.sample_y=lava?0:12,
        .sample_height=lava?16:4,.opaque=lava};
  }
  const DioramaCapture capture={.width=256,.height=224,
      .obj_apron=scenario&2?64:0,.bg_apron_mask=3,
      .textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,
      .distance_scale=1,.camera={.tilt_x=.15f,.tilt_y=scenario&1?.4f:-.4f}};
  const DioramaRenderOptions options={.edge_aa=s_depth_fixture,.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink depth_sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&depth_sink:NULL,.map_group=voxel->active_group,.map_number=voxel->active_room,
      .voxel_geometry=scenario<8?voxel:NULL};
  r.water_texture=textures[pool?kDioramaPlane_Bg1Hi:kDioramaPlane_Bg2Hi];
  r.pool=pool;r.lava=lava;
  r.actor_texture=textures[kDioramaPlane_Obj2];
  r.front_texture=textures[kDioramaPlane_Bg1Far];
  const unsigned base_resources=r.live;
  for(unsigned frame=0;frame<2;++frame) {
    const unsigned creates=r.creates;
    r.actor_seen=r.front_seen=false;r.water_before_actor=r.water_after_actor=0;
    r.pool_vertices=0;
    DioramaProjection projection;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && r.actor_seen && r.water_after_actor>0);
    /* Both orbit directions, with/without captured guards. Offscreen falls
     * add no rear pass, and ordinary Diorama retains its original ordering. */
    assert(r.water_before_actor==((!pool && scenario<8 && !(scenario&4))?8u:0u));
    if(pool) {
      assert(r.pool_vertices==((scenario<8 && !(scenario&4))?4u:0u));
      if(scenario<8)assert((projection.bg1_plane.z_world==projection.bg1_far_plane.z_world)==!lava);
    }
    assert(r.live<=base_resources+2 && !r.state.target.value && !r.state.viewport_set);
    if(frame)assert(r.creates==creates);
  }
  if(s_depth_fixture) {
    assert((r.depth_draws>0)==(scenario<8));
    if(scenario<8)assert(r.depth_actor_vertices);
  }
  ActionVoxel_Release(voxel);
  Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(r.live==0);
  return s_count;
}

static unsigned VoxelGrowthFixture(unsigned scenario) {
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,
      (ArRenderCapabilities){.flags=UINT64_MAX,.maximum_texture_width=8192,
        .maximum_texture_height=8192,.maximum_render_target_width=8192,
        .maximum_render_target_height=8192}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=1;voxel->active_room=1;
  voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);
  voxel->width=1024;voxel->height=256;voxel->ready=true;
  const bool visible=scenario<10,active=scenario<8 || scenario>=10;
  const int x=visible?80:640;
  voxel->patch_count=2;
  voxel->patches[0]=(VoxelPatch){.x=x,.y=80,.width=32,.height=16,.kind=kVoxelPatch_RearGrass};
  voxel->patches[1]=(VoxelPatch){.x=x,.y=112,.width=32,.height=16,
      .high_priority=!!(scenario&4),.kind=kVoxelPatch_Thorns};
  const DioramaCapture capture={.width=256,.height=224,.obj_apron=scenario&2?64:0,
      .bg_apron_mask=3,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.15f,.tilt_y=scenario&1?.4f:-.4f}};
  const DioramaRenderOptions options={.edge_aa=s_depth_fixture,.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .depth_of_field=true};
  const DioramaDepthSink depth_sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&depth_sink:NULL,.map_group=1,.map_number=1,
      .voxel_geometry=active?voxel:NULL};
  r.grass_texture=textures[SR_PPU_OVERLAY_BG1];
  r.thorn_high=!!(scenario&4);
  r.thorn_texture=textures[r.thorn_high?kDioramaPlane_Bg1Hi:kDioramaPlane_Bg1Far];
  r.actor_texture=textures[kDioramaPlane_Obj2];
  r.exposure_u0=(x+capture.obj_apron+.5f)/SR_PPU_SURFACE_MAX_WIDTH;
  r.exposure_u1=(x+capture.obj_apron+31.5f)/SR_PPU_SURFACE_MAX_WIDTH;
  for(unsigned frame=0;frame<2;++frame) {
    r.actor_seen=false;r.grass_vertices=r.thorn_vertices=0;
    const unsigned creates=r.creates;DioramaProjection projection;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && r.actor_seen);
    /* Rear tufts share backdrop blur and stay behind actors. Thorn profiles
     * retain low/high native priority; offscreen and Diorama add no clumps. */
    assert(r.grass_vertices==(active && visible?16u:0));
    assert(r.thorn_vertices==(active && visible?24u:0));
    assert(!r.state.target.value && !r.state.viewport_set);
    if(frame)assert(r.creates==creates);
  }
  if(s_depth_fixture) {
    assert((r.depth_draws>0)==active);if(active)assert(r.depth_actor_vertices);
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}

static void PropActorSlot(void *userdata,int plane,const DioramaProjection *projection) {
  Recorder *r=userdata;(void)projection;
  if(plane==DioramaPlaneForObjectPriority(r->prop_priority))r->actor_seen=true;
}
static bool PropLight(void *userdata,const DioramaProjection *projection,unsigned receiver) {
  (void)userdata;assert(projection && (receiver==2 || receiver==4));return true;
}
typedef struct PropObserved {
  unsigned count;
  DioramaVoxelPickFace first;
} PropObserved;
static void PropObserve(void *userdata,const DioramaVoxelPickFace *face) {
  PropObserved *p=userdata;
  if(face->kind!=kDioramaVoxelFace_Object || face->owner!=0xee0)return;
  assert(face->plane==-1 && face->cell_x==-1 && face->cell_y==-1);
  for(unsigned i=0;i<4;++i)assert(isfinite(face->depth[i]) && face->depth[i]>0 &&
      face->depth[i]<1 && face->clip_w[i]>0 && isfinite(face->screen_xy[2*i]));
  if(!p->count)p->first=*face;
  ++p->count;
}
static unsigned VoxelSpriteFixture(unsigned scenario) {
  const unsigned kind=scenario/6;
  const bool rock=kind==1,temple=kind>=2;
  scenario%=6;
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX,
      .maximum_texture_width=8192,.maximum_texture_height=8192,
      .maximum_render_target_width=8192,.maximum_render_target_height=8192}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,
        .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Streaming},&textures[i]));
    pixels[i]=&present;
  }
  assert(Create(&r,&(ArRenderTextureDesc){.width=kVoxelSpriteAtlasWidth,
      .height=kVoxelSpriteAtlasHeight,.format=kArRenderPixelFormat_Argb8888,
      .usage=kArRenderTextureUsage_Streaming},&r.prop_texture));
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=temple?5:rock?4:2;voxel->active_room=temple?7:rock?5:1;
  voxel->profile=temple?kActionVoxelProfile_MarahnaTemple:rock?kActionVoxelProfile_AitosCaverns:kActionVoxelProfile_BloodpoolMarsh;
  voxel->settings=ActionVoxel_SettingsForRoom(voxel->active_group,voxel->active_room);
  voxel->width=1024;voxel->height=512;voxel->ready=true;
  const unsigned priority=scenario%4;
  ActionVoxelSpriteFrame props={.count=1,.sprites={{.owner=0xee0,.shape=rock?2:1,
      .priority=priority,.width=rock?32:16,.height=16,.solid_height=16,.x=104,.y=180,.ambient=1}}};
  for(unsigned y=0;y<16;++y)props.sprites[0].rows[y]=rock?0xffffffff:0xffff;
  unsigned before=20,after=16;
  if(temple) {
    ActionVoxelSprite *p=&props.sprites[0];p->width=32;p->shape=kind+1;
    for(unsigned y=0;y<32;++y)p->rows[y]=UINT64_C(0xffffffff);
    if(kind==3) {
      /* Two bounded cells: upper spikes plus body, then the lower spikes. */
      p->height=32;p->solid_y=16;p->solid_height=16;
      props.count=2;props.sprites[1]=*p;
      props.sprites[1].height=16;props.sprites[1].y+=32;props.sprites[1].component_y=32;
      props.sprites[1].solid_y=props.sprites[1].solid_height=0;
      before=88;after=52;
    } else if(kind==4) {
      p->solid_height=0;before=36;after=20;
    } else if(kind==5) {
      p->solid_height=3;before=52;after=32;
    } else if(kind==6) {
      props.count=3;p->width=64;p->height=p->solid_height=32;
      for(unsigned y=0;y<32;++y)p->rows[y]=UINT64_MAX;
      for(unsigned i=0;i<3;++i) {
        props.sprites[i]=*p;props.sprites[i].x=104+i*64;
        props.sprites[i].component_x=i*64;
        props.sprites[i].join_left=i?UINT32_MAX:0;
        props.sprites[i].join_right=i==2?0:UINT32_MAX;
      }
      before=44;after=32;
    }
  }
  if(scenario>=4)before=after=0;
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.18f,.tilt_y=scenario&1?.4f:-.4f}};
  DioramaRenderOptions options={.edge_aa=s_depth_fixture,.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .depth_of_field=true};
  const DioramaDepthSink depth_sink={.submit=DepthGeometry,.userdata=&r};
  DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&depth_sink:NULL,.map_group=voxel->active_group,.map_number=voxel->active_room,
      .voxel_geometry=scenario==4?NULL:voxel,.voxel_sprites=&props,
      .voxel_sprite_atlas=scenario==5?ArRenderTexture_Invalid():r.prop_texture,
      .plane_effect=PropActorSlot,.plane_effect_userdata=&r,
      .actor_light=scenario==2||scenario==3?PropLight:NULL};
  const float cuts[]={.1f,.25f,.3f};
  const SceneryShadingField exposure={.x=cuts,.y=cuts,.nx=3,.ny=3,.sample=Exposure,.userdata=&r};
  if(s_depth_fixture)scene.scenery_exposure=&exposure;
  r.prop_priority=priority;
  r.prop_lit=scenario==2||scenario==3;
  for(unsigned frame=0;frame<2;++frame) {
    r.actor_seen=false;r.prop_before_actor=r.prop_after_actor=0;
    DioramaProjection projection;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    if(r.prop_before_actor!=before || r.prop_after_actor!=after)
      fprintf(stderr,"voxel prop kind=%u scenario=%u before=%u after=%u actor=%u\n",kind,scenario,
          r.prop_before_actor,r.prop_after_actor,r.actor_seen);
    assert(r.prop_before_actor==before);
    assert(r.prop_after_actor==after);
    assert(!r.state.target.value && !r.state.viewport_set);
  }
  if(s_depth_fixture) {
    assert((r.depth_draws>0)==(scenario!=4));if(scenario!=4)assert(r.depth_actor_vertices);
  }
  if(s_depth_fixture && scenario==0) {
    DioramaProjection projection;r.projection=&projection;
    r.fail_depth=true;
    assert(Diorama_Composite(&device,&capture,&view,&scene,&projection)==kPresentationOutcome_CoreFailure);
    assert(!projection.valid && !r.state.target.value && r.bound_effect==-1);
    r.fail_depth=false;
    const DioramaDepthSink missing={0};scene.depth_sink=&missing;
    assert(Diorama_Composite(&device,&capture,&view,&scene,&projection)==kPresentationOutcome_CoreFailure);
    assert(!projection.valid && !r.state.target.value && r.bound_effect==-1);
  }
  if(!kind && !scenario) {
    PropObserved observed={0};options.voxel_observe_face=PropObserve;options.voxel_observer_userdata=&observed;
    scene.depth_sink=s_depth_fixture?&depth_sink:NULL;
    ActionVoxelSprite *p=&props.sprites[0];
    for(unsigned mode=0;mode<2;++mode) {
      p->shape=kActionVoxelSpriteGenericShape;
      p->presentation_mode=mode?kDioramaVoxelObject_Flat:kDioramaVoxelObject_Auto;
      r.actor_seen=false;r.prop_before_actor=r.prop_after_actor=0;
      DioramaProjection projection;r.projection=&projection;
      assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
      assert(!r.prop_before_actor && !r.prop_after_actor && !observed.count);
    }
    p->presentation_mode=kDioramaVoxelObject_Extruded;p->presentation_fields=15;
    p->depth_offset=.12f;p->thickness=.04f;p->width_scale=1;
    DioramaVoxelPickFace full_face={0};
    for(unsigned narrower=0;narrower<2;++narrower) {
      p->width_scale=narrower?.5f:1;
      observed=(PropObserved){0};r.actor_seen=false;r.prop_before_actor=r.prop_after_actor=0;
      DioramaProjection projection;r.projection=&projection;
      assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
      assert(r.prop_before_actor==24 && !r.prop_after_actor && observed.count==6);
      assert(observed.first.world_xy[0]==104 && observed.first.world_xy[2]==120);
      if(!narrower)full_face=observed.first;
      else assert(fabsf(observed.first.screen_xy[2]-observed.first.screen_xy[0])<
          fabsf(full_face.screen_xy[2]-full_face.screen_xy[0])*.6f);
    }
    p->depth_offset=-.12f;observed=(PropObserved){0};
    r.actor_seen=false;r.prop_before_actor=r.prop_after_actor=0;
    DioramaProjection projection;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    /* The walking cap remains under actors; all other faces now sit wholly
     * in front of the unchanged native actor plane. */
    assert(r.prop_before_actor==4 && r.prop_after_actor==20 && observed.count==6);
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  Destroy(&r,r.prop_texture);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}

static unsigned VoxelSurfaceFixture(unsigned scenario) {
  const bool tomb=scenario>=4;
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));
    pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=3;voxel->active_room=tomb?3:1;
  voxel->profile=tomb?kActionVoxelProfile_KassandoraTomb:kActionVoxelProfile_KassandoraDesert;
  voxel->settings=ActionVoxel_SettingsForRoom(3,voxel->active_room);voxel->width=1024;voxel->height=768;voxel->ready=true;
  /* One-pixel slope risers, a flat top, and a tall cliff. Granular shading
   * affects the small risers while retaining the cliff's side-face treatment. */
  const VoxelEdge edges[]={
    {.x=80,.y=140,.length=1,.side=0,.far_rank=2,.colliding=1,.textured_top=1},
    {.x=81,.y=139,.length=1,.side=3,.far_rank=2,.colliding=1,.textured_top=1},
    {.x=81,.y=139,.length=1,.side=0,.far_rank=2,.colliding=1,.textured_top=1},
    {.x=82,.y=138,.length=1,.side=3,.far_rank=2,.colliding=1,.textured_top=1},
    {.x=82,.y=138,.length=30,.side=0,.far_rank=2,.colliding=1,.textured_top=1},
    {.x=112,.y=138,.length=32,.side=1,.far_rank=2,.colliding=1,.textured_top=1},
  };
  voxel->edge_count=voxel->pass_counts[0]=sizeof(edges)/sizeof(*edges);
  memcpy(voxel->edges,edges,sizeof(edges));
  if(tomb) {
    voxel->mixed_scenery=true;
    voxel->front_recess=calloc((size_t)voxel->width/8,voxel->height/8);assert(voxel->front_recess);
    for(int y=12;y<22;++y)for(int x=16;x<18;++x)voxel->front_recess[y*(voxel->width/8)+x]=1;
    const VoxelEdge architecture[]={
      {.x=128,.y=176,.length=16,.side=0,.near_rank=1,.far_rank=2,.colliding=1,.neighbor_rear_mounted=1},
      {.x=64,.y=176,.length=16,.side=0,.near_rank=1,.far_rank=2,.colliding=1},
      {.x=128,.y=96,.length=80,.side=3,.far_rank=1,.rear_mounted=1},
      {.x=144,.y=96,.length=80,.side=1,.far_rank=1,.rear_mounted=1},
      {.x=64,.y=96,.length=80,.side=3,.far_rank=1},
      {.x=80,.y=96,.length=80,.side=1,.far_rank=1},
    };
    memcpy(voxel->edges,architecture,sizeof(architecture));
    if(scenario>=6) {
      voxel->gate_count=1;
      voxel->gates[0]=(VoxelGate){.x=96,.y=96,.height=80,.width=5,.centered=true,.high_priority=scenario>=8};
    }
  }
  for(unsigned i=0;i<voxel->edge_count;++i) {
    voxel->edges[i].soft_surface=!tomb && scenario>=2;voxel->pass_edges[0][i]=i;
  }
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.15f,.tilt_y=scenario&1?.4f:-.4f}};
  const DioramaRenderOptions options={.edge_aa=s_depth_fixture,.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink depth_sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&depth_sink:NULL,.map_group=3,.map_number=voxel->active_room,.voxel_geometry=voxel};
  const unsigned resources=r.live;
  DioramaProjection projection;r.projection=&projection;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(projection.valid && r.draws && r.live<=resources+2);
  if(s_depth_fixture)assert(r.depth_draws && r.depth_actor_vertices);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  return s_count;
}
static unsigned VoxelFoldFixture(unsigned scenario) {
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));
    pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);
  voxel->width=1024;voxel->height=768;voxel->ready=true;
  voxel->edge_count=voxel->pass_counts[0]=1;voxel->pass_edges[0][0]=0;
  const bool top=scenario==4,bottom=scenario>=2;
  const VoxelEdge original={.x=96,.y=top?128:bottom?200:0,.length=top?32:56,
      .side=top?0:(scenario&1)?3:1,.far_rank=2,.colliding=1,.rear_fold_top=7};
  const DioramaCapture capture={.width=256,.height=224,.camera_y=7,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.15f,.tilt_y=scenario&1?-.4f:.4f}};
  const DioramaRenderOptions options={.edge_aa=s_depth_fixture,.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  r.fold_front_texture=textures[kDioramaPlane_Bg1Far];
  r.fold_rear_texture=textures[SR_PPU_OVERLAY_BG1];
  r.actor_texture=textures[kDioramaPlane_Obj2];
  ArRenderVertex2D reference[16];
  for(unsigned frame=0;frame<3;++frame) {
    voxel->edges[0]=original;
    if(frame==1 && !top) {
      /* Independently clip the native segment. A seven-pixel rise along
       * 56 rows becomes six at the top clip, or three at the bottom clip.
       * The submitted face must match this equivalent authored segment. */
      if(bottom) {voxel->edges[0].length=32;voxel->edges[0].rear_fold_bottom=3;}
      else {voxel->edges[0].y=8;voxel->edges[0].length=48;voxel->edges[0].rear_fold_top=6;}
    }
    if(frame==2)voxel->edges[0].rear_fold_top=0;
    r.fold_faces=0;r.actor_seen=false;
    DioramaProjection projection;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && r.fold_faces==2);
    if(r.fold_after_actor[0] || r.fold_after_actor[1])
      fprintf(stderr,"fold scenario %u frame %u: sources %u/%u, after actor %u/%u, actor seen %u\n",
          scenario,frame,(unsigned)r.fold_sources[0].value,(unsigned)r.fold_sources[1].value,
          r.fold_after_actor[0],r.fold_after_actor[1],r.actor_seen);
    assert(!r.fold_after_actor[0] && !r.fold_after_actor[1]);
    assert(r.fold_sources[0].value==textures[top && frame<2?SR_PPU_OVERLAY_BG1:kDioramaPlane_Bg1Far].value);
    assert(r.fold_sources[1].value==textures[kDioramaPlane_Bg1Far].value);
    if(!frame)memcpy(reference,r.fold_vertices,sizeof(reference));
    else for(unsigned i=0;i<8;++i) {
      /* Removing the fold leaves every vertex on/ahead of walking contact
       * fixed, including the rear surface's two contact endpoints. */
      if(frame==2 && (i==2 || i==3))continue;
      assert(fabsf(reference[i].position.x-r.fold_vertices[i].position.x)<.002f);
      assert(fabsf(reference[i].position.y-r.fold_vertices[i].position.y)<.002f);
      if(frame==1) {
        assert(fabsf(reference[i].tex_coord.x-r.fold_vertices[i].tex_coord.x)<.000001f);
        assert(fabsf(reference[i].tex_coord.y-r.fold_vertices[i].tex_coord.y)<.000001f);
      }
    }
    if(top && frame<2) {
      /* Only the rear dirt cap samples the rolled native low-plane strip. */
      assert(fabsf(r.fold_vertices[0].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT-120.5f)<.00001f);
      assert(fabsf(r.fold_vertices[2].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT-113.5f)<.00001f);
      for(unsigned i=0;i<2;++i) {
        const ArRenderPointF a=r.fold_vertices[i].position,b=r.fold_vertices[7-i].position;
        assert(fabsf(a.x-b.x)<.002f && fabsf(a.y-b.y)<.002f);
      }
    }
  }
  if(s_depth_fixture)assert(r.depth_draws && r.depth_actor_vertices);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
static bool CapPointEqual(ArRenderPointF a,ArRenderPointF b) {
  return fabsf(a.x-b.x)<.002f && fabsf(a.y-b.y)<.002f;
}
static bool CapUvEqual(ArRenderPointF a,ArRenderPointF b) {
  return fabsf(a.x-b.x)<.000001f && fabsf(a.y-b.y)<.000001f;
}
static float CapCross(ArRenderPointF a,ArRenderPointF b,ArRenderPointF c) {
  return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
}
static bool CapPointOnEdge(ArRenderPointF p,ArRenderPointF a,ArRenderPointF b) {
  const float dx=b.x-a.x,dy=b.y-a.y,length=hypotf(dx,dy);
  if(length<.001f)return CapPointEqual(p,a);
  const float along=((p.x-a.x)*dx+(p.y-a.y)*dy)/(length*length);
  return fabsf(CapCross(a,b,p))/length<.003f && along>=-.0001f && along<=1.0001f;
}
static void CheckCapTiling(const CapMesh *mesh,const CapMesh *plain,int sx,int sy) {
  assert(plain->count==8 && mesh->count>plain->count);
  const ArRenderPointF outline[]={plain->vertices[4].position,plain->vertices[5].position,
      plain->vertices[2].position,plain->vertices[3].position};
  float area=0;unsigned joins=0,actor_joins=0,boundaries=0;
  for(unsigned face=0;face<mesh->count/4;++face) {
    assert(!mesh->after_actor[face]);
    const ArRenderVertex2D *v=mesh->vertices+face*4;
    area+=fabsf(CapCross(v[0].position,v[1].position,v[2].position))*.5f;
    area+=fabsf(CapCross(v[0].position,v[2].position,v[3].position))*.5f;
    for(unsigned i=0;i<4;++i) {
      const float u=v[i].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH;
      const float t=v[i].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT;
      assert(u>=sx+.5f-.0001f && u<=sx+3.5f+.0001f);
      assert(t>=sy+.5f-.0001f && t<=sy+3.5f+.0001f);
      const ArRenderPointF a=v[i].position,b=v[(i+1)%4].position;
      bool joined=false;
      for(unsigned other=0;other<mesh->count/4 && !joined;++other)if(other!=face)
        for(unsigned j=0;j<4;++j) {
          const ArRenderVertex2D *w=mesh->vertices+other*4;
          if(!CapPointEqual(a,w[(j+1)%4].position) || !CapPointEqual(b,w[j].position))continue;
          joined=true;++joins;
          if(CapPointOnEdge(a,plain->vertices[0].position,plain->vertices[1].position) &&
             CapPointOnEdge(b,plain->vertices[0].position,plain->vertices[1].position)) {
            /* The actor split cuts through a repeat: source phase must agree. */
            assert(CapUvEqual(v[i].tex_coord,w[(j+1)%4].tex_coord));
            assert(CapUvEqual(v[(i+1)%4].tex_coord,w[j].tex_coord));
            ++actor_joins;
          }
          break;
        }
      if(!joined) {
        bool boundary=false;
        for(unsigned j=0;j<4;++j)
          boundary|=CapPointOnEdge(a,outline[j],outline[(j+1)%4]) &&
                    CapPointOnEdge(b,outline[j],outline[(j+1)%4]);
        assert(boundary);++boundaries;
      }
    }
  }
  const float expected=(fabsf(CapCross(outline[0],outline[1],outline[2]))+
                        fabsf(CapCross(outline[0],outline[2],outline[3])))*.5f;
  assert(fabsf(area-expected)<expected*.0001f);
  assert(joins && actor_joins && boundaries);
}
static unsigned AuthoredContourCapFixture(unsigned scenario) {
  s_count=0;
  AuthoredCapProbe probe={.automatic_art=(1u<<1)|(1u<<4)|(1u<<5)|(1u<<6)};
  if(scenario==1)probe.automatic_art&=~(1u<<1); /* Explicit top alpha. */
  if(scenario==2)probe.automatic_art&=~((1u<<4)|(1u<<5)|(1u<<6)); /* Shared side alpha. */
  if(scenario==3)probe.automatic_art&=~((1u<<1)|(1u<<4)|(1u<<5)); /* Verified full material patches. */
  Recorder r={.width=960,.height=600,.bound_effect=-1,.authored_caps=&probe};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=256;voxel->height=224;voxel->ready=true;
  voxel->authored=calloc(1,sizeof(*voxel->authored));assert(voxel->authored);voxel->authored_count=1;
  voxel->authored_at=calloc(16*14,sizeof(*voxel->authored_at));assert(voxel->authored_at);
  voxel->authored_at[6*16+5]=1;
  voxel->authored_art=calloc(2,sizeof(*voxel->authored_art));assert(voxel->authored_art);
  voxel->authored_art_count=voxel->authored_art_capacity=2;
  for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
    voxel->authored_art[0][y*16+x]=x==y?0x83e0:0;
    voxel->authored_art[1][y*16+x]=scenario==3?0x8000|x|(y<<5):((y&1)?0x801f:0);
  }
  uint16_t original[256];memcpy(original,voxel->authored_art[0],sizeof(original));
  VoxelAuthoredCell *a=voxel->authored;
  *a=(VoxelAuthoredCell){.x=80,.y=96,.width=16,.surface=true,
      .faces=kDioramaVoxelFaceMask_All,.automatic_art=probe.automatic_art};
  for(unsigned q=0;q<4;++q){a->front[q]=-.24f;a->back[q]=.3f;a->priority[q]=q&1;}
  for(unsigned face=0;face<7;++face) {
    const bool explicit_art=face==1?scenario==1:face>=4&&scenario==2;
    const bool material=scenario==3 && (face==1 || face==4 || face==5);
    a->art_id[face]=explicit_art||material?1:0;a->pixels[face]=voxel->authored_art[a->art_id[face]];
    if(material) {a->surface_sample_x[face]=512;a->surface_sample_y[face]=256;a->surface_sample_size[face]=face==1?8:16;}
  }
  voxel->authored_contours=calloc(64,sizeof(*voxel->authored_contours));assert(voxel->authored_contours);
  voxel->authored_contour_count=voxel->authored_contour_capacity=a->contour_count=64;
  for(unsigned side=0;side<4;++side)for(unsigned pixel=0;pixel<16;++pixel)
    voxel->authored_contours[side*16+pixel]=(VoxelAuthoredContour){pixel,pixel,1,side};
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.15f,.tilt_y=s_depth_fixture?-.35f:.35f}};
  const DioramaRenderOptions options={.edge_aa=s_depth_fixture,.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=AuthoredCapObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  r.actor_texture=textures[kDioramaPlane_Obj2];
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(probe.uploaded && probe.automatic_caps && probe.transparent_front);
  assert(probe.priorities[0] && probe.priorities[1]);
  assert((probe.explicit_caps>0)==(scenario!=0));
  assert((probe.material_caps>0)==(scenario==3));
  assert((probe.transparent_explicit>0)==(scenario==1 || scenario==2));
  for(unsigned i=0;i<probe.count;++i)assert(probe.submitted[i]);
  assert(!memcmp(original,voxel->authored_art[0],sizeof(original)));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    assert(probe.atlas[y*512+x]==ActionVoxel_AuthoredColor(original[y*16+x],NULL));
  if(s_depth_fixture)assert(r.depth_draws && r.depth_actor_vertices);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
static unsigned VoxelCapFixture(unsigned scenario) {
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=1024;voxel->height=768;voxel->ready=true;
  voxel->edge_count=voxel->pass_counts[0]=1;voxel->pass_edges[0][0]=0;
  /* An interior square is independent of the current contour's rounded rim.
   * Last two cases retain a visible cap but crop its source at right/bottom. */
  const int x=scenario==2?240:80,y=scenario==3?220:120;
  const VoxelEdge edge={.x=x,.y=y,.length=32,.side=0,.far_rank=2,.colliding=1,.textured_top=1,
      .top_sample_x=x+14,.top_sample_y=y+7};
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.15f,.tilt_y=scenario&1?-.4f:.4f}};
  const DioramaRenderOptions options={.edge_aa=s_depth_fixture,.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  r.cap_texture=textures[kDioramaPlane_Bg1Far];r.actor_texture=textures[kDioramaPlane_Obj2];
  CapMesh mesh={0},plain={0},tiled={0};r.cap_mesh=&mesh;
  for(unsigned frame=0;frame<(scenario<2?3u:2u);++frame) {
    voxel->edges[0]=edge;voxel->edges[0].top_sample_size=frame?4:0;
    if(frame==2) {voxel->edges[0].x+=3;voxel->edges[0].length-=3;}
    mesh.count=0;r.actor_seen=false;
    DioramaProjection projection;r.projection=&projection;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && mesh.count && r.actor_seen);
    if(!frame) {
      if(mesh.count!=8)fprintf(stderr,"cap scenario %u: original vertices=%u\n",scenario,mesh.count);
      assert(mesh.count==8);plain=mesh;continue;
    }
    if(scenario>=2) {
      /* An unavailable complete source must use the untouched original cap. */
      assert(mesh.count==plain.count);
      for(unsigned i=0;i<mesh.count;++i) {
        assert(CapPointEqual(mesh.vertices[i].position,plain.vertices[i].position));
        assert(CapUvEqual(mesh.vertices[i].tex_coord,plain.vertices[i].tex_coord));
      }
      assert(!mesh.after_actor[0] && !mesh.after_actor[1]);
    } else if(frame==1) {
      CheckCapTiling(&mesh,&plain,x+14,y+6);tiled=mesh;
    } else {
      /* Shortening a contour by a non-tile amount must not restart its grid
       * or source phase. Surviving interior vertices keep both unchanged. */
      unsigned shared=0;
      for(unsigned i=0;i<mesh.count;++i) {
        bool matched=false;
        for(unsigned j=0;j<tiled.count;++j)
          if(CapPointEqual(mesh.vertices[i].position,tiled.vertices[j].position) &&
              CapUvEqual(mesh.vertices[i].tex_coord,tiled.vertices[j].tex_coord)) {matched=true;break;}
        shared+=matched;
      }
      assert(shared>mesh.count*3/4);
    }
  }
  if(scenario<2) {
    /* An authored neighbour can suppress the original shared donor pixels.
     * The remaining automatic cap uses owned native art at exactly the same
     * projected vertices, depth tiling and native actor-priority split. */
    voxel->authored_art=malloc(sizeof(*voxel->authored_art));assert(voxel->authored_art);
    voxel->authored_art_count=voxel->authored_art_capacity=1;
    for(unsigned p=0;p<256;++p)voxel->authored_art[0][p]=0x83e0;
    voxel->authored_cap_art=malloc(sizeof(*voxel->authored_cap_art));assert(voxel->authored_cap_art);
    voxel->authored_cap_art[0]=0;voxel->authored_cap_art_count=1;
    voxel->edges[0]=edge;voxel->edges[0].top_sample_size=4;voxel->edges[0].top_owned_art=1;
    r.owned_cap=true;DioramaCapture owned_capture=capture;ActionRoomSceneFrameState color={.brightness=15};
    owned_capture.voxel_frame=&color;
    for(unsigned frame=0;frame<3;++frame) {
      if(frame==2)color.brightness=7;
      r.owned_cap_pixel=ActionVoxel_AuthoredColor(0x83e0,&color);mesh.count=0;r.actor_seen=false;
      DioramaProjection projection;r.projection=&projection;
      assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&owned_capture,&view,&scene,&projection)));
      assert(mesh.count==tiled.count && r.uploads==(frame==2?2u:1u));
      for(unsigned i=0;i<mesh.count;++i) {
        assert(CapPointEqual(mesh.vertices[i].position,tiled.vertices[i].position));
        assert(fabsf(mesh.vertices[i].tex_coord.x*512-(tiled.vertices[i].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH-(x+14)))<.0001f);
        assert(fabsf(mesh.vertices[i].tex_coord.y*2048-(tiled.vertices[i].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT-(y+6)))<.0001f);
        if(i%4==0)assert(mesh.after_actor[i/4]==tiled.after_actor[i/4]);
      }
    }
  }
  if(s_depth_fixture)assert(r.depth_draws && r.depth_actor_vertices);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
static ArRenderPointF RootExpectedPoint(const DioramaProjection *p,const DioramaCapture *capture,
    float world_x,float world_y,float depth) {
  /* Independently project native coordinates at the required physical depth.
   * Use the published resolved distance, including auto-fit and zoom. */
  const float x=world_x-capture->bg1_camera_x,y=world_y-capture->camera_y-1;
  assert(isfinite(p->camera_distance) && p->camera_distance>=2);
  const float scale=1+depth/p->camera_distance;
  Scene3DPoint point;
  assert(Scene3D_ProjectWorldPoint(p->matrix,(x/capture->width-.5f)*p->aspect_x*scale,
      (.5f-y/capture->height)*p->height_scale*scale,-depth,p->output_width,p->output_height,&point));
  return (ArRenderPointF){point.x,point.y};
}
/* The base remains attached to the native source plane. A shared domain bends
 * across both cell axes, so editing four cells cannot produce four separate
 * curves. The observer keeps source coordinates for picking and native UVs. */
static unsigned AuthoredAnchoredBendFixture(unsigned scenario) {
  const bool affine=scenario>=3;scenario%=3;
  s_count=0;
  AuthoredCapProbe probe={.automatic_art=(1u<<1)|(1u<<4)|(1u<<5)|(1u<<6)};
  Recorder r={.width=960,.height=600,.bound_effect=-1,.authored_caps=&probe};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=256;voxel->height=224;voxel->ready=true;
  voxel->authored=calloc(4,sizeof(*voxel->authored));assert(voxel->authored);voxel->authored_count=4;
  voxel->authored_at=calloc(16*14,sizeof(*voxel->authored_at));assert(voxel->authored_at);
  voxel->authored_art=malloc(sizeof(*voxel->authored_art));assert(voxel->authored_art);
  voxel->authored_art_count=voxel->authored_art_capacity=1;
  for(unsigned i=0;i<256;++i)voxel->authored_art[0][i]=0x83e0;
  for(unsigned i=0;i<4;++i) {
    VoxelAuthoredCell *a=&voxel->authored[i];
    *a=(VoxelAuthoredCell){.x=80+(i&1)*16,.y=96+(i>>1)*16,.width=16,.surface=true,
        .faces=kDioramaVoxelFaceMask_All,.automatic_art=probe.automatic_art,
        .bend={.axis=scenario?kDioramaVoxelBend_Y:kDioramaVoxelBend_X,
          .anchor=scenario?128:80,.tip=scenario?96:112,
          .dx=scenario==2&&(i&1)?-8:12,.dy=-6,.dz=.35f}};
    if(affine)a->transform=(DioramaVoxelTransform){.px=96,.py=112,.pz=0,
        .dx=18,.dy=9,.dz=-.07f,.sx=.5f,.sy=1.25f,.sz=.7f};
    voxel->authored_at[(a->y/16)*16+a->x/16]=i+1;
    for(unsigned q=0;q<4;++q){a->front[q]=-.2f;a->back[q]=.25f;a->priority[q]=q&1;}
    for(unsigned face=0;face<7;++face)a->pixels[face]=voxel->authored_art[0];
  }
  voxel->authored_contours=calloc(32,sizeof(*voxel->authored_contours));assert(voxel->authored_contours);
  voxel->authored_contour_capacity=32;
  for(unsigned i=0;i<4;++i) {
    VoxelAuthoredCell *a=&voxel->authored[i];a->contour_start=voxel->authored_contour_count;
    for(unsigned side=0;side<4;++side)for(unsigned run=0;run<2;++run) {
      const unsigned x=side==1?15:side==3?0:run*8,y=side==0?0:side==2?15:run*8;
      if(!ActionVoxel_AuthoredContourExposed(voxel,a,x,y,side))continue;
      assert(voxel->authored_contour_count<voxel->authored_contour_capacity);
      voxel->authored_contours[voxel->authored_contour_count++]=(VoxelAuthoredContour){x,y,8,side};
      ++a->contour_count;
    }
  }
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,
      .distance_scale=scenario==2?.8f:scenario==1?1.2f:1,
      .camera={.tilt_x=.15f,.tilt_y=s_depth_fixture?-.35f:.35f,
        .distance=scenario==2?1:scenario==1?3.25f:0}};
  const DioramaRenderOptions options={.edge_aa=s_depth_fixture,.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .voxel_observe_face=AuthoredCapObserve,.voxel_observer_userdata=&probe};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;r.actor_texture=textures[kDioramaPlane_Obj2];
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  if(scenario==1)assert(fabsf(projection.camera_distance-3.25f*1.2f)<.00001f);
  if(scenario==2)assert(projection.camera_distance==2); /* Both pre/post-zoom minimum clamps. */
  assert(r.actor_seen);
  assert(probe.uploaded && probe.automatic_caps && !probe.transparent_front && !probe.transparent_explicit);
  const float split=fmaxf(0,ActionVoxel_PlayRecess(voxel)-.01f);
  unsigned anchors=0,tips=0,shared_x=0,shared_y=0,seam_caps=0,near_caps=0,rear_caps=0;
  for(unsigned f=0;f<probe.count;++f) {
    const DioramaVoxelPickFace *face=&probe.faces[f];assert(probe.submitted[f]);
    const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(voxel,face->cell_x,face->cell_y);assert(a);
    if(face->kind!=kDioramaVoxelFace_Front) {
      if((face->kind==kDioramaVoxelFace_Left || face->kind==kDioramaVoxelFace_Right) &&
          fabsf(face->world_xy[0]-96)<.001f)++seam_caps;
      if(!face->priority) {
        assert(!probe.after_actor[f]);
        float nearest=INFINITY,farthest=-INFINITY;
        for(unsigned j=0;j<4;++j) {
          const float plane=PlaneDepth(&projection,-split,(ArRenderPointF){face->screen_xy[j*2],face->screen_xy[j*2+1]});
          const float delta=face->depth[j]-plane;
          nearest=fminf(nearest,delta);farthest=fmaxf(farthest,delta);
        }
        assert(nearest>=-.00001f || farthest<=.00001f);
        near_caps+=nearest<-.00001f;rear_caps+=farthest>.00001f;
      }
      continue;
    }
    for(unsigned j=0;j<4;++j) {
      const float x=face->world_xy[j*2],y=face->world_xy[j*2+1];
      const float source=scenario?y:x;
      float t=fmaxf(0,fminf(1,(source-a->bend.anchor)/(a->bend.tip-a->bend.anchor)));t=t*t*(3-2*t);
      float depth=-.2f+a->bend.dz*t,px=x+a->bend.dx*t,py=y+a->bend.dy*t;
      if(affine){px=96+(px-96)*.5f+18;py=112+(py-112)*1.25f+9;depth=depth*.7f-.07f;}
      const ArRenderPointF expected=RootExpectedPoint(&projection,&capture,px,py,depth);
      if(fabsf(expected.x-face->screen_xy[j*2])>=.003f || fabsf(expected.y-face->screen_xy[j*2+1])>=.003f)
        fprintf(stderr,"bend scenario=%u depth=%d face=%u vertex=%u source=(%.6f,%.6f) t=%.6f final-depth=%.6f expected=(%.6f,%.6f) actual=(%.6f,%.6f)\n",
            scenario,s_depth_fixture,f,j,x,y,t,depth,expected.x,expected.y,face->screen_xy[j*2],face->screen_xy[j*2+1]);
      assert(fabsf(expected.x-face->screen_xy[j*2])<.003f && fabsf(expected.y-face->screen_xy[j*2+1])<.003f);
      assert(fabsf(face->depth[j]-PlaneDepth(&projection,-depth,expected))<.00001f);
      assert(fabsf(face->art_uv[j*2]-(x-a->x))<.0001f && fabsf(face->art_uv[j*2+1]-(y-a->y))<.0001f);
      if(fabsf(source-a->bend.anchor)<.001f)++anchors;
      if(fabsf(source-a->bend.tip)<.001f)++tips;
      for(unsigned other=0;other<f;++other) {
        const DioramaVoxelPickFace *b=&probe.faces[other];if(b->kind!=kDioramaVoxelFace_Front ||
            (b->cell_x==face->cell_x && b->cell_y==face->cell_y))continue;
        const VoxelAuthoredCell *cell=ActionVoxel_AuthoredCellAt(voxel,b->cell_x,b->cell_y);assert(cell);
        if(cell->bend.dx!=a->bend.dx)continue;
        for(unsigned k=0;k<4;++k)if(fabsf(b->world_xy[k*2]-x)<.0001f && fabsf(b->world_xy[k*2+1]-y)<.0001f) {
          assert(fabsf(b->screen_xy[k*2]-face->screen_xy[j*2])<.001f &&
              fabsf(b->screen_xy[k*2+1]-face->screen_xy[j*2+1])<.001f);
          assert(fabsf(b->depth[k]-face->depth[j])<.000001f);
          shared_x+=b->cell_x!=face->cell_x;shared_y+=b->cell_y!=face->cell_y;
        }
      }
    }
  }
  assert(anchors && tips && shared_y && (scenario==2 || shared_x));
  assert((seam_caps>0)==(scenario==2));assert(near_caps && rear_caps);
  assert(probe.priorities[0] && probe.priorities[1]);
  if(s_depth_fixture)assert(r.depth_draws && r.depth_actor_vertices);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
static bool RootHasPoint(const RootMesh *mesh,ArRenderPointF point,bool front) {
  for(unsigned i=0;i<mesh->count;++i)
    if(mesh->front[i/4]==front && CapPointEqual(mesh->vertices[i].position,point))return true;
  return false;
}
static void CheckRootFaces(const RootMesh *mesh,const DioramaProjection *p,
    const DioramaCapture *capture,const ActionVoxelGeometry *voxel) {
  const int oy=capture->camera_y+1;
  const float top=fmaxf(64,oy),bottom=128;
  const VoxelTrunkCurve *curve=&voxel->trunk_curves[0];
  float area=0;unsigned root_faces=0,joins=0;
  for(unsigned f=0;f<mesh->count/4;++f) {
    if(!mesh->front[f])continue;
    const ArRenderVertex2D *v=mesh->vertices+f*4;
    float x[4],y[4],cx=0,cy=0;
    for(unsigned j=0;j<4;++j) {
      x[j]=v[j].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH+capture->bg1_camera_x;
      y[j]=v[j].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT+oy;cx+=x[j]*.25f;cy+=y[j]*.25f;
    }
    if(cx<=96 || cx>=128 || cy<=top || cy>=bottom)continue;
    ++root_faces;assert(!mesh->after_actor[f]);
    assert(x[1]-x[0]<=8.001f && y[3]-y[0]<=8.001f);
    area+=(x[1]-x[0])*(y[3]-y[0]);
    for(unsigned j=0;j<4;++j) {
      const float depth=ActionVoxel_TrunkFrontDepth(voxel,&voxel->trunk_curves[0],y[j]);
      assert(CapPointEqual(v[j].position,RootExpectedPoint(p,capture,x[j],y[j],depth)));
      const unsigned next=(j+1)%4;
      const bool boundary=(fabsf(x[j]-96)<.001f && fabsf(x[next]-96)<.001f) ||
          (fabsf(x[j]-128)<.001f && fabsf(x[next]-128)<.001f) ||
          (fabsf(y[j]-top)<.001f && fabsf(y[next]-top)<.001f) ||
          (fabsf(y[j]-bottom)<.001f && fabsf(y[next]-bottom)<.001f);
      if(boundary)continue;
      bool joined=false;
      for(unsigned g=0;g<mesh->count/4 && !joined;++g)if(g!=f && mesh->front[g])
        for(unsigned k=0;k<4;++k) {
          const ArRenderVertex2D *w=mesh->vertices+g*4;
          if(CapUvEqual(v[j].tex_coord,w[(k+1)%4].tex_coord) &&
              CapUvEqual(v[next].tex_coord,w[k].tex_coord)) {
            assert(CapPointEqual(v[j].position,w[(k+1)%4].position));
            assert(CapPointEqual(v[next].position,w[k].position));
            joined=true;++joins;break;
          }
        }
      assert(joined);
    }
  }
  assert(root_faces && joins && fabsf(area-32*(bottom-top))<.01f);
  /* The base closes onto the original rear plane; the uncut upper endpoint
   * stays on the existing branch contact. Neither can acquire a tile gap. */
  for(unsigned side=0;side<2;++side) {
    assert(RootHasPoint(mesh,RootExpectedPoint(p,capture,side?128:96,curve->base_y,.18f),true));
    if(top<=curve->top_y)
      assert(RootHasPoint(mesh,RootExpectedPoint(p,capture,side?128:96,curve->top_y,.05f),true));
  }
  unsigned shared_rows=0;
  for(float y=top;;) {
    const float front=ActionVoxel_TrunkFrontDepth(voxel,&voxel->trunk_curves[0],y);
    const float back=ActionVoxel_TrunkBackDepth(voxel,&voxel->trunk_curves[0],y);
    bool joined=false;
    for(unsigned side=0;side<2;++side) {
      const ArRenderPointF a=RootExpectedPoint(p,capture,side?128:96,y,front);
      const ArRenderPointF b=RootExpectedPoint(p,capture,side?128:96,y,back);
      joined|=RootHasPoint(mesh,a,true) && RootHasPoint(mesh,a,false) && RootHasPoint(mesh,b,false);
    }
    if(!joined)fprintf(stderr,"root contour join missing at world y=%g (capture oy=%d)\n",y,oy);
    assert(joined);++shared_rows;
    if(y==curve->base_y)break;
    float next=fminf(curve->base_y,floorf(y/8)*8+8);
    if(y<curve->top_y && next>curve->top_y)next=curve->top_y;
    y=next;
  }
  assert(shared_rows>=5);
}
static void CheckRootControls(const RootMesh *before,const RootMesh *after) {
  unsigned unchanged=0;
  for(unsigned i=0;i<before->count;++i) {
    const ArRenderVertex2D *v=before->vertices+i;
    const float x=v->tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH;
    if(x>=70 && x<=160)continue;
    bool found=false;
    for(unsigned j=0;j<after->count;++j) {
      const ArRenderVertex2D *w=after->vertices+j;
      if(CapUvEqual(v->tex_coord,w->tex_coord) && CapPointEqual(v->position,w->position) &&
          !memcmp(&v->color,&w->color,sizeof(v->color)) &&
          before->after_actor[i/4]==after->after_actor[j/4] && before->front[i/4]==after->front[j/4]) {
        found=true;break;
      }
    }
    assert(found);++unchanged;
  }
  assert(unchanged>32);
}
static void CheckMapPlacement(const RootMesh *native,const RootMesh *moved,
    const DioramaProjection *projection,const DioramaCapture *capture,
    const ActionVoxelGeometry *voxel,ArRenderPointF offset) {
  assert(native->count==moved->count);
  unsigned fronts=0;
  for(unsigned f=0;f<moved->count/4;++f) {
    const ArRenderVertex2D *v=moved->vertices+4*f;
    bool same_source=false;
    for(unsigned g=0;g<native->count/4 && !same_source;++g) {
      const ArRenderVertex2D *w=native->vertices+4*g;
      bool equal=moved->front[f]==native->front[g] && moved->after_actor[f]==native->after_actor[g];
      for(unsigned j=0;j<4;++j)equal&=CapUvEqual(v[j].tex_coord,w[j].tex_coord) &&
          !memcmp(&v[j].color,&w[j].color,sizeof(v[j].color));
      same_source=equal;
    }
    assert(same_source); /* No animation source, tint or native-priority change. */
    if(!moved->front[f])continue;
    float cx=0,cy=0;
    for(unsigned j=0;j<4;++j) {
      cx+=v[j].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH*.25f;
      cy+=(v[j].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT+capture->camera_y+1)*.25f;
    }
    const VoxelTrunkCurve *curve=ActionVoxel_TrunkCurveForCell(voxel,(int)cx/8,(int)cy/8);
    const float recess=ActionVoxel_FrontRecess(voxel,(int)cx,(int)cy);
    for(unsigned j=0;j<4;++j) {
      const float x=v[j].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH;
      const float y=v[j].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT+capture->camera_y+1;
      const float depth=curve?ActionVoxel_TrunkFrontDepth(voxel,curve,y):recess;
      const ArRenderPointF expected=RootExpectedPoint(projection,capture,x+offset.x,y+offset.y,depth);
      assert(CapPointEqual(v[j].position,expected));
    }
    ++fronts;
  }
  assert(fronts>20);
  const DioramaPlaneProjection *planes[]={&projection->bg1_plane,&projection->bg1_high_plane,&projection->bg1_far_plane};
  for(unsigned i=0;i<3;++i)assert(CapUvEqual(planes[i]->capture_offset,offset));
  for(unsigned i=0;i<4;++i)assert(CapUvEqual(projection->object_planes[i].capture_offset,(ArRenderPointF){0}));
  /* These are the public contact projectors consumed by scenery lights,
   * shadows and native effect anchors. Their placement must agree with the
   * shifted native mesh at the walking contact and unrecessed front. */
  ArRenderPointF walking,front;
  assert(Diorama_ProjectCapturedBg1Point(projection,40,80,&walking,NULL,NULL));
  assert(Diorama_ProjectCapturedBg1FarPoint(projection,40,80,&front,NULL,NULL));
  assert(CapPointEqual(walking,RootExpectedPoint(projection,capture,40+offset.x,
      80+capture->camera_y+1+offset.y,ActionVoxel_PlayRecess(voxel))));
  assert(CapPointEqual(front,RootExpectedPoint(projection,capture,40+offset.x,
      80+capture->camera_y+1+offset.y,0)));
}
static unsigned VoxelRootFixture(unsigned scenario) {
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=1024;voxel->height=768;voxel->ready=true;
  voxel->front_recess=calloc((size_t)voxel->width/8,voxel->height/8);
  voxel->trunk_cells=calloc((size_t)voxel->width/8,voxel->height/8);
  assert(voxel->front_recess && voxel->trunk_cells);
  const bool non_grid=scenario>=4;
  voxel->trunk_curves[0]=non_grid?(VoxelTrunkCurve){.top_y=66,.base_y=126}:(VoxelTrunkCurve){.top_y=64,.base_y=128};
  for(int y=8;y<16;++y)for(int x=12;x<16;++x) {
    voxel->front_recess[y*(voxel->width/8)+x]=2;voxel->trunk_cells[y*(voxel->width/8)+x]=1;
  }
  /* Only the lower trunk owns a curve. Separate rope, branch and stump
   * profiles use their existing native geometry and texture contracts. */
  const VoxelEdge edges[]={
    {.x=95,.y=64,.length=non_grid?62:64,.side=1,.far_rank=1,.trunk_curve=1},
    {.x=128,.y=64,.length=non_grid?62:64,.side=3,.far_rank=1,.trunk_curve=1},
    {.x=96,.y=64,.length=32,.side=0,.far_rank=1,.trunk_curve=1},
    {.x=96,.y=non_grid?125:127,.length=32,.side=2,.far_rank=1,.trunk_curve=1},
    {.x=176,.y=56,.length=40,.side=0,.far_rank=3,.colliding=1},
    {.x=207,.y=32,.length=72,.side=1,.far_rank=1},
    {.x=32,.y=136,.length=32,.side=0,.far_rank=2,.colliding=1,.textured_top=1},
  };
  voxel->edge_count=voxel->pass_counts[0]=sizeof(edges)/sizeof(*edges);
  memcpy(voxel->edges,edges,sizeof(edges));for(unsigned i=0;i<voxel->edge_count;++i)voxel->pass_edges[0][i]=i;
  voxel->patch_count=1;voxel->patches[0]=(VoxelPatch){.x=32,.y=136,.width=32,.height=56,.kind=kVoxelPatch_SideProfile};
  DioramaCapture capture={.width=256,.height=224,.camera_y=scenario>=2 && !non_grid?91:7,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.15f,.tilt_y=scenario&1?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  RootMesh before={0},after={0};r.root_texture=textures[kDioramaPlane_Bg1Far];
  r.actor_texture=textures[kDioramaPlane_Obj2];r.capture=&capture;r.root_voxel=voxel;
  DioramaProjection projection;r.projection=&projection;
  for(unsigned frame=0;frame<2;++frame) {
    voxel->trunk_curve_count=frame;r.root_mesh=frame?&after:&before;r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && r.root_mesh->count && r.actor_seen);
  }
  CheckRootFaces(&after,&projection,&capture,voxel);CheckRootControls(&before,&after);
  if(s_depth_fixture)assert(r.root_depth_vertices && r.depth_actor_vertices);
  if(scenario==0 || scenario==2) {
    const ArRenderPointF shifts[]={{2,-1},{-1.02f,.51f},{0,0}};
    ArRenderPointF offsets[kDioramaPlane_Count]={0};
    capture.voxel_map_plane_mask=(1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Bg1Hi)|(1u<<kDioramaPlane_Bg1Far);
    capture.plane_capture_offsets=offsets;r.root_voxel=NULL;
    for(unsigned step=0;step<sizeof(shifts)/sizeof(*shifts);++step) {
      offsets[SR_PPU_OVERLAY_BG1]=offsets[kDioramaPlane_Bg1Hi]=offsets[kDioramaPlane_Bg1Far]=shifts[step];
      RootMesh moved={0};r.root_mesh=&moved;r.actor_seen=false;
      assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
      assert(r.actor_seen);
      CheckMapPlacement(&after,&moved,&projection,&capture,voxel,shifts[step]);
    }
    capture.voxel_map_plane_mask=0;capture.plane_capture_offsets=NULL;
  }
  /* An unpublished/inactive room cannot turn ordinary Diorama into curved
   * Voxel geometry merely because immutable metadata is present. */
  scene.map_group=2;r.root_mesh=NULL;r.root_voxel=NULL;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  const unsigned first=s_count;voxel->trunk_curve_count=0;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  const unsigned second=s_count,length=second-first;voxel->trunk_curve_count=1;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(s_count-second==length && !memcmp(s_trace+first,s_trace+second,length*sizeof(*s_trace)));
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
static bool StumpHasPoint(const CapMesh *mesh,ArRenderPointF point) {
  for(unsigned i=0;i<mesh->count;++i)
    if(CapPointEqual(mesh->vertices[i].position,point))return true;
  return false;
}
static float StumpNativeFrontArea(const RootMesh *mesh,const DioramaCapture *capture,int cell_x,int cell_y) {
  float area=0;
  for(unsigned face=0;face<mesh->count/4;++face)if(mesh->front[face]) {
    float left=INFINITY,right=-INFINITY,top=INFINITY,bottom=-INFINITY;
    for(unsigned j=0;j<4;++j) {
      const ArRenderPointF uv=mesh->vertices[face*4+j].tex_coord;
      const float x=uv.x*SR_PPU_SURFACE_MAX_WIDTH;
      const float y=uv.y*SR_PPU_SURFACE_MAX_HEIGHT+capture->camera_y-capture->authentic_y0+1;
      left=fminf(left,x);right=fmaxf(right,x);top=fminf(top,y);bottom=fmaxf(bottom,y);
    }
    const float width=fminf(right,(cell_x+1)*16)-fmaxf(left,cell_x*16);
    const float height=fminf(bottom,(cell_y+1)*16)-fmaxf(top,cell_y*16);
    if(width>0 && height>0)area+=width*height;
  }
  return area;
}
static unsigned VoxelStumpContourFixture(unsigned scenario) {
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1};ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=1024;voxel->height=768;voxel->ready=true;
  /* A narrow native cut top widens at a bark ledge. Both oblique views must
   * retain the step instead of bridging it with a whole-profile side panel. */
  const VoxelEdge edges[]={
    {.x=84,.y=96,.length=24,.side=0,.far_rank=2,.colliding=1,.textured_top=1,
     .top_sample_x=89,.top_sample_y=103,.top_sample_size=4},
    {.x=84,.y=96,.length=8,.side=3,.far_rank=2,.colliding=1},
    {.x=107,.y=96,.length=8,.side=1,.far_rank=2,.colliding=1},
    {.x=80,.y=104,.length=4,.side=0,.far_rank=2,.colliding=1},
    {.x=108,.y=104,.length=4,.side=0,.far_rank=2,.colliding=1},
    {.x=80,.y=104,.length=48,.side=3,.far_rank=2,.colliding=1},
    {.x=111,.y=104,.length=48,.side=1,.far_rank=2,.colliding=1},
    {.x=80,.y=151,.length=32,.side=2,.far_rank=2,.colliding=1},
  };
  voxel->edge_count=voxel->pass_counts[0]=sizeof(edges)/sizeof(*edges);
  memcpy(voxel->edges,edges,sizeof(edges));for(unsigned i=0;i<voxel->edge_count;++i)voxel->pass_edges[0][i]=i;
  voxel->patches[0]=(VoxelPatch){.x=80,.y=96,.width=32,.height=56,.kind=kVoxelPatch_SideProfile};
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.15f,.tilt_y=scenario&1?-.4f:.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.edge_aa=s_depth_fixture,.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&sink:NULL,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  CapMesh mesh={0},contour={0};r.cap_mesh=&mesh;r.cap_texture=textures[kDioramaPlane_Bg1Far];
  r.actor_texture=textures[kDioramaPlane_Obj2];
  RootMesh fronts={0};r.root_mesh=&fronts;r.root_texture=textures[kDioramaPlane_Bg1Far];r.capture=&capture;
  DioramaProjection projection;r.projection=&projection;
  for(unsigned frame=0;frame<2;++frame) {
    voxel->patch_count=frame;mesh.count=0;fronts.count=0;r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && mesh.count && r.actor_seen);
    if(!frame) {contour=mesh;continue;}
    /* Semantic profile ownership still excludes folds and trunk curves; it
     * must not add an opaque wall over these native contour faces. */
    assert(mesh.count==contour.count);
    for(unsigned i=0;i<mesh.count;++i) {
      assert(CapPointEqual(mesh.vertices[i].position,contour.vertices[i].position));
      assert(CapUvEqual(mesh.vertices[i].tex_coord,contour.vertices[i].tex_coord));
      assert(!memcmp(&mesh.vertices[i].color,&contour.vertices[i].color,sizeof(ArRenderColorF)));
      assert(mesh.after_actor[i/4]==contour.after_actor[i/4]);
    }
  }
  bool visible_side=false,before_actor=false,after_actor=false;
  for(unsigned side=0;side<2;++side) {
    bool complete=true;
    for(unsigned end=0;end<4;++end)for(unsigned back=0;back<2;++back) {
      const float x=end<2?(side?108:84):(side?112:80);
      const float y=end==0?96:end==3?152:104;
      complete&=StumpHasPoint(&mesh,RootExpectedPoint(&projection,&capture,x,y,
          back?ActionVoxel_Depth(voxel):0));
    }
    visible_side|=complete;
  }
  for(unsigned i=0;i<mesh.count/4;++i) {
    before_actor|=!mesh.after_actor[i];after_actor|=mesh.after_actor[i];
  }
  assert(visible_side && before_actor && !after_actor);
  if(s_depth_fixture)assert(r.depth_draws && r.depth_actor_vertices);
  /* A Front-only visibility edit must filter the original front mesh while
   * retaining the stump's stepped contour and shared native donor texture.
   * Restoring All removes that filter instead of compiling a box over it. */
  assert(fabsf(StumpNativeFrontArea(&fronts,&capture,5,6)-256)<.01f);
  assert(fabsf(StumpNativeFrontArea(&fronts,&capture,6,6)-256)<.01f);
  voxel->authored=calloc(1,sizeof(*voxel->authored));
  voxel->authored_at=calloc((size_t)voxel->width/16,voxel->height/16*sizeof(*voxel->authored_at));
  assert(voxel->authored && voxel->authored_at);
  voxel->authored[0]=(VoxelAuthoredCell){.x=80,.y=96,.width=16,.native_front_hidden=true,
      .faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front};
  voxel->authored_at[6*(voxel->width/16)+5]=1;
  for(unsigned frame=0;frame<2;++frame) {
    voxel->authored_count=voxel->hidden_native_front_count=frame?0:1;
    mesh.count=fronts.count=0;r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(mesh.count==contour.count && r.actor_seen);
    for(unsigned i=0;i<mesh.count;++i) {
      assert(CapPointEqual(mesh.vertices[i].position,contour.vertices[i].position));
      assert(CapUvEqual(mesh.vertices[i].tex_coord,contour.vertices[i].tex_coord));
      assert(!memcmp(&mesh.vertices[i].color,&contour.vertices[i].color,sizeof(ArRenderColorF)));
      assert(mesh.after_actor[i/4]==contour.after_actor[i/4]);
    }
    assert(fabsf(StumpNativeFrontArea(&fronts,&capture,5,6)-(frame?256:0))<.01f);
    assert(fabsf(StumpNativeFrontArea(&fronts,&capture,6,6)-256)<.01f);
  }
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
#include "diorama_native_transform.inc"
#include "diorama_surface_texture.inc"
#include "diorama_surface_native_x.inc"
#include "diorama_native_actor_priority.inc"
#include "diorama_profile_surface.inc"
typedef struct SubscreenNested {
  ArRenderDevice *device;
  ArRenderTexture target;
  unsigned calls;
} SubscreenNested;
static void SubscreenNestedEffect(void *context,int plane,const DioramaProjection *projection) {
  SubscreenNested *nested=context;(void)projection;
  if(plane!=SR_PPU_OVERLAY_BG1)return;
  ArRenderTargetState state;
  assert(ArRenderDevice_BeginTarget(nested->device,nested->target,&state)==kArRenderTargetBegin_Ready);
  assert(state.target.value && state.target.value!=nested->target.value);
  assert(ArRenderDevice_Clear(nested->device,(ArRenderColorF){0,0,0,0}));
  assert(ArRenderDevice_EndTarget(nested->device,&state));
  ++nested->calls;
}
static unsigned VoxelSubscreenFixture(unsigned scenario) {
  const bool complete_sources=scenario>=8;
  if(complete_sources)scenario-=8;
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count]={0};
  const uint8_t *pixels[kDioramaPlane_Count]={0};const uint8_t present=255;
  const unsigned planes[]={SR_PPU_OVERLAY_BG2,SR_PPU_OVERLAY_BG1,kDioramaPlane_Bg1Far,kDioramaPlane_Obj2,
      SR_PPU_OVERLAY_OBJ,kDioramaPlane_Obj1,kDioramaPlane_Bg2Hi,kDioramaPlane_Bg1Hi,
      kDioramaPlane_Obj3,SR_PPU_OVERLAY_BG3};
  const unsigned plane_count=complete_sources?10:4;
  const uint32_t additive=(1u<<SR_PPU_OVERLAY_BG1)|(1u<<kDioramaPlane_Bg1Far)|(1u<<kDioramaPlane_Obj2)|
      (complete_sources?((1u<<SR_PPU_OVERLAY_OBJ)|(1u<<kDioramaPlane_Obj1)|
          (1u<<kDioramaPlane_Bg1Hi)|(1u<<kDioramaPlane_Obj3)):0);
  for(unsigned i=0;i<plane_count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[planes[i]]));
    pixels[planes[i]]=&present;
    r.subscreen_sources[textures[planes[i]].value]=(additive&(1u<<planes[i]))!=0;
  }
  SubscreenNested nested={.device=&device};
  if(scenario==1)assert(Create(&r,&(ArRenderTextureDesc){.width=32,.height=32,
      .format=kArRenderPixelFormat_Argb8888,.usage=kArRenderTextureUsage_Target},&nested.target));
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=5;voxel->active_room=1;voxel->profile=kActionVoxelProfile_MarahnaJungle;
  voxel->settings=ActionVoxel_SettingsForRoom(5,1);voxel->width=1024;voxel->height=512;voxel->ready=true;
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={8,10,944,580},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.18f,.tilt_y=scenario&1?.4f:-.4f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaScene scene={.render=&options,.map_group=5,.map_number=scenario==6?2:1,
      .voxel_geometry=scenario==5?NULL:voxel,
      .additive_plane_mask=scenario==7?0:additive,
      .plane_effect=scenario==1?SubscreenNestedEffect:NULL,.plane_effect_userdata=&nested};
  r.check_subscreen=scenario<5;
  /* The main layer's supersample is the first scratch allocation. Fail the
   * following full-output subscreen, not that optional AA fallback. */
  if(scenario==2)r.fail_create_at=r.creates+2;
  if(scenario==3)r.fail_subscreen_draw=true;
  if(scenario==4)r.fail_restore=true;
  const bool failure=scenario>=2 && scenario<=4;
  for(unsigned frame=0;frame<(failure?1u:2u);++frame) {
    const unsigned creates=r.creates,adds=r.subscreen_adds,draws=r.subscreen_draws;
    DioramaProjection projection;
    const PresentationOutcome outcome=Diorama_Composite(&device,&capture,&view,&scene,&projection);
    assert(PresentationOutcome_IsUsable(outcome)!=failure && projection.valid!=failure);
    if(complete_sources && !failure) {
      float x0,y0,x1,y1;
      assert(projection.bg1_plane.source_bounds_valid);
      assert(Diorama_PlaneCaptureBounds(&projection,&projection.bg1_plane,&x0,&y0,&x1,&y1));
      assert(fabsf(x0)<.001f && fabsf(y0)<.001f &&
          fabsf(x1-capture.width)<.001f && fabsf(y1-capture.height)<.001f);
      assert(!projection.bg2_plane.source_bounds_valid && !projection.object_planes[2].source_bounds_valid);
    }
    if(failure)assert(r.subscreen_adds==adds);
    else if(r.check_subscreen) {
      const unsigned minimum_sources=complete_sources?7:3;
      if(r.subscreen_adds!=adds+1 || r.subscreen_draws<draws+minimum_sources)
        fprintf(stderr,"subscreen case=%u adds=%u sources=%u\n",scenario,r.subscreen_adds-adds,r.subscreen_draws-draws);
      assert(r.subscreen_adds==adds+1 && r.subscreen_draws>=draws+minimum_sources);
    }
    if(frame)assert(r.creates==creates);
    if(scenario!=4)assert(!r.state.target.value && !r.state.viewport_set);
  }
  if(scenario==1)assert(nested.calls==2);
  if(scenario==4)assert(r.draws==r.draws_at_failed_restore);
  r.fail_restore=false;r.state.target=ArRenderTexture_Invalid();
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  if(nested.target.value)Destroy(&r,nested.target);
  for(unsigned i=0;i<plane_count;++i)Destroy(&r,textures[planes[i]]);
  assert(!r.live);return s_count;
}
static unsigned TrunkDepthFixture(unsigned scenario) {
  s_count=0;
  static ActionTrunkAssets trunk;memset(&trunk,0,sizeof(trunk));
  trunk.room=6;trunk.height=512;trunk.revision=100+scenario;
  for(unsigned y=80;y<160;++y)trunk.masks[0][y*48+10]=255;
  for(unsigned y=140;y<144;++y)for(unsigned x=14;x<22;++x)trunk.masks[1][y*48+x]=255;
  for(unsigned i=0;i<32*32;++i)trunk.bark[i]=0xff304050;
  Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true,.trunk=&trunk};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX,
      .maximum_texture_width=8192,.maximum_texture_height=8192,
      .maximum_render_target_width=8192,.maximum_render_target_height=8192}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  const DioramaCapture capture={.width=256,.height=224,.bg1_camera_x=384,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={8,10,944,580},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.18f,.tilt_y=scenario&1?.4f:-.4f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1,
      .depth_of_field=true,.edge_aa=true};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.map_group=6,.map_number=6,
      .trunk=&trunk,.trunk_settings=DioramaTrunk_Default(),.depth_sink=scenario<2?&sink:NULL};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  for(unsigned frame=0;frame<2;++frame) {
    const unsigned creates=r.creates;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    if(frame)assert(r.creates==creates);
  }
  if(scenario<2)assert(r.depth_draws && r.depth_actor_vertices && r.depth_trunk_vertices);
  else assert(!r.depth_draws);
  Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
static unsigned TrunkSideFixture(unsigned scenario) {
  s_count=0;
  const bool right=!!(scenario&1),positive_yaw=!!(scenario&2),shared_depth=scenario<4;
  static ActionTrunkAssets trunk;memset(&trunk,0,sizeof(trunk));
  trunk.room=6;trunk.height=512;trunk.revision=1000+scenario*2;trunk.side_width=1024;
  const int start=right?706:270,end=right?754:318;
  for(unsigned high=0;high<2;++high)for(unsigned y=110+high*40;y<118+high*40;++y)
    for(int x=start;x<end;++x) {
      /* Snow terrace descends by 1px, then 2px, and returns by 2px then
       * 1px. Both yaw signs see each short-riser length. The outer walls
       * remain 8px tall and the underside is one unbroken horizontal run. */
      const int offset=x-start;
      const unsigned snow_top=offset<8?110:offset<16?111:offset<24?113:offset<32?111:110;
      if(!high && y<snow_top)continue;
      const unsigned at=y*kActionTrunkSideMaskPitch+(unsigned)x/8,bit=1u<<(x&7);
      trunk.side_masks[kActionTrunkSideSolid][at]|=bit;
      if(high) {trunk.side_masks[kActionTrunkSidePriority][at]|=bit;++trunk.side_high_pixels;}
      trunk.side_masks[kActionTrunkSideSnow][at]|=bit;
      /* Both bodies are a reviewed snow family. Only the first one's
       * vertical outline is native white snow ink; the second one's blue
       * body must not become snow merely because of that shared family. */
      if(!high && (x==start || x==end-1))trunk.side_masks[kActionTrunkSideSnowFace][at]|=bit;
    }
  /* Deliberately distinct two-dimensional art catches swapped rows, atlas
   * quadrants, accidental 1px bands and camera-relative source phase. */
  for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
    trunk.bark[y*32+x]=0xff200000u|(x<<8)|y;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    trunk.side_snow[y*16+x]=0xffa00000u|(y<<8)|x;
  TrunkSideProbe probe={.snow_start=start};
  Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true,.trunk=&trunk,.trunk_side=&probe};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX,
      .maximum_texture_width=8192,.maximum_texture_height=8192,
      .maximum_render_target_width=8192,.maximum_render_target_height=8192}));
  ArRenderTexture textures[kDioramaPlane_Count];const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=640,.height=352,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  r.actor_texture=textures[kDioramaPlane_Obj2];
  DioramaCapture capture={.width=256,.height=224,.bg1_camera_x=right?608:160,
      .camera_y=31,.textures=textures,.pixels=pixels};
  DioramaView view={.viewport={8,10,944,580},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.18f,.tilt_y=positive_yaw?.4f:-.4f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.map_group=6,.map_number=6,
      .trunk=&trunk,.trunk_settings=DioramaTrunk_Default(),.depth_sink=shared_depth?&sink:NULL};
  DioramaProjection projection;r.projection=&projection;r.capture=&capture;
  for(unsigned frame=0;frame<5;++frame) {
    if(frame==2) {capture.bg1_camera_x+=7;capture.camera_y+=11;view.camera.tilt_y*=.8f;}
    if(frame==3)view.camera.tilt_x=-.18f;
    if(frame==4) {++trunk.revision;trunk.bark[5]^=0x00000100;}
    const unsigned creates=r.creates,uploads=r.uploads,atlas_uploads=probe.atlas_uploads;
    probe.caps[0]=probe.caps[1]=probe.snow_caps=probe.bark_caps=0;
    probe.taper_vertices=probe.full_vertices=probe.far_vertices=probe.high_vertices=0;
    probe.one_pixel_risers=probe.two_pixel_risers=probe.white_walls=probe.blue_walls=probe.undersides=0;
    r.actor_seen=false;
    assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
    assert(projection.valid && r.actor_seen && probe.caps[0] && probe.caps[1]);
    assert(probe.snow_caps && probe.bark_caps && probe.taper_vertices && probe.full_vertices);
    assert(probe.one_pixel_risers && probe.two_pixel_risers>=2 && probe.white_walls && probe.blue_walls);
    if(frame>=3)assert(probe.undersides);
    if(shared_depth)assert(probe.far_vertices && probe.high_vertices && r.depth_actor_vertices);
    else assert(!r.depth_draws);
    if(frame>0 && frame<4)assert(r.creates==creates && r.uploads==uploads);
    if(frame==4)assert(r.creates==creates+2 && r.uploads==uploads+2 && probe.atlas_uploads==atlas_uploads+1);
  }
  Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
/* This is an actual published forest deck contour, not a synthetic arbitrary
 * trapezoid. Rotate the fixed native capture while following one physical
 * midpoint on its rear cap. Projection W must keep that point on one texel. */
static unsigned PerspectiveVoxelDeck(unsigned scenario) {
  s_count=0;
  Recorder r={.width=1280,.height=800,.bound_effect=-1,.perspective_capture=true};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=voxel->active_room=1;voxel->profile=kActionVoxelProfile_FillmoreForest;
  voxel->settings=ActionVoxel_SettingsForRoom(1,1);voxel->width=4096;voxel->height=768;voxel->ready=true;
  voxel->edges[0]=(VoxelEdge){.x=912,.y=640,.length=128,.side=0,.far_rank=2,
      .colliding=1,.textured_top=1,.rear_fold_top=7};
  voxel->edge_count=voxel->pass_counts[0]=1;voxel->pass_edges[0][0]=0;
  const DioramaCapture capture={.width=624,.height=352,.authentic_y0=64,
      .bg1_camera_x=897,.camera_y=488,.textures=textures,.pixels=pixels};
  const DioramaView view={.viewport={0,0,1280,800},.visible_width=496,.distance_scale=1,
      .camera={.tilt_x=.15f,.tilt_y=scenario?.4f:-.4f,.distance=3.25f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=&sink,
      .map_group=1,.map_number=1,.voxel_geometry=voxel};
  DioramaProjection projection;r.projection=&projection;r.actor_texture=textures[kDioramaPlane_Obj2];
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(projection.valid && r.perspective_count==8 && r.depth_actor_vertices);
  assert(!r.perspective_after_actor[0] && !r.perspective_after_actor[1]);
  /* The first quad is behind native actor contact. Derive its clip coordinates
   * independently from the captured native edge and the public camera matrix. */
  Scene3DClipPoint expected[4];
  for(unsigned j=0;j<4;++j) {
    const bool end=j==1 || j==2,back=j>=2;
    const float z=back?.18f:.05f,scale=1+z/3.25f;
    const float x=912+128*end-(897-(624-256)/2),y=640-7*back-(488-64+1);
    assert(Scene3D_TransformToClip(projection.matrix,(x/624-.5f)*projection.aspect_x*scale,
        (.5f-y/352)*projection.height_scale*scale,-z,&expected[j]));
    const ArRenderVertex2D *v=r.perspective_vertices+j;
    assert(fabsf(r.perspective_w[j]-expected[j].w)<.000002f);
    assert(fabsf(v->position.x-(expected[j].x/expected[j].w*.5f+.5f)*1280)<.002f);
    assert(fabsf(v->position.y-(1-expected[j].y/expected[j].w*.5f-.5f)*800)<.002f);
    Record(r.perspective_w[j]);
  }
  const double a=expected[0].w/(expected[0].w+expected[2].w),b=1-a;
  const double u0=r.perspective_vertices[0].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH;
  const double u2=r.perspective_vertices[2].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH;
  const double fixed_u=(u0+u2)*.5,affine=a*u0+b*u2;
  const double corrected=(a*u0/r.perspective_w[0]+b*u2/r.perspective_w[2])/
      (a/r.perspective_w[0]+b/r.perspective_w[2]);
  assert(fabs(corrected-fixed_u)<.00002 && fabs(affine-fixed_u)>.9);
  /* Rotating the camera reverses the legacy drift; the corrected midpoint
   * stays at source pixel263, with unchanged native split ordering. */
  assert(fabs(fixed_u-263)<.00002 && (scenario?affine<fixed_u:affine>fixed_u));
  Record((float)corrected);Record((float)(affine-fixed_u));
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
/* Native semantic tests establish which Aitos cloud pixels enter each
 * texture. This fixture checks the real compositor contract they depend on:
 * equal projected planes at both yaws, with separate native priority slots. */
static unsigned VoxelCloudPriorityFixture(unsigned scenario) {
  s_count=0;
  Recorder r={.width=960,.height=600,.bound_effect=-1,.shaders=true};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));
    pixels[i]=&present;
  }
  ActionVoxelGeometry *voxel=ActionVoxel_Create();assert(voxel);
  voxel->active_group=4;voxel->active_room=1;voxel->profile=kActionVoxelProfile_AitosMountains;
  voxel->settings=ActionVoxel_SettingsForRoom(4,1);
  voxel->width=1024;voxel->height=768;voxel->ready=true;
  /* Face-only cloud artwork introduces no solid edges or contact caps. */
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels,
      .bg1_camera_x=48,.camera_y=32};
  const DioramaView view={.viewport={0,0,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_x=.18f,.tilt_y=scenario?.4f:-.4f}};
  /* Assert exact native-art UV/XY/depth correspondence. The separately
   * tested AA fringe deliberately expands screen XY and clamps UVs while
   * retaining boundary depth, so it is not another point on this plane. */
  const DioramaRenderOptions options={.visible_planes=(1u<<kDioramaPlane_Count)-1};
  const DioramaDepthSink depth_sink={.submit=DepthGeometry,.userdata=&r};
  const DioramaScene scene={.render=&options,.depth_sink=s_depth_fixture?&depth_sink:NULL,
      .map_group=4,.map_number=1,.voxel_geometry=voxel};
  r.cloud_textures[0]=textures[kDioramaPlane_Bg1Far];
  r.cloud_textures[1]=textures[kDioramaPlane_Bg1Hi];
  r.actor_texture=textures[kDioramaPlane_Obj2];r.capture=&capture;
  DioramaProjection projection;r.projection=&projection;
  assert(PresentationOutcome_IsUsable(Diorama_Composite(&device,&capture,&view,&scene,&projection)));
  assert(projection.valid && projection.voxel_scenery && r.actor_seen);
  assert(r.cloud_vertices[0] && r.cloud_vertices[1]);
  assert(projection.bg1_far_plane.valid && projection.bg1_high_plane.valid);
  assert(fabsf(projection.bg1_far_plane.z_world)<.000001f);
  assert(fabsf(projection.bg1_high_plane.z_world)<.000001f);
  assert(projection.object_planes[2].valid);
  assert(fabsf(projection.object_planes[2].z_world-(.01f-ActionVoxel_PlayRecess(voxel)))<.000001f);
  if(s_depth_fixture)assert(r.cloud_depth_vertices[0] && r.cloud_depth_vertices[1] && r.depth_actor_vertices);
  ActionVoxel_Release(voxel);Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}

/* Resident sky draws and later attached effects each own one ordered scope.
 * The two scopes must not span intervening target or ordinary renderer work. */
enum { kSkyBatchBands = 10 };
static bool SkyBandBatch(ArRenderDevice *device,bool begin) {
  Recorder *r=device->context;SkyBatchProbe *p=&r->sky_batch;
  if(begin) {
    assert(!r->batch_open);
    ++p->attempts;
    if(p->attempts==p->fail_begin)return false;
    p->failed=false;
  }
  const bool ok=SourceBatch(device,begin);
  return ok && (begin || (!p->failed && p->attempts!=p->fail_end));
}
static bool SkyBandDraw(ArRenderDevice *device,ArRenderTexture texture,
    const DioramaSkyboxSourceDraw *draw) {
  Recorder *r=device->context;SkyBatchProbe *p=&r->sky_batch;
  assert(texture.value && r->batch_open==p->grouped);
  assert(!r->state.target.value && !p->effect_calls);
  assert(draw->mapping.meta[0]==kSkyBatchBands && draw->band==p->sky_calls);
  /* Keep the raw empty span in both ordered callback sequences. */
  if(draw->band==4)assert(draw->mapping.bands[4][0]==draw->mapping.bands[4][1]);
  Record(24);Record(draw->band);
  ++p->sky_calls;
  if(p->fail_draw==1 && draw->band==3)p->failed=true;
  return !p->failed;
}
static void SkyBandEffect(void *context,int plane,const DioramaProjection *projection) {
  Recorder *r=context;SkyBatchProbe *p=&r->sky_batch;
  assert(r->batch_open==p->grouped && !r->state.target.value && !r->state.viewport_set);
  assert(projection->valid);
  if(plane!=SR_PPU_OVERLAY_BG2) {
    assert(plane==kDioramaPlane_Obj2);
    assert(!p->grouped || p->attempts==3); /* Separate native actor slot scope. */
    assert(p->effect_calls==kSkyBatchBands && projection->bg2_skybox.active_band==-1);
    ++p->actor_calls;return;
  }
  assert(projection->bg2_skybox.count==kSkyBatchBands && p->sky_calls==kSkyBatchBands);
  assert(projection->bg2_skybox.active_band==(int)p->effect_calls);
  assert(!r->actor_seen); /* Native foreground/actor work follows EndBatch. */
  Record(25);Record(projection->bg2_skybox.active_band);
  ++p->effect_calls;
  if(p->fail_draw==2 && projection->bg2_skybox.active_band==3)p->failed=true;
}
static unsigned SkyBatchFixture(unsigned scenario) {
  s_count=0;memset(&s_layers,0,sizeof(s_layers));
  Recorder r={.width=976,.height=620,.bound_effect=-1};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device,&kOps,&r,(ArRenderCapabilities){.flags=UINT64_MAX}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];const uint8_t present=255;
  for(int i=0;i<kDioramaPlane_Count;++i) {
    assert(Create(&r,&(ArRenderTextureDesc){.width=SR_PPU_SURFACE_MAX_WIDTH,
        .height=SR_PPU_SURFACE_MAX_HEIGHT,.format=kArRenderPixelFormat_Argb8888,
        .usage=kArRenderTextureUsage_Streaming},&textures[i]));
    pixels[i]=&present;
  }
  DioramaBgValidSpanPlan spans={.count=kSkyBatchBands};
  for(unsigned i=0;i<kSkyBatchBands;++i)
    spans.spans[i]=(DioramaBgValidSpan){i*224/kSkyBatchBands,(i+1)*224/kSkyBatchBands,
        i%2?8:0,i==4?0:256};
  const DioramaCapture capture={.width=256,.height=224,.textures=textures,.pixels=pixels,
      .bg2_valid_spans=&spans};
  const DioramaView view={.viewport={8,10,960,600},.visible_width=256,.distance_scale=1,
      .camera={.tilt_y=.2f}};
  const DioramaRenderOptions options={.visible_planes=(1u<<SR_PPU_OVERLAY_BG2)|(1u<<kDioramaPlane_Obj2),
      .skybox=kDioramaSky_Only,.margin_fix=true,.layers=&s_layers,
      .draw_resident_skybox=SkyBandDraw,.source_effect_batch=scenario==1?NULL:SkyBandBatch};
  const DioramaScene scene={.render=&options,.map_group=2,.map_number=1,
      .effect_bg_plane_mask=1u<<SR_PPU_OVERLAY_BG2,
      .plane_effect=SkyBandEffect,.plane_effect_userdata=&r};
  r.actor_texture=textures[kDioramaPlane_Obj2];
  DioramaProjection projection;r.capture=&capture;r.projection=&projection;
  for(unsigned frame=0;frame<2;++frame) {
    r.batch_begins=r.batch_ends=0;r.actor_seen=false;
    r.sky_batch=(SkyBatchProbe){.grouped=scenario!=1};
    const bool failing=!frame && scenario>=2;
    if(failing) {
      if(scenario==2 || scenario==5)r.sky_batch.fail_begin=scenario==2?1:2;
      if(scenario==3 || scenario==6)r.sky_batch.fail_end=scenario==3?1:2;
      if(scenario==4 || scenario==7)r.sky_batch.fail_draw=scenario==4?1:2;
    }
    const PresentationOutcome outcome=Diorama_Composite(&device,&capture,&view,&scene,&projection);
    assert(!r.batch_open && r.batch_begins==r.batch_ends);
    assert(!r.state.target.value && !r.state.viewport_set && !r.state.clip_enabled);
    if(failing) {
      assert(outcome==kPresentationOutcome_CoreFailure && !projection.valid && !r.actor_seen);
      assert(!projection.bg2_skybox.count); /* Failed frames invalidate the complete projection. */
      const bool sky_failure=scenario<=4;
      assert(r.sky_batch.attempts==(sky_failure?1u:2u));
      assert(r.sky_batch.sky_calls==(scenario==2?0u:kSkyBatchBands));
      assert(r.sky_batch.effect_calls==(scenario<=5?0u:kSkyBatchBands));
      assert(r.batch_ends==(scenario==2?0u:scenario<=5?1u:2u));
      assert(!r.sky_batch.actor_calls);
    } else {
      assert(PresentationOutcome_IsUsable(outcome) && projection.valid);
      assert(projection.bg2_skybox.active_band==-1);
      assert(r.sky_batch.sky_calls==kSkyBatchBands && r.sky_batch.effect_calls==kSkyBatchBands);
      assert(r.sky_batch.actor_calls==1);
      assert(r.batch_begins==(scenario==1?0u:3u));
    }
    Record(outcome);Record(r.batch_begins);Record(r.batch_ends);
  }
  Diorama_ResetCompositorResources(&device);
  for(int i=0;i<kDioramaPlane_Count;++i)Destroy(&r,textures[i]);
  assert(!r.live);return s_count;
}
#include "diorama_voxel_cull.inc"
#include "diorama_voxel_endpoint.inc"
#include "diorama_ground_cover_compositor.inc"
#include "diorama_waterfall_continuation.inc"
#include "diorama_death_heim_water.inc"
#include "diorama_trunk_guard_compositor.inc"
#include "diorama_owned_heat_compositor.inc"
#include "diorama_tomb_contour_skin.inc"
#include "diorama_voxel_guard_compositor.inc"
#include "diorama_desert_columns.inc"
#include "diorama_waterfall_effect_bounds.inc"
#include "diorama_waterfall_cloud_coords.inc"
#include "diorama_jungle_water_color.inc"
#include "diorama_castle_terminal_matte.inc"
#include "diorama_jungle_terminal.inc"
#include "diorama_summit_crest_skin.inc"
#include "diorama_death_heim_nebula.inc"
#include "diorama/diorama_sand_surface.h"
#include "diorama_sand_transitions.inc"
#include "diorama_snow_closing_riser_compositor.inc"
#include "diorama_voxel_clipped_cover.inc"
#include "diorama_ramp_taper.inc"
unsigned DioramaFixture_Count(void) { return 662; }
const float *DioramaFixture_Trace(void) { return s_trace; }
unsigned DioramaFixture_Run(unsigned scenario) {
  assert(scenario < DioramaFixture_Count());
  if(scenario>=654)return RampTaperFixture(scenario-654);
  if(scenario>=650)return ClippedCoverFixture(scenario-650);
  if(scenario>=644)return NativeActorPriorityFixture(scenario-644);
  if(scenario>=640)return VoxelNativeXSurfaceFixture(scenario-640);
  if(scenario>=636) { SnowCoverRender(scenario-636); return s_count; }
  if(scenario>=632)return JungleTerminalFixture(scenario-632);
  if(scenario>=624) { DesertFrontFixture(scenario-624); return s_count; }
  if(scenario>=608) {
    if(scenario==608)SandTransitionBoundsFixture();
    SandTransitionRenderFixture(scenario-608);
    return s_count;
  }
  if(scenario>=600)return DeathHeimNebulaFixture(scenario-600);
  if(scenario>=592)return SummitCrestSkinRenderFixture(scenario-592);
  if(scenario>=584)return CastleTerminalMatteFixture(scenario-584);
  if(scenario>=580)return JungleWaterColorFixture(scenario-580);
  if(scenario>=572)return WaterfallCloudFixture(scenario-572);
  if(scenario>=564)return WaterfallEffectBoundsFixture(scenario-564);
  if(scenario>=556)return VoxelGuardFixture(scenario-556);
  if(scenario>=540) {
    s_depth_fixture=scenario>=548;
    return TombContourSkinRenderFixture((scenario-540)%8);
  }
  if(scenario>=536)return OwnedHeatTransportFixture(scenario-536);
  if(scenario>=528)return TrunkGuardFixture(scenario-528);
  if(scenario>=520)return GroundCoverTransformTaperFixture(scenario-520);
  if(scenario>=512)return DeathHeimWaterFixture(scenario-512);
  if(scenario>=504)return WaterfallContinuationFixture(scenario-504);
  if(scenario>=500)return GroundCoverSnowFixture(scenario-500);
  if(scenario>=484)return GroundCoverFixture(scenario-484);
  if(scenario>=476)return TrunkSideFixture(scenario-476);
  if(scenario>=472)return VoxelEndpointFixture(scenario-472);
  if(scenario>=468)return VoxelCullFixture(scenario-468);
  if(scenario>=466)return VoxelSubscreenFixture(8+scenario-466);
  if(scenario>=450) {
    s_depth_fixture=scenario>=458;
    return VoxelMaterialSkinFixture(16+(scenario-450)%8);
  }
  if(scenario>=442) {
    s_depth_fixture=scenario>=446;
    return VoxelProfileSurfaceFixture((scenario-442)%4);
  }
  if(scenario>=438) {
    s_depth_fixture=scenario>=440;
    return VoxelFlatRearJoinFixture((scenario-438)%2);
  }
  if(scenario>=422) {
    s_depth_fixture=scenario>=430;
    return VoxelMaterialSkinFixture(8+(scenario-422)%8);
  }
  if(scenario>=420) {
    s_depth_fixture=scenario>=421;
    return AuthoredContourCapFixture(3);
  }
  if(scenario>=404) {
    s_depth_fixture=scenario>=412;
    return VoxelMaterialSkinFixture((scenario-404)%8);
  }
  if(scenario>=400) {
    s_depth_fixture=scenario>=402;
    return NativeAffineFixture((scenario-400)%2);
  }
  if(scenario>=394) {
    s_depth_fixture=scenario>=397;
    return AuthoredAnchoredBendFixture(3+(scenario-394)%3);
  }
  if(scenario>=388) {
    s_depth_fixture=scenario>=391;
    return AuthoredAnchoredBendFixture((scenario-388)%3);
  }
  if(scenario>=382) {
    s_depth_fixture=scenario>=385;
    return AuthoredContourCapFixture((scenario-382)%3);
  }
  if(scenario>=374)return SkyBatchFixture(scenario-374);
  if(scenario>=370) {
    s_depth_fixture=scenario>=372;
    return VoxelCloudPriorityFixture((scenario-370)%2);
  }
  if(scenario>=368)return PerspectiveVoxelDeck(scenario-368);
  if(scenario>=364) {
    s_depth_fixture=scenario>=366;
    return VoxelStumpContourFixture((scenario-364)%2);
  }
  if(scenario>=360) {
    s_depth_fixture=scenario>=362;
    return VoxelRootFixture(4+(scenario-360)%2);
  }
  if(scenario>=352) {
    s_depth_fixture=scenario>=356;
    return VoxelRootFixture((scenario-352)%4);
  }
  if(scenario>=344) {
    s_depth_fixture=scenario>=348;
    return VoxelCapFixture((scenario-344)%4);
  }
  if(scenario>=334) {
    s_depth_fixture=scenario>=339;
    return VoxelFoldFixture((scenario-334)%5);
  }
  s_depth_fixture=scenario>=236;
  if(scenario>=330)return TrunkDepthFixture(scenario-330);
  if(scenario>=320)return VoxelSurfaceFixture(scenario-320);
  if(scenario>=290)return VoxelWaterFixture(scenario-290);
  if(scenario>=278)return VoxelGrowthFixture(scenario-278);
  if(scenario>=236)return VoxelSpriteFixture(scenario-236);
  if(scenario>=206)return VoxelSpriteFixture(scenario-206+12);
  if(scenario>=198)return VoxelSubscreenFixture(scenario-198);
  if(scenario>=192)return VoxelSpriteFixture(scenario-192+6);
  if(scenario>=182)return VoxelWaterFixture(scenario-182+20);
  if(scenario>=172)return VoxelSurfaceFixture(scenario-172);
  if(scenario>=166)return VoxelSpriteFixture(scenario-166);
  if(scenario>=156)return VoxelWaterFixture(scenario-156+10);
  if(scenario>=144)return VoxelGrowthFixture(scenario-144);
  if(scenario>=134)return VoxelWaterFixture(scenario-134);
  if (scenario>=120) return AstralFixture(scenario-120);
  s_count = 0;
  memset(&s_layers, 0, sizeof(s_layers));
  const int widths[] = {640,960,960}, heights[] = {480,540,600};
  const int extension[] = {0,32,64,128};
  const unsigned aspect = scenario % 3, ext = (scenario / 3) % 4;
  const bool death_heim = scenario >= 108;
  const bool completion = death_heim && ((scenario - 108) & 2u);
  const unsigned zoom = (scenario / 12) % 3;
  const unsigned sky = death_heim ? (scenario - 108) / 4 : scenario / 36;
  Recorder r = {.width = widths[aspect]+16, .height = heights[aspect]+20,
    .shaders = scenario % 2 != 0, .bound_effect = -1};
  ArRenderDevice device;
  assert(ArRenderDevice_Init(&device, &kOps, &r,
      (ArRenderCapabilities){.flags = UINT64_MAX,
        .maximum_texture_width = 8192, .maximum_texture_height = 8192,
        .maximum_render_target_width = 8192, .maximum_render_target_height = 8192}));
  ArRenderTexture textures[kDioramaPlane_Count];
  const uint8_t *pixels[kDioramaPlane_Count];
  DioramaCoverageMask masks[kDioramaPlane_Count];
  ArRenderPointF offsets[kDioramaPlane_Count];
  const uint8_t pixel = 255;
  for (int i = 0; i < kDioramaPlane_Count; ++i) {
    assert(Create(&r, &(ArRenderTextureDesc){.width = SR_PPU_SURFACE_MAX_WIDTH,
        .height = SR_PPU_SURFACE_MAX_HEIGHT, .format = kArRenderPixelFormat_Argb8888,
        .usage = kArRenderTextureUsage_Streaming}, &textures[i]));
    pixels[i] = &pixel; /* Presence only; captured uploads belong to the caller. */
    masks[i] = DioramaCoverage_Dilate(UINT64_C(1) << ((i * 3) % 48));
    offsets[i] = (ArRenderPointF){0.25f,-0.5f};
  }
  DioramaRenderOptions options = {
    .visible_planes = (1u << kDioramaPlane_Count)-1,
    .skybox = sky, .depth_shade = 0.65f, .hud_flat = scenario % 5 == 0,
    .shoebox = scenario % 7 == 0, .margin_fix = true,
    .shadow_blur = true, .rim_light = true, .depth_of_field = true, .edge_aa = true,
    .stack_grouping = true, .sparse_coverage = true,
    .skybox_prefilter = true, .priority_surface = true,
    .layers = &s_layers, .resolve_skybox = Skybox,
    .skybox_userdata = &textures[SR_PPU_OVERLAY_BG2],
    .source_effect_batch = SourceBatch,
  };
  if (scenario % 4 == 0) options.visible_planes &= ~(1u << kDioramaPlane_Bg1Far);
  s_layers.count = 1;
  DioramaRoomOverride *room = &s_layers.rooms[0];
  room->map_group = death_heim ? 7 : 1;
  room->map_number = death_heim ? 1 : 2;
  room->section = completion ? kDioramaLayerSection_DeathHeimCompletion :
      kDioramaLayerSection_Room;
  DioramaPlaneOverride *bg1 = &room->planes[SR_PPU_OVERLAY_BG1];
  bg1->set_rake = bg1->set_bow = true;
  bg1->rake = scenario % 2 ? 0 : 0.17f;
  bg1->bow = scenario % 2 ? 0 : -0.09f;
  room->planes[kDioramaPlane_Bg1Hi] = *bg1;
  room->planes[kDioramaPlane_Bg1Hi].set_z = true;
  room->planes[kDioramaPlane_Bg1Hi].z = 0.5f;
  DioramaPlaneOverride *bg2 = &room->planes[SR_PPU_OVERLAY_BG2];
  bg2->set_stack = bg2->set_stack_copies = true;
  bg2->stack = 0.12f;
  bg2->stack_copies = 3;
  if (!death_heim && scenario % 9 == 0) {
    room->planes[kDioramaPlane_Backdrop].set_source = true;
    room->planes[kDioramaPlane_Backdrop].source = kDioramaLayerSource_AitosSky;
  }
  DioramaCapture capture = {
    .width = ext == 3 ? 576 : (ext ? 384 : 256),
    .height = 224 + (ext == 3 ? extension[ext] : 2 * extension[ext]),
    .authentic_y0 = extension[ext], .obj_apron = ext == 3 ? 32 : 64,
    .textures = textures, .pixels = pixels, .coverage_masks = masks,
    .plane_capture_offsets = offsets, .camera_y = 320,
    .bg2_camera_y = 160, .bg2_world_height = 1024, .bg2_vertical_ratio = 0x12,
    .bg2_scroll_valid = true, .bg2_revision = 1,
    .vertical_bounds = {.valid = true, .plane = SR_PPU_OVERLAY_BG1,
      .bottom_reached = scenario % 6 == 0},
    .horizontal_bounds = {.valid = true, .plane = SR_PPU_OVERLAY_BG1,
      .left = 0, .right = 1, .left_reached = scenario % 5 == 0},
  };
  DioramaView view = {
    .camera = {.tilt_x = 0.1f, .tilt_y = -0.15f,
      .distance = scenario % 4 == 3 ? 4.5f : 0},
    .distance_scale = 0.8f + zoom * 0.2f,
    .camera_framing_weight = scenario % 3 * 0.5f,
    .pixel_aspect = scenario % 2, .ignore_aspect_ratio = scenario % 7 == 0,
    .visible_width = capture.width,
    .viewport = {8,10,widths[aspect],heights[aspect]},
  };
  const DioramaBgValidSpanPlan spans = {.count = 2, .spans = {
    {0, capture.height / 2, capture.obj_apron + 16, capture.obj_apron + 256},
    {capture.height / 2, capture.height, capture.obj_apron, capture.obj_apron + capture.width},
  }};
  const DioramaSkyboxView skybox = {
    .texture = textures[SR_PPU_OVERLAY_BG2], .revision = 1, .width = capture.width,
    .periodic = scenario % 8 == 0, .capture_offset = {0.25f,-0.5f},
  };
  if (scenario % 2) capture.bg2_valid_spans = &spans;
  if (!death_heim && scenario % 4 == 0) capture.skybox = &skybox;
  if (scenario % 5 == 0) capture.bg_apron_mask = 3;
  if (scenario % 6 == 0) capture.vertical_bounds.top_reached = true;
  const float exposure_cuts[]={.1f,.3f,.8f};
  const SceneryShadingField exposure={.x=exposure_cuts,.y=exposure_cuts,
      .nx=3,.ny=3,.sample=Exposure,.userdata=&r};
  const bool periodic_lighting = !death_heim && capture.skybox &&
      capture.skybox->periodic && sky != kDioramaSky_Off && scenario % 9 != 0;
  if (periodic_lighting) r.shaders = true;
  DioramaScene scene = {.render = &options,
    .map_group = room->map_group, .map_number = room->map_number,
    .layer_section = room->section,
    .bg1_dimming = 0.23f, .bg1_dimming_ramp = {0.1f,0.2f,0.2f,0.1f},
    .scenery_exposure = scenario==0?&exposure:NULL,
    .effect_obj_priority_mask = 15, .effect_bg_plane_mask = 3,
    .plane_effect = PlaneEffect, .plane_effect_userdata = &r,
    .scenery_light = r.shaders ? SceneryLight : NULL,
    .backdrop = completion ? Backdrop : NULL, .backdrop_userdata = &r};
  DioramaProjection projection;
  if (periodic_lighting) {
    scene.cloud_light = (DioramaCloudLight){.source = {64,96,640,352}, .meta = {1},
        .regions = {{.bounds = {360,-80,800,900}, .core = {0,0,1,0},
          .shape = {1,.4f,.12f,.9f}, .motion = {-.3f,0,17,3},
          .color = {1,.9f,.8f,1}, .spill = {.8f,.8f,.7f,1}}}};
  }
  r.capture = &capture;
  r.projection = &projection;
  const unsigned base_resources = r.live;
  for (int frame = 0; frame < 2; ++frame) {
    unsigned creates = r.creates;
    const PresentationOutcome outcome = Diorama_Composite(
        &device, &capture, &view, &scene, &projection);
    assert(PresentationOutcome_IsUsable(outcome));
    assert(projection.valid && r.draws && r.bound_effect == -1);
    if (periodic_lighting)
      assert(r.periodic_cloud_bindings == (unsigned)frame + 1);
    if (projection.bg1_plane.valid) {
      const unsigned before = r.draws;
      ArRenderRectF footprint;
      assert(Diorama_ResolveCaptureFootprint(&capture, &view, &scene, SR_PPU_OVERLAY_BG1, &footprint));
      assert(r.draws == before); /* A map guide cannot cause a render pass. */
      for (int y = 0; y <= capture.height; y += 16)
        for (int x = -capture.obj_apron; x <= capture.width + capture.obj_apron; x += 16) {
          ArRenderPointF p;
          if (!Diorama_ProjectCapturedBg1Point(&projection, x, y, &p, NULL, NULL) ||
              p.x < view.viewport.x || p.x > view.viewport.x + view.viewport.w ||
              p.y < view.viewport.y || p.y > view.viewport.y + view.viewport.h) continue;
          assert(x >= footprint.x - .01f && x <= footprint.x + footprint.w + .01f);
          assert(y >= footprint.y - .01f && y <= footprint.y + footprint.h + .01f);
        }
    }
    /* Culling must cover the same guards as the drawn BG/OBJ meshes, without
     * changing the UV mapping checked against their vertex positions above. */
    const DioramaPlaneProjection *planes[]={&projection.bg1_plane,&projection.bg1_high_plane,
      &projection.bg1_far_plane,&projection.bg2_plane,&projection.bg2_high_plane,
      &projection.object_planes[0],&projection.object_planes[1],
      &projection.object_planes[2],&projection.object_planes[3]};
    for(unsigned i=0;i<sizeof(planes)/sizeof(*planes);++i) {
      const DioramaPlaneProjection *p=planes[i];if(!p->valid)continue;
      const bool guard=i>=5 || ((capture.bg_apron_mask>>(i<3?0:1))&1);
      assert(p->capture_apron==(guard?capture.obj_apron:0));
      float x0,y0,x1,y1;
      assert(Diorama_PlaneCaptureBounds(&projection,p,&x0,&y0,&x1,&y1));
      assert(fabsf(x0+p->capture_offset.x+(guard?capture.obj_apron:0))<.001f);
      assert(fabsf(x1+p->capture_offset.x-capture.width-(guard?capture.obj_apron:0))<.001f);
    }
    if(scenario==0) {
      assert(r.exposure_samples>0);
      /* Dimming must shade the guards too, using their real texture/world
       * coordinates rather than clamping at the ordinary viewport edges. */
      assert(r.exposure_u0<=.001f);
      assert(r.exposure_u1+0.001f >=
          (float)(capture.width+2*capture.obj_apron)/SR_PPU_SURFACE_MAX_WIDTH);
    }
    if (death_heim) {
      assert(projection.bg2_plane.valid == (sky != kDioramaSky_Only || !completion));
      if (sky != kDioramaSky_Off && !completion) {
        assert(r.skybox_inputs & (1u << kDioramaPlane_Bg2Far));
        assert(projection.bg2_skybox.count == 1);
        assert(projection.bg2_skybox.motion_source == 4);
        /* The enlarged face image must end above the moving cloud plane.
         * Check the published transforms used by effect/light projection too. */
        DioramaProjection water = projection;
        water.bg2_skybox = (DioramaSkyboxProjection){0};
        ArRenderPointF cloud, face;
        const float bottom = capture.authentic_y0 + 9 * 16;
        assert(Diorama_ProjectCapturedBg2Point(&water, capture.width * 0.5f,
            bottom, &cloud, NULL, NULL));
        assert(Diorama_ProjectSkyboxAnchorPoint(&projection, bottom,
            capture.width * 0.5f, bottom, &face));
        const float clearance = projection.output_height * 8.0f / 224;
        if (cloud.y >= projection.output_y + clearance)
          assert(face.y <= cloud.y - clearance + 0.01f);
        if (sky == kDioramaSky_Only) {
          /* Lower the existing water, including its published effect mapping,
           * until its bottom covers the viewport at both projected sides. */
          for (int side = 0; side < 2; side++) {
            ArRenderPointF edge;
            assert(Diorama_ProjectCapturedBg2Point(&water,
                side * capture.width, capture.authentic_y0 + 224 - 4,
                &edge, NULL, NULL));
            assert(edge.y >= projection.output_y + projection.output_height - 0.01f);
          }
          assert(!water.bg2_plane.overflow_valid);
        }
      } else if (sky == kDioramaSky_Only) {
        assert(r.skybox_inputs & (1u << SR_PPU_OVERLAY_BG2));
        assert(projection.bg2_skybox.motion_source == 3);
      }
    }
    if (r.shaders) assert(r.checked_object_meshes >= (unsigned)(frame+1)*4);
    assert(!r.state.target.value && !r.state.viewport_set && !r.state.clip_enabled);
    assert(r.light_preparations == (r.shaders ? (unsigned)frame+1 : 0));
    if(r.shaders) {
      assert(r.light_skybox.count == projection.bg2_skybox.count);
      assert(r.light_skybox.motion_source == projection.bg2_skybox.motion_source);
      assert(!memcmp(r.light_skybox.bands,projection.bg2_skybox.bands,
          projection.bg2_skybox.count*sizeof(projection.bg2_skybox.bands[0])));
    }
    assert(r.live <= base_resources + 8); /* Two SS/DOF, priority, stack, skybox, illumination. */
    if (frame) assert(r.creates == creates); /* Retained intermediates, no churn. */
    Record(15);
    Record(outcome);
    for (int i = 0; i < 16; ++i) Record(projection.matrix[i]);
    Record(projection.aspect_x);
    Record(projection.height_scale);
  }
  assert(!r.batch_open && r.batch_begins == r.batch_ends);
  if (r.shaders) {
    /* Failure inside a callback still closes its scope before the compositor
     * restores the target. Submission failure must also reject the frame. */
    r.fail_light = true;
    assert(Diorama_Composite(&device,&capture,&view,&scene,&projection) == kPresentationOutcome_CoreFailure);
    assert(!r.batch_open && r.batch_begins == r.batch_ends);
    r.fail_light = false;
    r.fail_submit = true;
    assert(Diorama_Composite(&device,&capture,&view,&scene,&projection) == kPresentationOutcome_CoreFailure);
    assert(!r.batch_open && r.batch_begins == r.batch_ends);
    r.fail_submit = false;
  }
  if (scenario == 0) {
    const DioramaCapture good_capture = capture;
    const DioramaView good_view = view;
    const unsigned before = s_count;
    for (int bad = 0; bad < 5; ++bad) {
      capture = good_capture;
      view = good_view;
      if (bad == 0) capture.width = 0;
      if (bad == 1) capture.obj_apron = INT32_MAX;
      if (bad == 2) capture.authentic_y0 = INT32_MAX;
      if (bad == 3) view.camera.tilt_y = NAN;
      if (bad == 4) view.distance_scale = 0;
      assert(Diorama_Composite(&device, &capture, &view, &scene, &projection) ==
          kPresentationOutcome_CoreFailure);
      assert(!projection.valid && s_count == before);
    }
    capture = good_capture;
    view = good_view;
    options.visible_planes &= ~(1u << SR_PPU_OVERLAY_BG1);
    assert(PresentationOutcome_IsUsable(
        Diorama_Composite(&device, &capture, &view, &scene, &projection)));
    assert(!projection.bg1_plane.valid);
    options.visible_planes |= 1u << SR_PPU_OVERLAY_BG1;
    r.fail_draw = true;
  } else if (scenario == 1) {
    r.fail_restore = true;
  } else if (scenario == 2) {
    /* Changing room/capture size evicts bounded scratch entries without
     * retaining a texture for every view ever visited by the editor. */
    for (int width = 272; width <= 368; width += 16) {
      capture.width = width;
      assert(PresentationOutcome_IsUsable(
          Diorama_Composite(&device, &capture, &view, &scene, &projection)));
      assert(r.live <= base_resources + 7);
    }
  } else if (scenario == 3) {
    device.capabilities.flags &= ~kArRenderCapability_ScopedRenderTargets;
    assert(Diorama_Composite(&device, &capture, &view, &scene, &projection) ==
        kPresentationOutcome_OptionalOmitted);
    assert(projection.valid && r.bound_effect == -1);
  }
  if (r.fail_draw || r.fail_restore) {
    assert(Diorama_Composite(&device, &capture, &view, &scene, &projection) ==
        kPresentationOutcome_CoreFailure);
    assert(!projection.valid && r.bound_effect == -1);
    if (r.fail_restore) {
      /* A failed backend restore loses ownership. The compositor must stop;
       * the host then abandons the frame before resetting device resources. */
      assert(r.draws == r.draws_at_failed_restore);
      Target(&r, ArRenderTexture_Invalid());
    } else {
      assert(!r.state.target.value && !r.state.viewport_set && !r.state.clip_enabled);
    }
  }
  Diorama_ResetCompositorResources(&device);
  Diorama_ResetCompositorResources(&device);
  assert(r.live == base_resources);
  for (int i = 0; i < kDioramaPlane_Count; ++i) Destroy(&r, textures[i]);
  assert(r.live == 0);
  return s_count;
}

#ifndef AR_COMPOSITOR_WASM
int main(int argc, char **argv) {
  FILE *stream = argc == 2 ? fopen(argv[1], "wb") : NULL;
  assert(argc == 1 || stream);
  for (unsigned test = 0; test < DioramaFixture_Count(); ++test) {
    unsigned count = DioramaFixture_Run(test);
    if (!stream) continue;
    for (unsigned i = 0; i <= count; ++i) {
      uint32_t word = count;
      if (i) memcpy(&word, &s_trace[i-1], sizeof(word));
      const uint8_t bytes[] = {word, word>>8, word>>16, word>>24};
      assert(fwrite(bytes, 1, 4, stream) == 4);
    }
  }
  if (stream) assert(fclose(stream) == 0);
  else printf("Diorama compositor: %u scenarios passed, including Voxel water and vegetation ordering\n",DioramaFixture_Count());
  return 0;
}
#endif
