/* Finite source extension for generic Voxel. Geometry and receiver coordinates
 * stay in the primary capture's frame; only native material sampling changes. */
static bool VoxelGuardMatches(const DioramaCapture *capture,const ActionVoxelGeometry *voxel,
    const DioramaVoxelGuardView *guard) {
  if(!guard||!guard->capture_generation||!voxel||
      guard->source_revision!=ActionVoxel_Revision(voxel))return false;
  for(unsigned i=0;i<3;++i)if(!ArRenderTexture_IsValid(guard->textures[i]))return false;
  const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int oy=capture->camera_y-capture->authentic_y0+1;
  DioramaVoxelGuardLayout expected;
  return DioramaVoxelGuard_Resolve(ox,oy,capture->width,capture->height,
      guard->layout.source,&expected)&&!memcmp(&expected,&guard->layout,sizeof(expected));
}
static int VoxelGuardBand(int plane) {
  return plane==SR_PPU_OVERLAY_BG1?0:plane==kDioramaPlane_Bg1Hi?1:2;
}
static ArRenderTexture VoxelGuardPriority(ArRenderDevice *device,const DioramaVoxelGuardView *guard) {
  const int width=guard->layout.lower.width,height=kDioramaVoxelGuardTextureHeight;
  if(s_diorama_voxel_guard_priority_width!=width) {
    ArRenderDevice_DestroyTexture(device,s_diorama_voxel_guard_priority);
    s_diorama_voxel_guard_priority=ArRenderTexture_Invalid();
  }
  if(!ArRenderTexture_IsValid(s_diorama_voxel_guard_priority)) {
    const ArRenderTextureDesc desc={width,height*2,kArRenderPixelFormat_Argb8888,
      kArRenderTextureUsage_Target,kArRenderFilter_Nearest,kArRenderBlendMode_Alpha};
    if(!ArRenderDevice_CreateTexture(device,&desc,&s_diorama_voxel_guard_priority))return ArRenderTexture_Invalid();
    s_diorama_voxel_guard_priority_width=width;
  }
  /* Capture generations are producer-owned. The pair is rebuilt for each
   * plane visit too, so editor replacement handles cannot reuse stale ink. */
  ArRenderTargetState saved;
  if(ArRenderDevice_BeginTarget(device,s_diorama_voxel_guard_priority,&saved)!=kArRenderTargetBegin_Ready)
    return ArRenderTexture_Invalid();
  const ArRenderDrawState state={.flags=kArRenderDrawState_Blend,.blend=kArRenderBlendMode_Opaque};
  const ArRenderRectF src={0,0,width,height};bool ok=true;
  for(unsigned i=0;i<2;++i) {
    const ArRenderRectF dst={0,(float)i*height,width,height};
    ok=ArRenderDevice_DrawTextureWithState(device,guard->textures[i?1:2],&src,&dst,&state)&&ok;
  }
  ok=ArRenderDevice_EndTarget(device,&saved)&&ok;
  if(!ok)return ArRenderTexture_Invalid();
  return s_diorama_voxel_guard_priority;
}

