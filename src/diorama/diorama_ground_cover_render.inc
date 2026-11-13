/* Included after the compositor's view/layer types. The existing cap builders
 * own geometry and native draw slots; this adapter only stages their cover. */
#include "diorama_ground_cover.inc"
#include "action/action_ground_cover_light.h"
enum { kGroundCoverMaximumFaces=16384,kGroundCoverMaximumLights=16384,
  kGroundCoverLightHashSize=32768,kGroundCoverCapVertices=8192 };
typedef struct GroundCoverLight {
  ActionGroundCoverLightSample source;
  ArRenderColorF body,crest;
  unsigned char bank;
  bool ready;
} GroundCoverLight;
typedef struct GroundCoverLightSlot { uint32_t epoch,index; } GroundCoverLightSlot;
typedef struct GroundCoverFace {
  ArRenderVertex2D vertices[3];
  float depths[3],clip_w[3],distance;
  uint16_t lights[3];
  float mix[3],alpha[3];
} GroundCoverFace;
typedef struct GroundCoverFaceOrder {float distance;unsigned index;} GroundCoverFaceOrder;
typedef struct GroundCoverDrawState {
  const ActionSceneEffectFrame *frame;
  ActionGroundCoverBank banks[kActionAuthoredMaxInstances];
  unsigned char active[kActionAuthoredMaxInstances];
  unsigned active_count,face_count,light_count;
  uint32_t epoch;
  bool lighting;
  const DioramaRenderOptions *options;
  ActionGroundCoverLightSamplesFn light;
  float fade;
  unsigned bank_caps[kActionAuthoredMaxInstances],bank_triangles[kActionAuthoredMaxInstances];
} GroundCoverDrawState;
static GroundCoverDrawState s_ground_cover;
static DioramaGroundCoverCache s_ground_cover_associations;
static GroundCoverFace s_ground_cover_faces[kGroundCoverMaximumFaces];
static GroundCoverFaceOrder s_ground_cover_order[kGroundCoverMaximumFaces];
static GroundCoverLight s_ground_cover_lights[kGroundCoverMaximumLights];
static GroundCoverLightSlot s_ground_cover_light_hash[kGroundCoverLightHashSize];
static ActionGroundCoverLightSample s_ground_cover_light_work[kGroundCoverMaximumLights];
static uint16_t s_ground_cover_light_indices[kGroundCoverMaximumLights];
static DioramaGroundCoverVertex s_ground_cover_cap[kGroundCoverCapVertices];
static ArRenderVertex2D s_ground_cover_vertices[kGroundCoverMaximumFaces*3];
static float s_ground_cover_depths[kGroundCoverMaximumFaces*3],s_ground_cover_clip_w[kGroundCoverMaximumFaces*3];
static int32_t s_ground_cover_indices[kGroundCoverMaximumFaces*3];