static bool VoxelGuardMaterial(ArRenderDevice *device,const DioramaLayerDraw *layer,bool body,
    bool rear,ArRenderTexture *texture,ArRenderRectF *receiver,
    DioramaSceneryLightEffectParams *light) {
  const DioramaVoxelGuardView *guard=layer->voxel_guard;
  const int band=layer->voxel_source_plane>=0?VoxelGuardBand(layer->voxel_source_plane):
      rear?0:VoxelGuardBand(layer->description->plane);
  bool compact=false;
  *texture=guard->textures[band];
  if(body) {
    DioramaVoxelGuardFilter *cache=layer->voxel_guard_filter;
    if(!cache)return false;
    if(!cache->ready) {
      cache->priority=layer->voxel_guard_priority&&band!=0;
      if(cache->priority) {
        cache->texture=VoxelGuardPriority(device,guard);cache->compact=true;
        if(!ArRenderTexture_IsValid(cache->texture))return false;
      } else if(light->mode==1) {
        cache->texture=*texture;cache->compact=false;
      } else {
        PresentationOutcome outcome=kPresentationOutcome_Complete;
        cache->texture=BuildDioramaSupersampleFromPool(device,*texture,0,0,
            guard->layout.lower.width,kDioramaVoxelGuardTextureHeight,&s_diorama_voxel_guard_ss,&outcome);
        if(!PresentationOutcome_IsUsable(outcome))return false;
        cache->compact=ArRenderTexture_IsValid(cache->texture);
        if(!cache->compact)cache->texture=*texture;
      }
      cache->ready=true;
    }
    *texture=cache->texture;compact=cache->compact;
    if(cache->priority) {
      light->mode=2;
      light->priority=(DioramaPrioritySurfaceEffectParams){guard->layout.lower.width,
        kDioramaVoxelGuardTextureHeight,band==1,layer->blend==kArRenderBlendMode_Add};
    }
  }
  const float width=compact?guard->layout.lower.width:kDioramaVoxelGuardWidth;
  *receiver=(ArRenderRectF){0,(float)(guard->layout.primary_height-kDioramaVoxelGuardPrefixRows)/SR_PPU_SURFACE_MAX_HEIGHT,
    width/SR_PPU_SURFACE_MAX_WIDTH,(float)kDioramaVoxelGuardTextureHeight/SR_PPU_SURFACE_MAX_HEIGHT};
  light->attributes_uv=*receiver;
  const float scale=body&&compact?kDioramaSupersample:1;
  light->texel_width=1/(width*scale);
  light->texel_height=1.f/(kDioramaVoxelGuardTextureHeight*scale);
  return true;
}

/* Bounded streaming scratch; submission order follows the original triangle
 * order even on painter backends. Clipping never closes the storage seam. */