static bool GroundCoverPrepare(const DioramaScene *scene,DioramaProjection *projection) {
  s_ground_cover.frame=NULL;s_ground_cover.active_count=0;
  s_ground_cover.options=scene->render;
  s_ground_cover.face_count=s_ground_cover.light_count=0;
  if(projection)memset(projection->voxel_ground_cover_claims,0,sizeof(projection->voxel_ground_cover_claims));
  /* Dense cover is authored scenery. Disabling flakes/loose grains must not
   * restore the old flat top or remove this receiving-surface layer. */
  if(!scene->trunk_effects||
      (!HollowPlaySpace(scene)&&!ActionVoxel_Active(scene->voxel_geometry,scene->map_group,scene->map_number)))return true;
  const ActionSceneEffectFrame *frame=scene->trunk_effects;
  if(frame->authored_count>kActionAuthoredMaxInstances||frame->actor_count>kActionEffectActorMax)return false;
  s_ground_cover.frame=frame;s_ground_cover.lighting=scene->trunk_lighting;
  s_ground_cover.light=scene->ground_cover_light;
  s_ground_cover.fade=fmaxf(0,fminf(1,1-scene->native_bloom_dimming));
  memset(s_ground_cover.bank_caps,0,sizeof(s_ground_cover.bank_caps));
  memset(s_ground_cover.bank_triangles,0,sizeof(s_ground_cover.bank_triangles));
  if(!++s_ground_cover.epoch){memset(s_ground_cover_light_hash,0,sizeof(s_ground_cover_light_hash));++s_ground_cover.epoch;}
  for(unsigned i=0;i<frame->authored_count;++i) {
    if(!ActionGroundCover_Prepare(&frame->authored[i],&s_ground_cover.banks[i]))continue;
    s_ground_cover.active[s_ground_cover.active_count++]=(unsigned char)i;
    if(projection)projection->voxel_ground_cover_claims[i/64]|=UINT64_C(1)<<(i%64);
    if(scene->render->voxel_observe_ground_cover) {
      const ActionGroundCoverBank *bank=&s_ground_cover.banks[i];
      DioramaGroundCoverObservation observation={.bank=i,.event=kDioramaGroundCoverBegin,
          .claim=kActionGroundCoverClaimedEmpty,.point_count=bank->point_count,.complete=true};
      for(unsigned j=0;j<bank->point_count;++j) {
        observation.path_xy[j*2]=bank->points[j].x;observation.path_xy[j*2+1]=bank->points[j].y;
      }
      scene->render->voxel_observe_ground_cover(scene->render->voxel_observer_userdata,&observation);
    }
  }
  return true;
}
static void GroundCoverObserveEnd(const DioramaScene *scene,bool success) {
  if(!scene||!scene->render||scene->render!=s_ground_cover.options||!scene->render->voxel_observe_ground_cover)return;
  for(unsigned i=0;i<s_ground_cover.active_count;++i) {
    const unsigned bank=s_ground_cover.active[i];unsigned lights=0;
    for(unsigned j=0;j<s_ground_cover.light_count;++j)lights+=s_ground_cover_lights[j].bank==bank;
    const DioramaGroundCoverObservation observation={.bank=bank,.event=kDioramaGroundCoverEnd,
        .claim=ActionGroundCover_ResolveClaim(true,true,success,s_ground_cover.bank_caps[bank]),
        .caps=s_ground_cover.bank_caps[bank],.triangles=s_ground_cover.bank_triangles[bank],
        .light_samples=lights,.complete=success};
    scene->render->voxel_observe_ground_cover(scene->render->voxel_observer_userdata,&observation);
  }
}
static void GroundCoverBeginBatch(void) {s_ground_cover.face_count=0;}
static bool GroundCoverLightIndex(unsigned bank,const DioramaGroundCoverVertex *v,unsigned *index) {
  const ActionGroundCoverLightSample source={.native_x=v->native_x,
      .native_floor_y=v->sample.native_floor_y,.height=v->sample.height,
      .body=v->sample.body,.crest=v->sample.crest};
  uint64_t hash=GroundCoverKey(UINT64_C(14695981039346656037),&bank,sizeof(bank));
  hash=GroundCoverKey(hash,&source,sizeof(source));
  unsigned at=(unsigned)(hash%kGroundCoverLightHashSize);
  for(unsigned probe=0;probe<kGroundCoverLightHashSize;++probe) {
    GroundCoverLightSlot *slot=&s_ground_cover_light_hash[at];
    if(slot->epoch!=s_ground_cover.epoch) {
      if(s_ground_cover.light_count==kGroundCoverMaximumLights)return false;
      *index=s_ground_cover.light_count++;
      s_ground_cover_lights[*index]=(GroundCoverLight){.source=source,.bank=(unsigned char)bank};
      *slot=(GroundCoverLightSlot){s_ground_cover.epoch,*index};return true;
    }
    GroundCoverLight *light=&s_ground_cover_lights[slot->index];
    if(light->bank==bank&&!memcmp(&light->source,&source,sizeof(source))){*index=slot->index;return true;}
    at=(at+1)%kGroundCoverLightHashSize;
  }
  return false;
}
static bool GroundCoverShadeBatch(void) {
  for(unsigned active=0;active<s_ground_cover.active_count;++active) {
    const unsigned bank=s_ground_cover.active[active];unsigned count=0;
    for(unsigned i=0;i<s_ground_cover.light_count;++i) {
      const GroundCoverLight *light=&s_ground_cover_lights[i];
      if(light->ready||light->bank!=bank)continue;
      s_ground_cover_light_indices[count]=(uint16_t)i;
      s_ground_cover_light_work[count++]=light->source;
    }
    if(count&&s_ground_cover.light&&!s_ground_cover.light(s_ground_cover.frame,
        &s_ground_cover.frame->authored[bank],s_ground_cover.lighting,!s_ground_cover.lighting,
        s_ground_cover_light_work,count))return false;
    for(unsigned i=0;i<count;++i) {
      GroundCoverLight *light=&s_ground_cover_lights[s_ground_cover_light_indices[i]];
      light->body=s_ground_cover_light_work[i].body;light->crest=s_ground_cover_light_work[i].crest;light->ready=true;
    }
  }
  for(unsigned i=0;i<s_ground_cover.face_count;++i)for(unsigned j=0;j<3;++j) {
    GroundCoverFace *face=&s_ground_cover_faces[i];
    const GroundCoverLight *light=&s_ground_cover_lights[face->lights[j]];
    const float t=face->mix[j],fade=s_ground_cover.fade;
    face->vertices[j].color=(ArRenderColorF){
        (light->body.r+(light->crest.r-light->body.r)*t)*fade,
        (light->body.g+(light->crest.g-light->body.g)*t)*fade,
        (light->body.b+(light->crest.b-light->body.b)*t)*fade,face->alpha[j]};
  }
  return true;
}
/* Generic cap depth is positive-backward and uses perspective compensation;
 * dedicated caps supply signed scene Z and keep their existing native scale. */
static bool GroundCoverProject(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const float xyz[3],bool dedicated,ArRenderPointF *screen,float *depth,float *w) {
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  const float u=(xyz[0]-ox)/SR_PPU_SURFACE_MAX_WIDTH,v=(xyz[1]-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
  const float scale=dedicated?1:1+xyz[2]/fmaxf(1,g->camera.distance);
  const float x=((u-g->u0)/(g->u1-g->u0)-.5f)*g->aspect_x*scale;
  const float y=(.5f-(v-g->v0)/(g->v1-g->v0))*g->height_scale*scale;
  Scene3DClipPoint clip;
  if(!Scene3D_TransformToClip(g->matrix,x,y,dedicated?xyz[2]:-xyz[2],&clip)||
      clip.w<=kScene3DMinimumProjectionDepth)return false;
  screen->x=(clip.x/clip.w*.5f+.5f)*g->width;
  screen->y=(1-(clip.y/clip.w*.5f+.5f))*g->height;
  *depth=clip.z/clip.w*.5f+.5f;*w=clip.w;
  return isfinite(screen->x)&&isfinite(screen->y)&&isfinite(*depth);
}
static bool GroundCoverGather(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer,DioramaGroundCoverOwner owner,
    const DioramaGroundCoverCap *cap,bool dedicated,unsigned *first,unsigned *added) {
  *first=s_ground_cover.face_count;*added=0;
  if(!s_ground_cover.active_count||g->options->voxel_inspection==kDioramaVoxelInspect_Backing)return true;
  for(unsigned active=0;active<s_ground_cover.active_count;++active) {
    const unsigned bank=s_ground_cover.active[active];const ActionGroundCoverBank *b=&s_ground_cover.banks[bank];
    if(fmaxf(cap->from.x,cap->to.x)<b->points[0].x||
        fminf(cap->from.x,cap->to.x)>b->points[b->point_count-1].x)continue;
    const DioramaGroundCoverAssociation *association;
    if(!DioramaGroundCover_Associate(&s_ground_cover_associations,owner,b,cap->from,cap->to,
        ACTION_GROUND_COVER_MAX_CONTACT_TOLERANCE,cap->receiver,&association))return false;
    if(!association->count)continue;
    unsigned count;ActionGroundCoverPressure pressure;
    if(!DioramaGroundCover_PrepareCapPressure(b,s_ground_cover.frame,cap,&pressure))return false;
    if(!DioramaGroundCover_BuildCap(&s_ground_cover_associations,owner,b,
        &pressure,cap,ACTION_GROUND_COVER_MAX_CONTACT_TOLERANCE,
        s_ground_cover_cap,kGroundCoverCapVertices,&count))return false;
    if(count)++s_ground_cover.bank_caps[bank];
    const unsigned first_overlay=count;
    if(count&&g->options->voxel_observe_ground_cover) {
      /* Union native-X coverage rather than reporting the cap bounding box:
       * an authored cutout can remove a whole middle strip. */
      ActionGroundCoverInterval ranges[64];unsigned nr=0;bool complete=true;
      for(unsigned i=0;i<count;i+=3) {
        float lo=fminf(s_ground_cover_cap[i].native_x,fminf(s_ground_cover_cap[i+1].native_x,s_ground_cover_cap[i+2].native_x));
        float hi=fmaxf(s_ground_cover_cap[i].native_x,fmaxf(s_ground_cover_cap[i+1].native_x,s_ground_cover_cap[i+2].native_x));
        if(hi-lo<.000001f)continue;
        unsigned at=0;while(at<nr&&ranges[at].t1<lo-.00001f)++at;
        unsigned end=at;while(end<nr&&ranges[end].t0<=hi+.00001f){lo=fminf(lo,ranges[end].t0);hi=fmaxf(hi,ranges[end].t1);++end;}
        if(at==end&&nr==64){complete=false;break;}
        memmove(ranges+at+1,ranges+end,(nr-end)*sizeof(*ranges));
        nr=nr-(end-at)+1;ranges[at]=(ActionGroundCoverInterval){lo,hi};
      }
      for(unsigned i=0;i<nr;++i) {
        const float a=(ranges[i].t0-cap->from.x)/(cap->to.x-cap->from.x),z=(ranges[i].t1-cap->from.x)/(cap->to.x-cap->from.x);
        const DioramaGroundCoverObservation observation={.bank=bank,.event=kDioramaGroundCoverSupport,
            .claim=kActionGroundCoverClaimedGeometry,.support_xy={ranges[i].t0,cap->from.y+(cap->to.y-cap->from.y)*a,
                ranges[i].t1,cap->from.y+(cap->to.y-cap->from.y)*z},.plane=layer->description->plane,
            .dedicated=dedicated,.complete=complete};
        g->options->voxel_observe_ground_cover(g->options->voxel_observer_userdata,&observation);
      }
    }
    unsigned lip_count=0;
    if(!DioramaGroundCover_BuildLip(&s_ground_cover_associations,owner,b,&pressure,cap,
        ACTION_GROUND_COVER_MAX_CONTACT_TOLERANCE,s_ground_cover_cap+count,
        kGroundCoverCapVertices-count,&lip_count))return false;
    count+=lip_count;
    unsigned riser_count=0;
    if(!DioramaGroundCover_BuildClosingRisers(&s_ground_cover_associations,owner,b,
        s_ground_cover.frame,&pressure,cap,ACTION_GROUND_COVER_MAX_CONTACT_TOLERANCE,
        s_ground_cover_cap+count,kGroundCoverCapVertices-count,&riser_count))return false;
    count+=riser_count;
    for(unsigned i=0;i<count;i+=3) {
      GroundCoverFace face={0};bool visible=true;
      for(unsigned j=0;j<3;++j) {
        const DioramaGroundCoverVertex *v=&s_ground_cover_cap[i+j];
        if(!GroundCoverProject(capture,g,v->position,dedicated,&face.vertices[j].position,&face.depths[j],&face.clip_w[j])){visible=false;break;}
        /* Native lip and riser overlays share vertical planes with their
         * receivers. Give the overlay a small raster depth lead. */
        if(cap->receiver && i>=first_overlay)face.depths[j]-=.00002f;
        face.distance+=face.clip_w[j]/3;
        face.mix[j]=v->sample.crest_mix;face.alpha[j]=v->sample.alpha*layer->shade.a;
      }
      if(!visible)continue;
      const ArRenderPointF a=face.vertices[0].position,z=face.vertices[1].position,c=face.vertices[2].position;
      if((z.x-a.x)*(c.y-a.y)-(z.y-a.y)*(c.x-a.x)>=-.000001f)continue;
      if(s_ground_cover.face_count==kGroundCoverMaximumFaces)return false;
      for(unsigned j=0;j<3;++j) {
        unsigned light;if(!GroundCoverLightIndex(bank,&s_ground_cover_cap[i+j],&light))return false;
        face.lights[j]=(uint16_t)light;
      }
      s_ground_cover_faces[s_ground_cover.face_count++]=face;
      ++s_ground_cover.bank_triangles[bank];
    }
  }
  *added=s_ground_cover.face_count-*first;return true;
}
static void GroundCoverCopyFace(unsigned index,ArRenderVertex2D vertices[4],float depths[4],float clip_w[4]) {
  const GroundCoverFace *face=&s_ground_cover_faces[index];
  for(unsigned j=0;j<4;++j) {
    const unsigned source=j<3?j:2;vertices[j]=face->vertices[source];depths[j]=face->depths[source];
    if(clip_w)clip_w[j]=face->clip_w[source];
  }
}
static int GroundCoverFaceCompare(const void *a,const void *b) {
  const GroundCoverFaceOrder *x=a,*y=b;
  if(x->distance!=y->distance)return x->distance>y->distance?-1:1;
  return x->index<y->index?-1:x->index>y->index;
}
static bool GroundCoverSubmitDepth(ArRenderDevice *device,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer) {
  const unsigned count=s_ground_cover.face_count;if(!count)return true;
  if(!GroundCoverShadeBatch())return false;
  for(unsigned i=0;i<count;++i)s_ground_cover_order[i]=(GroundCoverFaceOrder){s_ground_cover_faces[i].distance,i};
  qsort(s_ground_cover_order,count,sizeof(*s_ground_cover_order),GroundCoverFaceCompare);
  for(unsigned i=0;i<count;++i) {
    const GroundCoverFace *face=&s_ground_cover_faces[s_ground_cover_order[i].index];
    memcpy(s_ground_cover_vertices+i*3,face->vertices,sizeof(face->vertices));
    memcpy(s_ground_cover_depths+i*3,face->depths,sizeof(face->depths));
    memcpy(s_ground_cover_clip_w+i*3,face->clip_w,sizeof(face->clip_w));
    for(unsigned j=0;j<3;++j)s_ground_cover_indices[i*3+j]=(int32_t)(i*3+j);
  }
  return RenderShadedDioramaDepthGeometryWithW(device,ArRenderTexture_Invalid(),s_ground_cover_vertices,
      s_ground_cover_depths,s_ground_cover_clip_w,count*3,s_ground_cover_indices,count*3,
      kArRenderBlendMode_Alpha,NULL,(ArRenderRectF){0,0,1,1},NULL,g->depth_sink,layer->description->plane,false);
}