static bool RenderVoxelNativeSources(ArRenderDevice *device,const DioramaLayerDraw *layer,
    ArRenderTexture primary,const ArRenderVertex2D *vertices,const float *depths,
    const float *clip_w,int nv,const int32_t *indices,int ni,ArRenderBlendMode blend,
    const SceneryShadingField *field,ArRenderRectF source_uv,
    const DioramaSceneryLightEffectParams *material,const DioramaDepthSink *sink,
    bool body,bool rear,bool group_sources) {
  if(!layer->voxel_guard)
    return RenderShadedDioramaDepthGeometryWithW(device,primary,vertices,depths,clip_w,nv,indices,ni,
        blend,field,source_uv,material,sink,layer->description->plane,false);
  enum {capacity=768};
  ArRenderVertex2D packed[capacity];float packed_depth[capacity],packed_w[capacity];int32_t packed_indices[capacity/4*6];
  const float seam=(float)(layer->voxel_guard->layout.primary_height-kDioramaVoxelGuardPrefixRows/2)/SR_PPU_SURFACE_MAX_HEIGHT;
  float max_v=-INFINITY;
  for(int i=0;i<nv;++i)max_v=fmaxf(max_v,source_uv.y+vertices[i].tex_coord.y*source_uv.h);
  if(max_v<seam) return !ArRenderTexture_IsValid(primary)||
      RenderShadedDioramaDepthGeometryWithW(device,primary,vertices,depths,clip_w,nv,indices,ni,
        blend,field,source_uv,material,sink,layer->description->plane,false);
  /* The ordinary face/cap caller may have bound a primary-only sampling
   * effect. Release it before switching between the two source dimensions. */
  if(!sink&&!ArRenderTexture_IsValid(material->illumination)&&
      (material->mode==1||material->mode==2)&&!DioramaEffectBackend_Unbind(device))return false;
  unsigned count=0;int owner=-1;bool ok=true;
  int prologue=-1;
  if(group_sources&&body&&layer->voxel_guard_filter&&!layer->voxel_guard_filter->ready) {
    /* A target creation drains the native hybrid depth batch. Preserve the
     * original primary prefix at that boundary: opaque depth survives a
     * drain, while its ordered-alpha attachment is local to the batch. */
    for(int triangle=0;triangle<ni&&prologue<0;triangle+=3) {
      DioramaVoxelSourceVertex input[3],polygon[8];
      for(unsigned j=0;j<3;++j) {
        const int at=indices[triangle+j];if(at<0||at>=nv)return false;
        input[j]=(DioramaVoxelSourceVertex){vertices[at],depths?depths[at]:0,clip_w?clip_w[at]:1};
        input[j].vertex.tex_coord.y=source_uv.y+vertices[at].tex_coord.y*source_uv.h;
      }
      if(DioramaVoxelSource_ClipTriangle(input,seam,true,clip_w!=NULL,polygon)>=3)prologue=triangle;
    }
  }
  /* Only the caller's regular coplanar native body opts in. Its primary and
   * guard pieces have disjoint coverage, so group those pieces without
   * changing any clipped triangle after the original flush boundary. Relief,
   * caps and AA retain original order. */
  const unsigned passes=group_sources?(prologue>=0?3u:2u):1u;
  for(unsigned pass=0;pass<passes&&ok;++pass) {
    if(prologue>=0&&pass==1) {
      if(count) {
        bool bound=false;
        if(!sink&&!ArRenderTexture_IsValid(material->illumination)&&material->mode==2) {
          if(!DioramaEffectBackend_BindPrioritySurface(device,&material->priority))return false;bound=true;
        } else if(!sink&&!ArRenderTexture_IsValid(material->illumination)&&material->mode==1) {
          const DioramaDofEdgeEffectParams params={.texel_width=material->texel_width,.texel_height=material->texel_height,
            .blur_radius=material->blur_radius,.u_max=1,.v_max=1};
          if(!DioramaEffectBackend_BindDofEdge(device,&params))return false;bound=true;
        }
        ok=RenderShadedDioramaDepthGeometryWithW(device,primary,packed,packed_depth,clip_w?packed_w:NULL,
            (int)count,packed_indices,(int)(count/4*6),blend,field,source_uv,material,sink,layer->description->plane,false);
        if(bound)ok=DioramaEffectBackend_Unbind(device)&&ok;
        if(!ok)return false;
        count=0;
      }
      ArRenderTexture texture;ArRenderRectF receiver;DioramaSceneryLightEffectParams light=*material;
      if(!VoxelGuardMaterial(device,layer,body,rear,&texture,&receiver,&light))return false;
    }
    const int first=prologue>=0&&pass==1?prologue+3:0;
    const int last=prologue>=0&&!pass?prologue+3:ni;
    const unsigned selected=group_sources?(pass==passes-1):0;
  for(int triangle=first;triangle<last&&ok;triangle+=3) {
    DioramaVoxelSourceVertex input[3];
    for(unsigned j=0;j<3;++j) {
      const int at=indices[triangle+j];if(at<0||at>=nv)return false;
      input[j]=(DioramaVoxelSourceVertex){vertices[at],depths?depths[at]:0,clip_w?clip_w[at]:1};
      input[j].vertex.tex_coord.x=source_uv.x+vertices[at].tex_coord.x*source_uv.w;
      input[j].vertex.tex_coord.y=source_uv.y+vertices[at].tex_coord.y*source_uv.h;
    }
    for(unsigned guard=selected;guard<(group_sources?selected+1:2)&&ok;++guard) {
      if(!guard&&!ArRenderTexture_IsValid(primary))continue;
      DioramaVoxelSourceVertex polygon[8],scratch[8];
      unsigned n=DioramaVoxelSource_ClipTriangle(input,seam,guard,clip_w!=NULL,polygon);
      if(guard&&n) {
        const float limit=(float)(layer->voxel_guard->layout.primary_height+kDioramaVoxelGuardRows)/SR_PPU_SURFACE_MAX_HEIGHT;
        n=DioramaVoxelSource_ClipLimit(polygon,n,1,limit,false,clip_w!=NULL,scratch);
        memcpy(polygon,scratch,n*sizeof(*polygon));
      }
      if(n<3)continue;
      for(unsigned fan=1;fan+1<n&&ok;++fan) {
        if(count&&(owner!=(int)guard||count+4>capacity)) {
          ArRenderTexture texture=primary;ArRenderRectF receiver=source_uv;
          DioramaSceneryLightEffectParams light=*material;
          if(owner&&!VoxelGuardMaterial(device,layer,body,rear,&texture,&receiver,&light))return false;
          bool bound=false;
          if(!sink&&!ArRenderTexture_IsValid(light.illumination)&&light.mode==2) {
            if(!DioramaEffectBackend_BindPrioritySurface(device,&light.priority))return false;bound=true;
          } else if(!sink&&!ArRenderTexture_IsValid(light.illumination)&&light.mode==1) {
            const DioramaDofEdgeEffectParams params={.texel_width=light.texel_width,.texel_height=light.texel_height,
              .blur_radius=light.blur_radius,.u_max=1,.v_max=1};
            if(!DioramaEffectBackend_BindDofEdge(device,&params))return false;bound=true;
          }
          ok=RenderShadedDioramaDepthGeometryWithW(device,texture,packed,packed_depth,clip_w?packed_w:NULL,
              (int)count,packed_indices,(int)(count/4*6),blend,field,receiver,&light,sink,layer->description->plane,false);
          if(bound)ok=DioramaEffectBackend_Unbind(device)&&ok;
          count=0;
        }
        owner=(int)guard;
        ArRenderRectF receiver=source_uv;
        if(guard) {
          ArRenderTexture ignored;DioramaSceneryLightEffectParams light=*material;
          if(!VoxelGuardMaterial(device,layer,body,rear,&ignored,&receiver,&light))return false;
        }
        const unsigned base=count,tri[]={0,1,2,0,2,3};
        for(unsigned j=0;j<6;++j)packed_indices[base/4*6+j]=(int32_t)(base+tri[j]);
        const unsigned corners[]={0,fan,fan+1,fan+1};
        for(unsigned j=0;j<4;++j) {
          const DioramaVoxelSourceVertex *v=&polygon[corners[j]];
          packed[count]=v->vertex;
          packed[count].tex_coord.x=(v->vertex.tex_coord.x-receiver.x)/receiver.w;
          packed[count].tex_coord.y=(v->vertex.tex_coord.y-receiver.y)/receiver.h;
          packed_depth[count]=v->depth;packed_w[count]=v->clip_w;++count;
        }
      }
    }
  }
  }
  if(count&&ok) {
    ArRenderTexture texture=primary;ArRenderRectF receiver=source_uv;DioramaSceneryLightEffectParams light=*material;
    if(owner&&!VoxelGuardMaterial(device,layer,body,rear,&texture,&receiver,&light))return false;
    bool bound=false;
    if(!sink&&!ArRenderTexture_IsValid(light.illumination)&&light.mode==2) {
      if(!DioramaEffectBackend_BindPrioritySurface(device,&light.priority))return false;bound=true;
    } else if(!sink&&!ArRenderTexture_IsValid(light.illumination)&&light.mode==1) {
      const DioramaDofEdgeEffectParams params={.texel_width=light.texel_width,.texel_height=light.texel_height,
        .blur_radius=light.blur_radius,.u_max=1,.v_max=1};
      if(!DioramaEffectBackend_BindDofEdge(device,&params))return false;bound=true;
    }
    ok=RenderShadedDioramaDepthGeometryWithW(device,texture,packed,packed_depth,clip_w?packed_w:NULL,
        (int)count,packed_indices,(int)(count/4*6),blend,field,receiver,&light,sink,layer->description->plane,false);
    if(bound)ok=DioramaEffectBackend_Unbind(device)&&ok;
  }
  /* The caller may continue an already-bound native/owned DoF batch. Restore
   * its primary parameters after the guard's different texel dimensions. */
  if(!sink&&!ArRenderTexture_IsValid(material->illumination)&&material->mode==1) {
    const DioramaDofEdgeEffectParams params={.texel_width=material->texel_width,.texel_height=material->texel_height,
      .blur_radius=material->blur_radius,.u_max=1,.v_max=1};
    ok=DioramaEffectBackend_BindDofEdge(device,&params)&&ok;
  } else if(!sink&&!ArRenderTexture_IsValid(material->illumination)&&material->mode==2)
    ok=DioramaEffectBackend_BindPrioritySurface(device,&material->priority)&&ok;
  return ok;
}
