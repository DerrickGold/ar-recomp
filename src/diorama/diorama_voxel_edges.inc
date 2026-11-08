/* Solid sides split around actors. Native high-priority trim owns its own
 * shallow caps, drawn in the original BG1-high slot. */
typedef struct VoxelSide {
  ArRenderVertex2D v[4];
  float depths[4];
  float clip_w[4];
  float distance;
  bool rear_source,owned_source;
  uint16_t crisp_page; /* One-based native-front filtered source, zero unchanged. */
} VoxelSide;
static bool PrepareVoxelAuthoredAtlas(ArRenderDevice *,const DioramaCapture *,const DioramaViewGeometry *,const ActionVoxelGeometry *);
static ArRenderTexture VoxelAuthoredAtlas(void);
static ArRenderPointF VoxelOwnedArtUv(unsigned,float,float);
static bool VoxelOwnedArtReady(unsigned);
static VoxelSide s_voxel_sides[kVoxelMaxVisibleFaces];
static ArRenderVertex2D s_voxel_vertices[kVoxelMaxVisibleFaces * 4];
static float s_voxel_depths[kVoxelMaxVisibleFaces * 4];
static float s_voxel_clip_w[kVoxelMaxVisibleFaces * 4];
static int32_t s_voxel_indices[kVoxelMaxVisibleFaces * 6];
/* Faces are ordered far-to-near through a compact key array. Sorting the
 * 168-byte faces themselves moved most of their bytes on every swap. */
typedef struct VoxelSideKey { float distance; uint16_t index; } VoxelSideKey;
_Static_assert(kVoxelMaxVisibleFaces <= 65536, "face keys use 16-bit indices");
static VoxelSideKey s_voxel_keys[kVoxelMaxVisibleFaces];
static uint16_t s_voxel_order[kVoxelMaxVisibleFaces];

/* Every quad batch uses the same {0,1,2,0,2,3} pattern; build it once. */
static const int32_t *VoxelQuadIndices(void) {
  static bool ready;
  if (!ready) {
    static const int tri[] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < kVoxelMaxVisibleFaces; ++i)
      for (int j = 0; j < 6; ++j) s_voxel_indices[i * 6 + j] = i * 4 + tri[j];
    ready = true;
  }
  return s_voxel_indices;
}

/* Curve endpoints are native pixel boundaries, not necessarily cell borders.
 * Front faces and contours must include both knots before interpolation. */
static unsigned VoxelTrunkCuts(const VoxelTrunkCurve *curve,float top,float bottom,
    float world_y,bool include_rows,float *cuts) {
  unsigned count=0;cuts[count++]=top;
  if(include_rows)
    for(float y=(floorf((top+world_y)/8)+1)*8-world_y;y<bottom;y+=8)
      cuts[count++]=y;
  cuts[count++]=bottom;
  const float knots[]={curve->top_y-world_y,curve->base_y-world_y};
  for(unsigned k=0;k<2;++k) {
    const float y=knots[k];
    if(y<=top || y>=bottom)continue;
    unsigned at=0;while(at<count && cuts[at]<y)++at;
    if(at<count && cuts[at]==y)continue;
    for(unsigned i=count;i>at;--i)cuts[i]=cuts[i-1];
    cuts[at]=y;++count;
  }
  return count;
}
static int VoxelSideKeyCompare(const void *a, const void *b) {
  const VoxelSideKey *x = a, *y = b;
  if (x->distance != y->distance) return x->distance > y->distance ? -1 : 1;
  return (x->index > y->index) - (x->index < y->index);
}
/* Fills s_voxel_order with s_voxel_sides[0..count) indices, farthest first.
 * Equal distances keep build order, so the result is deterministic. */
static void SortVoxelSides(unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    s_voxel_keys[i] = (VoxelSideKey){s_voxel_sides[i].distance, (uint16_t)i};
  qsort(s_voxel_keys, count, sizeof(*s_voxel_keys), VoxelSideKeyCompare);
  for (unsigned i = 0; i < count; ++i) s_voxel_order[i] = s_voxel_keys[i].index;
}
static bool VoxelProjectWithW(const DioramaViewGeometry *g, float x, float y, float z,
                          ArRenderPointF *point, float *distance, float *depth,float *clip_w) {
  Scene3DClipPoint clip;
  if (!Scene3D_TransformToClip(g->matrix, x, y, z, &clip) ||
      clip.w <= kScene3DMinimumProjectionDepth)
    return false;
  const float inverse = 1.f / clip.w;
  const ArRenderPointF p = {(clip.x * inverse * .5f + .5f) * g->width,
                            (1.f - (clip.y * inverse * .5f + .5f)) * g->height};
  if (!isfinite(p.x) || !isfinite(p.y)) return false;
  *point = p;
  if(clip_w)*clip_w=clip.w;
  if(g->depth_sink || (g->options && g->options->voxel_observe_face)) {
    *depth=clip.z*inverse*.5f+.5f;
    if(!isfinite(*depth))return false;
  }
  *distance += clip.w * .25f;
  return true;
}
static bool VoxelProject(const DioramaViewGeometry *g, float x, float y, float z,
                          ArRenderPointF *point, float *distance, float *depth) {
  return VoxelProjectWithW(g,x,y,z,point,distance,depth,NULL);
}
static void ObserveVoxelFace(const DioramaViewGeometry *g,const DioramaLayerDraw *layer,
    const VoxelSide *face,const float world_xy[8],int kind,const VoxelEdge *source) {
  if(!g->options->voxel_observe_face)return;
  DioramaVoxelPickFace pick={.kind=kind,.priority=layer->description->plane==kDioramaPlane_Bg1Hi,
      .plane=layer->description->plane,.cell_x=-1,.cell_y=-1};
  if(source){pick.art_source_x=source->top_sample_x;pick.art_source_y=source->top_sample_y;
    pick.art_source_size=source->top_sample_size;}
  for(unsigned i=0;i<4;++i) {
    pick.screen_xy[i*2]=face->v[i].position.x;pick.screen_xy[i*2+1]=face->v[i].position.y;
    pick.world_xy[i*2]=world_xy[i*2];pick.world_xy[i*2+1]=world_xy[i*2+1];
    pick.depth[i]=face->depths[i];pick.clip_w[i]=face->clip_w[i];
  }
  g->options->voxel_observe_face(g->options->voxel_observer_userdata,&pick);
}
/* A single connected rear mesh reuses the ordinary wall texture, lighting and
 * DoF pass. Fixed world grid lines keep its relief stable during scrolling;
 * only the two capture boundaries move. Capacity is shared with the trunk. */
static void PrepareVoxelRearMesh(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer,DioramaLayerMesh *mesh) {
  *mesh=(DioramaLayerMesh){.constrained=true,.has_depth=layer->depth_sink!=NULL};
  const float ox=capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->camera_y-capture->authentic_y0+1;
  const float x0=ox+g->u0*SR_PPU_SURFACE_MAX_WIDTH,x1=ox+g->u1*SR_PPU_SURFACE_MAX_WIDTH;
  const float y0=oy+g->v0*SR_PPU_SURFACE_MAX_HEIGHT,y1=oy+g->v1*SR_PPU_SURFACE_MAX_HEIGHT;
  if(x1<=x0 || y1<=y0)return;
  float step=16;
  for(;;step*=2) {
    const int nx=(int)ceilf(x1/step)-(int)floorf(x0/step)+1;
    const int ny=(int)ceilf(y1/step)-(int)floorf(y0/step)+1;
    if(nx*ny<=DIORAMA_VERTS_PER_LAYER && (nx-1)*(ny-1)*6<=DIORAMA_INDICES_PER_LAYER)break;
  }
  float xs[DIORAMA_VERTS_PER_LAYER],ys[DIORAMA_VERTS_PER_LAYER];
  float observed_w[DIORAMA_VERTS_PER_LAYER];
  const bool observe=g->options->voxel_observe_face!=NULL;
  int nx=0,ny=0;xs[nx++]=x0;ys[ny++]=y0;
  for(float x=floorf(x0/step)*step+step;x<x1;x+=step)xs[nx++]=x;
  for(float y=floorf(y0/step)*step+step;y<y1;y+=step)ys[ny++]=y;
  xs[nx++]=x1;ys[ny++]=y1;
  for(int row=0;row<ny;++row)for(int col=0;col<nx;++col) {
    const float depth=layer->voxel_depth+ActionVoxel_RearRelief(layer->voxel_geometry,xs[col],ys[row]);
    const float scale=1+depth/fmaxf(1,g->camera.distance);
    const float s=(xs[col]-x0)/(x1-x0),t=(ys[row]-y0)/(y1-y0);
    const int index=mesh->vertex_count++;
    ArRenderVertex2D *v=&mesh->vertices[index];
    if(!ProjectWorldPointDepth(g->matrix,(s-.5f)*g->aspect_x*scale,(.5f-t)*g->height_scale*scale,
        -depth,g->width,g->height,&v->position,(mesh->has_depth||observe)?mesh->depths+index:NULL)) {mesh->vertex_count=0;return;}
    if(observe) {
      Scene3DClipPoint clip;
      if(!Scene3D_TransformToClip(g->matrix,(s-.5f)*g->aspect_x*scale,
          (.5f-t)*g->height_scale*scale,-depth,&clip)){mesh->vertex_count=0;return;}
      observed_w[index]=clip.w;
    }
    v->tex_coord=(ArRenderPointF){(xs[col]-ox)/SR_PPU_SURFACE_MAX_WIDTH,(ys[row]-oy)/SR_PPU_SURFACE_MAX_HEIGHT};
    v->color=layer->shade;
  }
  TriangulateGrid(nx-1,ny-1,mesh->indices,&mesh->index_count);
  ShadeDioramaVertices(layer,mesh->vertices,mesh->vertex_count);
  if(observe)for(int i=0;i<mesh->index_count;i+=3) {
    VoxelSide face={0};float world_xy[8];
    for(unsigned j=0;j<4;++j) {
      const int at=mesh->indices[i+(j<3?j:2)];face.v[j]=mesh->vertices[at];
      face.depths[j]=mesh->depths[at];face.clip_w[j]=observed_w[at];
      world_xy[j*2]=face.v[j].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH+ox;
      world_xy[j*2+1]=face.v[j].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT+oy;
    }
    ObserveVoxelFace(g,layer,&face,world_xy,kDioramaVoxelFace_Backing,NULL);
  }
}
static bool SubmitVoxelSides(ArRenderDevice *device, const DioramaViewGeometry *g,
                             const DioramaLayerDraw *layer, int count, float blur);
/* A full face buffer submits its batch and continues instead of failing the
 * layer. Distance order holds within each batch; only an overflowing view
 * (already a failure before) splits across batches. */
static bool ReserveVoxelSide(ArRenderDevice *device, const DioramaViewGeometry *g,
                             const DioramaLayerDraw *layer, int *count, float blur) {
  if (*count < kVoxelMaxVisibleFaces) return true;
  if (!SubmitVoxelSides(device, g, layer, *count, blur)) return false;
  *count = 0;
  return true;
}
static const VoxelAuthoredCell *NativeTransformCell(const ActionVoxelGeometry *geometry,int x,int y) {
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(geometry,(int)floorf(x/16.f),(int)floorf(y/16.f));
  return a && a->native_transform?a:NULL;
}
/* Clip in final physical depth, then return the original material parameter
 * for UVs/folds. Native decorative priority remains wholly behind actors. */
static bool TransformedNativeEdgeSpan(const ActionVoxelGeometry *geometry,const VoxelEdge *edge,
    const VoxelAuthoredCell *a,VoxelEdgePass pass,float far_depth,
    float y,VoxelEdgeSpan *span) {
  VoxelEdge raw=*edge;raw.high_priority=false;raw.colliding=false;
  if(!ActionVoxel_ResolveEdgeSpanAtY(geometry,&raw,kVoxelEdgeNative,-INFINITY,INFINITY,y,span))return false;
  const float depth=ActionVoxel_Depth(geometry),split=fmaxf(0,ActionVoxel_PlayRecess(geometry)-.01f);
  const bool deck=edge->high_priority&&edge->colliding&&edge->side==0&&edge->far_rank>=2;
  const bool behind=!edge->high_priority&&(!edge->colliding||edge->far_rank==1);
  const bool front_phase=far_depth<depth-.000001f;
  if(behind && front_phase)return false;
  float lo=-INFINITY,hi=INFINITY;
  if(deck) {if(pass==kVoxelEdgeHighDeckBehind)lo=split;else hi=split;}
  else if(!edge->high_priority && !behind) {if(front_phase)hi=split;else lo=split;}
  const DioramaVoxelTransform *t=&a->transform;
  lo=t->pz+(lo-t->pz-t->dz)/t->sz;hi=t->pz+(hi-t->pz-t->dz)/t->sz;
  const VoxelTrunkCurve *curve=ActionVoxel_TrunkCurveForEdge(geometry,edge);
  if(curve) {
    const float front=ActionVoxel_TrunkFrontDepth(geometry,curve,y),back=ActionVoxel_TrunkBackDepth(geometry,curve,y);
    const float trim=ActionVoxel_TrimDepth(geometry),recess=ActionVoxel_SceneryRecess(geometry);
    if(back>front+.000001f){lo=recess+(lo-front)*trim/(back-front);hi=recess+(hi-front)*trim/(back-front);}
  }
  span->near_depth=fmaxf(span->near_depth,lo);span->far_depth=fminf(span->far_depth,hi);
  return span->far_depth>span->near_depth;
}
static bool NativeBranchHasTransform(const ActionVoxelGeometry *geometry,const VoxelPatch *p) {
  if(!ActionVoxel_HasNativeTransforms(geometry))return false;
  for(int y=p->y/16;y<(p->y+p->height+15)/16;++y)
    for(int x=p->x/16;x<(p->x+p->width+15)/16;++x)
      if(NativeTransformCell(geometry,x*16,y*16))return true;
  return false;
}
static bool DrawTransformedNativeBranch(ArRenderDevice *device,const DioramaCapture *capture,
    const DioramaViewGeometry *g,const DioramaLayerDraw *layer,const VoxelPatch *p,int *count,float blur) {
  const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int oy=capture->camera_y-capture->authentic_y0+1;
  const float depth=ActionVoxel_BranchBackDepth(layer->voxel_geometry);
  for(int cy=(int)floorf(p->y/16.f);cy<(int)ceilf((p->y+p->height)/16.f);++cy)
    for(int cx=(int)floorf(p->x/16.f);cx<(int)ceilf((p->x+p->width)/16.f);++cx) {
      const VoxelAuthoredCell *a=NativeTransformCell(layer->voxel_geometry,cx*16,cy*16);
      if(a && (!(a->faces&kDioramaVoxelFaceMask_Back)||!VoxelOwnedArtReady(a->art_id[0])))continue;
      float left=fmaxf(p->x,cx*16),right=fminf(p->x+p->width,(cx+1)*16);
      float top=fmaxf(p->y,cy*16),bottom=fminf(p->y+p->height,(cy+1)*16);
      if(!a){left=fmaxf(left,ox);right=fminf(right,ox+capture->width);top=fmaxf(top,oy);bottom=fminf(bottom,oy+capture->height);}
      if(right<=left||bottom<=top)continue;
      const float xs[]={left,right,right,left},ys[]={top,top,bottom,bottom};
      VoxelSide face={.owned_source=a!=NULL};float observer_xy[8];bool valid=true;
      for(unsigned j=0;j<4;++j) {
        float x=xs[j],y=ys[j],z=depth;
        if(a)ActionVoxel_TransformPoint(&a->transform,x,y,z,&x,&y,&z);
        const float u=(x-ox)/SR_PPU_SURFACE_MAX_WIDTH,v=(y-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
        const float scale=1+z/fmaxf(1,g->camera.distance);
        valid&=VoxelProjectWithW(g,((u-g->u0)/(g->u1-g->u0)-.5f)*g->aspect_x*scale,
            (.5f-(v-g->v0)/(g->v1-g->v0))*g->height_scale*scale,-z,
            &face.v[j].position,&face.distance,&face.depths[j],&face.clip_w[j]);
        const float su=(xs[j]-ox)/SR_PPU_SURFACE_MAX_WIDTH,sv=(ys[j]-oy)/SR_PPU_SURFACE_MAX_HEIGHT;
        face.v[j].tex_coord=a?VoxelOwnedArtUv(a->art_id[0],fmaxf(.0001f,fminf(15.9999f,xs[j]-a->x)),
            fmaxf(.0001f,fminf(15.9999f,ys[j]-a->y))):(ArRenderPointF){su,sv};
        float shade=.90f*(1-.12f*depth/layer->voxel_depth)*(1-SceneryDimming_Amount(layer->dimming,layer->dimming_ramp,su,sv));
        if(layer->exposure&&layer->exposure->sample)shade*=layer->exposure->sample(layer->exposure->userdata,su,sv,su,sv);
        face.v[j].color=(ArRenderColorF){layer->shade.r*shade,layer->shade.g*shade,layer->shade.b*shade,1};
        observer_xy[j*2]=xs[j];observer_xy[j*2+1]=ys[j];
      }
      if(!valid)continue;
      if(a && g->options->voxel_observe_face) {
        DioramaVoxelPickFace pick={.authored=true,.kind=kDioramaVoxelFace_Backing,
            .priority=layer->description->plane==kDioramaPlane_Bg1Hi,.plane=layer->description->plane,
            .cell_x=a->x/16,.cell_y=a->y/16,.art_index=0};
        for(unsigned j=0;j<4;++j) {
          pick.screen_xy[j*2]=face.v[j].position.x;pick.screen_xy[j*2+1]=face.v[j].position.y;
          pick.world_xy[j*2]=observer_xy[j*2];pick.world_xy[j*2+1]=observer_xy[j*2+1];
          pick.art_uv[j*2]=xs[j]-a->x;pick.art_uv[j*2+1]=ys[j]-a->y;
          pick.depth[j]=face.depths[j];pick.clip_w[j]=face.clip_w[j];
        }
        g->options->voxel_observe_face(g->options->voxel_observer_userdata,&pick);
      } else ObserveVoxelFace(g,layer,&face,observer_xy,kDioramaVoxelFace_Backing,NULL);
      if(!ReserveVoxelSide(device,g,layer,count,blur))return false;s_voxel_sides[(*count)++]=face;
    }
  return true;
}
static bool DrawVoxelGeometry(ArRenderDevice *device, const DioramaCapture *capture,
                               const DioramaViewGeometry *g, const DioramaLayerDraw *layer,
                               float near_depth, float far_depth, VoxelEdgePass pass) {
  if (far_depth <= near_depth) return true;
  const ActionVoxelGeometry *castle = layer->voxel_geometry;
  const bool native_transforms=ActionVoxel_HasNativeTransforms(castle);
  unsigned total;
  const VoxelEdge *edges = ActionVoxel_Edges(castle, &total);
  unsigned owned_caps;ActionVoxel_AuthoredCapArt(castle,&owned_caps);
  if((owned_caps||native_transforms) && !PrepareVoxelAuthoredAtlas(device,capture,g,castle))return false;
  const int ox = capture->bg1_camera_x - (capture->width - 256) / 2 - capture->obj_apron;
  const int oy = capture->camera_y - capture->authentic_y0 + 1;
  const float du = g->u1 - g->u0, dv = g->v1 - g->v0, depth = layer->voxel_depth;
  const bool rear = pass == kVoxelEdgeRearTrim;
  const bool rear_grass = layer->description->plane == SR_PPU_OVERLAY_BG1;
  const bool high_priority =
      pass != kVoxelEdgeNative || layer->description->plane == kDioramaPlane_Bg1Hi;
  const float rear_offset = rear ? ActionVoxel_RearFaceDepth(layer->voxel_geometry) : 0;
  const float blur =
      (rear || rear_grass) && DofBlurEnabled(device, g->options)
          ? DofRadiusForLayer(.5f - (rear_grass ? depth : rear_offset)) : 0;
  int count = 0;
  /* The iron gate is a two-sided grating, not an extrusion of opaque pixels.
   * Repeat its native transparent strip across the doorway and join the rear
   * jamb. Keep the native priority slot, including the portion ahead of actors. */
  if (!rear_grass && pass == kVoxelEdgeNative && far_depth == depth) {
    unsigned n;
    const VoxelGate *gates = ActionVoxel_Gates(layer->voxel_geometry, &n);
    for (unsigned i = 0; i < n; ++i) {
      const VoxelGate p = gates[i];
      const int width=p.width?p.width:8;
      if (p.high_priority != high_priority || p.x < ox || p.x + width > ox + capture->width) continue;
      const float y0 = fmaxf(0, p.y - oy), y1 = fminf(capture->height, p.y + p.height - oy);
      if (y1 <= y0) continue;
      const float front_x = p.x - ox + (p.centered?width*.5f:(p.to_right ? 0 : width)),
                  back_x = p.x - ox + (p.centered?width*.5f:(p.to_right ? 8 : 0));
      for (unsigned slice = 0; slice < kVoxelGateDepthSlices; ++slice) {
        VoxelSide face = {0};
        bool ok = true;
        for (unsigned j = 0; j < 4; ++j) {
          const bool back = j == 1 || j == 2, bottom = j >= 2;
          const float t = (slice + (back ? 1.f : 0.f)) / kVoxelGateDepthSlices, z = t * depth;
          const float x = front_x + (back_x - front_x) * t, y = bottom ? y1 : y0;
          const float u = x / SR_PPU_SURFACE_MAX_WIDTH, v = y / SR_PPU_SURFACE_MAX_HEIGHT;
          const float scale = 1 + z / fmaxf(1, g->camera.distance);
          const float wx = ((u - g->u0) / du - .5f) * g->aspect_x * scale;
          const float wy = (.5f - (v - g->v0) / dv) * g->height_scale * scale;
          ok = ok && VoxelProjectWithW(g, wx, wy, -z, &face.v[j].position, &face.distance, &face.depths[j],&face.clip_w[j]);
          if (!ok) break;
          /* Half-texel insets keep adjacent marble/floor out of filtered bars. */
          const float su = (p.x - ox + (back ? width-.5f : .5f)) / SR_PPU_SURFACE_MAX_WIDTH;
          const float sv = (bottom ? y1 - .5f : y0 + .5f) / SR_PPU_SURFACE_MAX_HEIGHT;
          face.v[j].tex_coord = (ArRenderPointF){su, sv};
          float shade = (1 - .22f * t) *
                        (1 - SceneryDimming_Amount(layer->dimming, layer->dimming_ramp, su, sv));
          if (layer->exposure && layer->exposure->sample)
            shade *= layer->exposure->sample(layer->exposure->userdata, su, sv, su, sv);
          face.v[j].color = (ArRenderColorF){layer->shade.r * shade, layer->shade.g * shade,
                                             layer->shade.b * shade, 1};
        }
        if (!ok) continue;
        if (!ReserveVoxelSide(device, g, layer, &count, blur)) return false;
        s_voxel_sides[count++] = face;
      }
    }
  }
  if (pass == kVoxelEdgeNative || pass == kVoxelEdgeHighDeckBehind) {
    unsigned n;
    /* Crossed native profiles give hazards depth without opaque side walls.
     * Branches keep a rear cap at their own shallow depth, not the ground's. */
    const VoxelPatch *patches = ActionVoxel_Patches(layer->voxel_geometry, &n);
    for (unsigned i = 0; i < n; ++i) {
      const VoxelPatch p = patches[i];
      if ((p.kind == kVoxelPatch_RearGrass) != rear_grass) continue;
      if (p.high_priority != high_priority) continue;
      if (p.kind == kVoxelPatch_Spikes || p.kind == kVoxelPatch_Thorns || rear_grass) {
        if (pass != kVoxelEdgeNative || far_depth != depth) continue;
        const float y0 = fmaxf(0,p.y-oy), y1 = fminf(capture->height,p.y+p.height-oy);
        if (y1 <= y0) continue;
        /* Preserve the full 32px vine profile; spikes have a 16px repeat.
         * Hazards fill the path; reviewed standalone grass occupies only a
         * narrow rear band. Native silhouettes and collision stay intact. */
        const bool thorns = p.kind == kVoxelPatch_Thorns;
        const int repeat = thorns ? 32 : 16;
        const unsigned rows = rear_grass ? 1 : 3;
        const float back = depth, front = rear_grass ? depth*.84f : 0;
        const float row_depth = (back-front)/rows;
        const float pixel_depth = g->aspect_x / (du * SR_PPU_SURFACE_MAX_WIDTH);
        for (int column = 0; column < p.width; column += repeat) {
          const float width = fminf(repeat,p.width-column), x0 = p.x+column-ox;
          if (x0 < 0 || x0+width > capture->width) continue;
          const float half_x = width * (thorns || rear_grass ? .5f : .35355339f);
          const float half_z = thorns || rear_grass ? row_depth*.5f : fminf(depth/6,half_x*pixel_depth);
          for (unsigned row = 0; row < rows; ++row) for (unsigned diagonal = 0; diagonal < 2; ++diagonal) {
            VoxelSide face = {0};bool ok = true;
            for (unsigned j = 0; j < 4; ++j) {
              const bool end = j == 1 || j == 2, bottom = j >= 2;
              const float z = (rear_grass ? front+row_depth*(row+.5f) : depth*(row+.5f)/3) +
                  (end != (diagonal != 0) ? half_z : -half_z);
              const float x = x0+width*.5f+(end?half_x:-half_x), y = bottom?y1:y0;
              const float u = x/SR_PPU_SURFACE_MAX_WIDTH, v = y/SR_PPU_SURFACE_MAX_HEIGHT;
              const float scale = 1+z/fmaxf(1,g->camera.distance);
              ok = VoxelProjectWithW(g,((u-g->u0)/du-.5f)*g->aspect_x*scale,
                  (.5f-(v-g->v0)/dv)*g->height_scale*scale,-z,&face.v[j].position, &face.distance, &face.depths[j],&face.clip_w[j]);
              if (!ok) break;
              const float su = (x0+(end?width-.5f:.5f))/SR_PPU_SURFACE_MAX_WIDTH;
              const float sv = (bottom?y1-.5f:y0+.5f)/SR_PPU_SURFACE_MAX_HEIGHT;
              face.v[j].tex_coord=(ArRenderPointF){su,sv};
              float shade=(1-.12f*z/depth)*(1-SceneryDimming_Amount(layer->dimming,layer->dimming_ramp,su,sv));
              if(layer->exposure && layer->exposure->sample)
                shade*=layer->exposure->sample(layer->exposure->userdata,su,sv,su,sv);
              face.v[j].color=(ArRenderColorF){layer->shade.r*shade,layer->shade.g*shade,layer->shade.b*shade,1};
            }
            if (!ok) continue;
            if (!ReserveVoxelSide(device, g, layer, &count, blur)) return false;
            s_voxel_sides[count++]=face;
          }
        }
        continue;
      }
      const bool branch = p.kind == kVoxelPatch_BranchRear;
      const bool walls = p.kind == kVoxelPatch_SideProfile;
      if (walls && ActionVoxel_Profile(layer->voxel_geometry) == kActionVoxelProfile_FillmoreForest) continue;
      if (pass == kVoxelEdgeHighDeckBehind ? !branch : branch && high_priority) continue;
      if (!walls && far_depth != depth) continue;
      if(branch && NativeBranchHasTransform(castle,&p)) {
        if(!DrawTransformedNativeBranch(device,capture,g,layer,&p,&count,blur))return false;
        continue;
      }
      if (p.x < ox || p.x+p.width > ox+capture->width) continue;
      const float x0 = p.x - ox, x1 = x0 + p.width;
      const float y0 = fmaxf(0,p.y - oy), y1 = fminf(capture->height,p.y + p.height - oy);
      if (y1 <= y0) continue;
      const float patch_depth = branch ? ActionVoxel_BranchBackDepth(layer->voxel_geometry) : depth;
      for (unsigned side = walls ? 1u : 0u; side < (branch ? 1u : 3u); ++side) {
        VoxelSide face = {0};
        bool ok = true;
        for (unsigned j = 0; j < 4; ++j) {
          const bool end = j == 1 || j == 2, bottom = j >= 2;
          const float z = walls ? (end ? far_depth : near_depth) : !side || end ? patch_depth : 0;
          const float x = side ? (side == 1 ? x0 : x1) : (end ? x1 : x0), y = bottom ? y1 : y0;
          const float u = x / SR_PPU_SURFACE_MAX_WIDTH, v = y / SR_PPU_SURFACE_MAX_HEIGHT;
          const float scale = 1 + z / fmaxf(1,g->camera.distance);
          const float wx = ((u-g->u0)/du-.5f)*g->aspect_x*scale;
          const float wy = (.5f-(v-g->v0)/dv)*g->height_scale*scale;
          ok = ok && VoxelProjectWithW(g,wx,wy,-z,&face.v[j].position, &face.distance, &face.depths[j],&face.clip_w[j]);
          if (!ok) break;
          const float t = walls ? z/depth : end ? 1.f : 0.f;
          const float su = (x0+.5f+(p.width-1)*(side == 2 ? 1-t : t)) / SR_PPU_SURFACE_MAX_WIDTH;
          const float sv = (bottom ? y1-.5f : y0+.5f) / SR_PPU_SURFACE_MAX_HEIGHT;
          face.v[j].tex_coord = (ArRenderPointF){su,sv};
          float shade = (side ? .86f : .90f) * (1-.12f*z/depth) *
              (1-SceneryDimming_Amount(layer->dimming,layer->dimming_ramp,su,sv));
          if (layer->exposure && layer->exposure->sample)
            shade *= layer->exposure->sample(layer->exposure->userdata,su,sv,su,sv);
          face.v[j].color = (ArRenderColorF){layer->shade.r*shade,layer->shade.g*shade,
                                             layer->shade.b*shade,1};
        }
        if (!ok) continue;
        if (!ReserveVoxelSide(device, g, layer, &count, blur)) return false;
        s_voxel_sides[count++] = face;
      }
    }
  }
  if (rear) {
    unsigned n;
    const VoxelRearTrim *rear_trims = ActionVoxel_RearTrims(layer->voxel_geometry, &n);
    for (unsigned i = 0; i < n; ++i) {
      const VoxelRearTrim p = rear_trims[i];
      const float x0 = fmaxf(0, p.x - ox), y0 = fmaxf(0, p.y - oy);
      const float x1 = fminf(capture->width, p.x + p.width - ox),
                  y1 = fminf(capture->height, p.y + 16 - oy);
      if (x1 <= x0 || y1 <= y0) continue;
      VoxelSide face = {0};
      bool ok = true;
      const float xx[] = {x0, x1, x1, x0}, yy[] = {y0, y0, y1, y1};
      for (unsigned j = 0; j < 4; ++j) {
        const float u = xx[j] / SR_PPU_SURFACE_MAX_WIDTH, v = yy[j] / SR_PPU_SURFACE_MAX_HEIGHT;
        const float scale = 1 + rear_offset / fmaxf(1, g->camera.distance);
        const float wx = ((u - g->u0) / du - .5f) * g->aspect_x * scale;
        const float wy = (.5f - (v - g->v0) / dv) * g->height_scale * scale;
        ok = ok && VoxelProjectWithW(g, wx, wy, -rear_offset, &face.v[j].position, &face.distance, &face.depths[j],&face.clip_w[j]);
        if (!ok) break;
        face.v[j].tex_coord = (ArRenderPointF){u, v};
        float shade = .88f * (1 - SceneryDimming_Amount(layer->dimming, layer->dimming_ramp, u, v));
        if (layer->exposure && layer->exposure->sample)
          shade *= layer->exposure->sample(layer->exposure->userdata, u, v, u, v);
        face.v[j].color = (ArRenderColorF){layer->shade.r * shade, layer->shade.g * shade,
                                           layer->shade.b * shade, 1};
      }
      if (!ok) continue;
      if (!ReserveVoxelSide(device, g, layer, &count, blur)) return false;
      s_voxel_sides[count++] = face;
    }
  }
  const uint16_t *indices = NULL;
  total = 0;
  if (!rear_grass) indices = ActionVoxel_EdgesForPass(castle, pass, high_priority, &total);
  for (unsigned i = 0; i < total; ++i) {
    const VoxelEdge e = edges[indices[i]];
    if (e.high_priority != high_priority) continue;
    const VoxelAuthoredCell *transformed=native_transforms?NativeTransformCell(castle,e.x,e.y):NULL;
    const unsigned face_mask[]={kDioramaVoxelFaceMask_Top,kDioramaVoxelFaceMask_Right,kDioramaVoxelFaceMask_Bottom,kDioramaVoxelFaceMask_Left};
    if(transformed && !(transformed->faces&face_mask[e.side]))continue;
    float x0 = e.x - ox, y0 = e.y - oy, x1 = x0, y1 = y0;
    if (e.side & 1) {
      if (e.side == 1) x0 = x1 = x0 + 1;
      y1 += e.length;
    } else {
      if (e.side == 2) y0 = y1 = y0 + 1;
      x1 += e.length;
    }
    if (!transformed && (fmaxf(x0, x1) < 0 || fminf(x0, x1) > capture->width || fmaxf(y0, y1) < 0 ||
        fminf(y0, y1) > capture->height)
      )continue;
    if(transformed) {
      float ax,ay,az,bx,by,bz;
      ActionVoxel_TransformPoint(&transformed->transform,fminf(x0,x1)+ox,
          fminf(y0,y1)+oy-fmaxf(e.rear_fold_top,e.rear_fold_bottom),0,&ax,&ay,&az);
      ActionVoxel_TransformPoint(&transformed->transform,fmaxf(x0,x1)+ox,fmaxf(y0,y1)+oy,
          depth+ActionVoxel_TrimDepth(castle),&bx,&by,&bz);
      const float pixels=2*fmaxf(du*SR_PPU_SURFACE_MAX_WIDTH/fmaxf(.01f,g->aspect_x),
          dv*SR_PPU_SURFACE_MAX_HEIGHT/fmaxf(.01f,g->height_scale));
      const float guard=fmaxf(fabsf(az),fabsf(bz))*pixels+16;
      if(bx+guard<ox || ax-guard>ox+capture->width || by+guard<oy || ay-guard>oy+capture->height)continue;
    }
    VoxelEdgeSpan span;
    if(transformed) {
      bool any=false;
      for(unsigned sample=0;sample<3;++sample) {
        VoxelEdgeSpan part;const float y=e.y+(e.side==2)+((e.side&1)?e.length*sample*.5f:0);
        if(!TransformedNativeEdgeSpan(castle,&e,transformed,pass,far_depth,y,&part))continue;
        if(!any)span=part;else {span.near_depth=fminf(span.near_depth,part.near_depth);span.far_depth=fmaxf(span.far_depth,part.far_depth);}any=true;
      }
      if(!any)continue;
    } else if (!ActionVoxel_ResolveEdgeSpan(castle, &e, pass, near_depth, far_depth, &span)) continue;
    /* Stay inside the captured texture; neighbouring views rebuild only projection. */
    if(!transformed) {
      x0 = fmaxf(0, fminf(capture->width, x0));
      x1 = fmaxf(0, fminf(capture->width, x1));
      y0 = fmaxf(0, fminf(capture->height, y0));
      y1 = fmaxf(0, fminf(capture->height, y1));
    }
    if (e.side == 2 || e.side == 3) {
      float t = x0;
      x0 = x1;
      x1 = t;
      t = y0;
      y0 = y1;
      y1 = t;
    }
    /* Reviewed profile caps repeat an interior native square instead of
     * stretching the curved rim across the top. Anchor the repeats in world
     * pixels and depth, so clipping and actor-band splits share the same UVs.
     * A cropped-out source retains the original cap sampling. */
    const float sample_x = e.top_sample_x - ox, sample_y = e.top_sample_y - oy;
    const bool sampled_top = e.side == 0 && e.top_sample_size > 1 && (e.top_owned_art ||
        (sample_x >= 0 && sample_y >= 0 &&
        sample_x + e.top_sample_size <= capture->width &&
        sample_y + e.top_sample_size <= capture->height));
    const float tile_width = sampled_top ? e.top_sample_size : 1;
    const float tile_depth = sampled_top ? tile_width * g->aspect_x /
        (du * SR_PPU_SURFACE_MAX_WIDTH) : 1;
    const float clipped_x0 = x0, clipped_x1 = x1;
    const VoxelTrunkCurve *trunk_curve = ActionVoxel_TrunkCurveForEdge(castle, &e);
    const VoxelTrunkCurve *column_curve = ActionVoxel_ColumnCurveForEdge(castle, &e);
    VoxelEdge neighbor=e;neighbor.column_curve=e.neighbor_column_curve;
    const VoxelTrunkCurve *neighbor_curve = ActionVoxel_ColumnCurveForEdge(castle, &neighbor);
    const bool column_side = column_curve || neighbor_curve;
    const VoxelTrunkCurve *cut_curve=trunk_curve?trunk_curve:column_curve?column_curve:neighbor_curve;
    const bool curved_side = cut_curve && (e.side & 1);
    const float clipped_y0 = y0, clipped_y1 = y1;
    float ycuts[SR_PPU_SURFACE_MAX_HEIGHT/8+4];
    const unsigned ycount = curved_side ? VoxelTrunkCuts(cut_curve,
        fminf(y0,y1),fmaxf(y0,y1),oy,true,ycuts) : 2;
    const int first_x = sampled_top ? (int)floorf((x0 + ox) / tile_width) : 0;
    const int last_x = sampled_top ? (int)ceilf((x1 + ox) / tile_width) : 1;
    const int first_z = sampled_top ? (int)floorf(span.near_depth / tile_depth) : 0;
    const int last_z = sampled_top ? (int)ceilf(span.far_depth / tile_depth) : 1;
    /* Root contours share the front mesh's fixed world rows. A clipped edge
     * samples the same curve as its neighbouring face, including both ends. */
    for (unsigned iy = 0; iy + 1 < ycount; ++iy)
    for (int iz = first_z; iz < last_z; ++iz)
    for (int ix = first_x; ix < last_x; ++ix) {
    const float tile_x0 = ix * tile_width - ox, tile_z0 = iz * tile_depth;
    x0 = sampled_top ? fmaxf(clipped_x0, tile_x0) : clipped_x0;
    x1 = sampled_top ? fminf(clipped_x1, tile_x0 + tile_width) : clipped_x1;
    const float band_top = curved_side ? ycuts[iy] : 0;
    const float band_bottom = curved_side ? ycuts[iy+1] : 0;
    y0 = curved_side ? (clipped_y0<clipped_y1?band_top:band_bottom) : clipped_y0;
    y1 = curved_side ? (clipped_y0<clipped_y1?band_bottom:band_top) : clipped_y1;
    const float face_near = sampled_top ? fmaxf(span.near_depth, tile_z0) : span.near_depth;
    const float face_far = sampled_top ? fminf(span.far_depth, tile_z0 + tile_depth) : span.far_depth;
    if (sampled_top && (x1 <= x0 || face_far <= face_near)) continue;
    const bool owned_top=sampled_top && e.top_owned_art;
    VoxelSide next = {.owned_source=owned_top || transformed};float observer_xy[8];
    const float contact = fmaxf(0, layer->voxel_recess - .01f);
    const bool folded_top = e.side == 0 && e.rear_fold_top && face_far > contact;
    next.rear_source = folded_top && !e.textured_top && face_near >= contact - .00001f;
    bool ok = true;
    for (unsigned j = 0; j < 4; ++j) {
      const bool end = j == 1 || j == 2, back = j >= 2;
      float local_z = back ? face_far : face_near;
      const float x = end ? x1 : x0, native_y = end ? y1 : y0;
      observer_xy[j*2]=x+ox+(e.side==1?-.01f:e.side==3?.01f:0);
      observer_xy[j*2+1]=native_y+oy+(e.side==2?-.01f:e.side==0?.01f:0);
      if(column_side || transformed) {
        /* Clip the actual taper interval against the native actor band. A
         * covered endpoint collapses to one depth; the opposite triangle can
         * still close an emerging side at the cap or a neighbouring shaft. */
        VoxelEdgeSpan actual={face_near,face_near};
        const bool exists=transformed?TransformedNativeEdgeSpan(castle,&e,transformed,pass,far_depth,native_y+oy,&actual):
            ActionVoxel_ResolveEdgeSpanAtY(castle,&e,pass,near_depth,far_depth,native_y+oy,&actual);
        if(!exists)
          actual.near_depth=actual.far_depth=fminf(face_far,fmaxf(face_near,actual.near_depth));
        else if(transformed) {
          actual.near_depth=fmaxf(face_near,fminf(face_far,actual.near_depth));
          actual.far_depth=fmaxf(face_near,fminf(face_far,actual.far_depth));
        }
        local_z=back?actual.far_depth:actual.near_depth;
      }
      float z = rear_offset + local_z;
      if (trunk_curve) {
        const float front = ActionVoxel_TrunkFrontDepth(castle,trunk_curve,native_y+oy);
        const float back_depth = ActionVoxel_TrunkBackDepth(castle,trunk_curve,native_y+oy);
        const float fraction = fmaxf(0,fminf(1,(local_z-ActionVoxel_SceneryRecess(castle))/
            fmaxf(.00001f,ActionVoxel_TrimDepth(castle))));
        z = front + (back_depth-front)*fraction;
      }
      float fold_offset = e.side == 0
          ? ActionVoxel_RearFoldOffset(e.rear_fold_top,local_z,contact,depth) : 0;
      if (e.side & 1) {
        /* Capture clipping can create a vertex midway along a native side.
         * Interpolate its original endpoint offsets to keep the side planar. */
        const float t = fmaxf(0,fminf(1,(native_y-((e.fold_length?e.fold_origin_y:e.y)-oy))/(e.fold_length?e.fold_length:e.length)));
        const float top = ActionVoxel_RearFoldOffset(e.rear_fold_top,local_z,contact,depth);
        const float bottom = ActionVoxel_RearFoldOffset(e.rear_fold_bottom,local_z,contact,depth);
        fold_offset = top + (bottom-top)*t;
      }
      float px=x+ox,py=native_y-fold_offset+oy;
      if(transformed)ActionVoxel_TransformPoint(&transformed->transform,px,py,z,&px,&py,&z);
      const float scale = 1 + z / fmaxf(1, g->camera.distance);
      const float u = (px-ox) / SR_PPU_SURFACE_MAX_WIDTH, v = (py-oy) / SR_PPU_SURFACE_MAX_HEIGHT;
      float wx = ((u - g->u0) / du - .5f) * g->aspect_x,
            wy = (.5f - (v - g->v0) / dv) * g->height_scale;
      wx *= scale;
      wy *= scale;
      ok = ok && VoxelProjectWithW(g, wx, wy, -z, &next.v[j].position, &next.distance, &next.depths[j],&next.clip_w[j]);
      if (!ok) break;
      /* Extend the stone's top rim all the way to the rear wall. Sampling
       * farther down its front face stretches the shadow beneath that rim
       * across the walking surface, making a connected floor look split. */
      /* Turf and rounded cave rock sample a small native texture patch.
       * Stretching one rim row across their depth produces long stripes. */
      const float material_depth = e.far_rank == 3 ? ActionVoxel_BranchDepth(castle) : depth;
      const float material_z = local_z - ((e.far_rank==1 || e.far_rank==3)
          ? ActionVoxel_EdgeRecess(castle,&e) : 0);
      /* Pixel-sized risers on a reviewed sand slope share its upper-surface
       * shading and texture footprint. Preserve the native silhouette while
       * avoiding alternating dark stair faces on granular material. */
      const bool soft = e.soft_surface && (e.side==0 || ((e.side&1) && e.length<=2));
      const float inset = e.side == 0 && e.textured_top ? .5f + 6 * material_z / depth
          : e.side == 0 && e.far_rank >= 2 ? 1 - material_z / material_depth : 1 + 2 * material_z / depth;
      const float sx = (e.side & 1) ? e.x - ox + .5f + (e.side == 1 ? -(soft?.5f:inset) : (soft?.5f:inset))
                                    : x + (x1 > x0 ? (end ? -.5f : .5f) : (end ? .5f : -.5f));
      const float original_sy = (e.side & 1) ? native_y + (soft ? .5f+6*material_z/depth :
                                        (y1 > y0 ? (end ? -.5f : .5f) : (end ? .5f : -.5f)))
                                    : e.y - oy + .5f + (e.side == 0 ? inset : -inset);
      /* The rolled row remains native artwork. Its seven pixels occupy the
       * rear part of the cap; their upright source is behind the new surface. */
      const float sy = next.rear_source ? e.y-oy+.5f-fold_offset : original_sy;
      const float top_u = sample_x + .5f + (e.top_sample_size - 1) *
          (x - tile_x0) / tile_width;
      const float top_v = sample_y + .5f + (e.top_sample_size - 1) *
          (local_z - tile_z0) / tile_depth;
      next.v[j].tex_coord = (ArRenderPointF){
          (sampled_top ? top_u : sx) / SR_PPU_SURFACE_MAX_WIDTH,
          (sampled_top ? top_v : sy) / SR_PPU_SURFACE_MAX_HEIGHT};
      float shade = (soft || e.side == 0   ? .90f
                     : e.side == 2 ? .54f
                                   : .72f) *
                    (1 - (soft?.18f:.38f) * local_z / depth) * (rear ? .88f : 1);
      shade *= 1 - SceneryDimming_Amount(layer->dimming, layer->dimming_ramp, next.v[j].tex_coord.x,
                                         next.v[j].tex_coord.y);
      if (layer->exposure && layer->exposure->sample)
        shade *= layer->exposure->sample(layer->exposure->userdata, next.v[j].tex_coord.x,
                                         next.v[j].tex_coord.y, next.v[j].tex_coord.x,
                                         next.v[j].tex_coord.y);
      next.v[j].color = (ArRenderColorF){layer->shade.r * shade, layer->shade.g * shade,
                                         layer->shade.b * shade, 1};
      if(owned_top)next.v[j].tex_coord=VoxelOwnedArtUv(e.top_owned_art-1,
          top_u-sample_x,top_v-sample_y);
      else if(transformed) {
        const unsigned source=next.rear_source?1:0,id=e.source_owned_art[source];
        if(!id)return false;
        next.v[j].tex_coord=VoxelOwnedArtUv(id-1,sx+ox-e.source_x[source],sy+oy-e.source_y[source]);
      }
    }
    if (!ok) continue;
    const ArRenderPointF a = next.v[0].position, b = next.v[1].position, c = next.v[2].position;
    float orientation = (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
    if (trunk_curve || column_side) {
      /* The relief tapers to zero at the root. Either triangle can collapse
       * there; retain the other triangle that closes the side against grass. */
      const ArRenderPointF d=next.v[3].position;
      orientation += (c.x-a.x)*(d.y-a.y)-(c.y-a.y)*(d.x-a.x);
    }
    if (orientation >= -.00001f) continue;
    ObserveVoxelFace(g,layer,&next,observer_xy,e.side==0?kDioramaVoxelFace_Top:kDioramaVoxelFace_Side,sampled_top?&e:NULL);
    if (!ReserveVoxelSide(device, g, layer, &count, blur)) return false;
    s_voxel_sides[count++] = next;
    }
  }
  return SubmitVoxelSides(device, g, layer, count, blur);
}
/* Sort and submit the accumulated faces, farthest first. */
static bool SubmitVoxelSides(ArRenderDevice *device, const DioramaViewGeometry *g,
                             const DioramaLayerDraw *layer, int count, float blur) {
  if (!count) return true;
  SortVoxelSides(count);
  const int32_t *quad_indices = VoxelQuadIndices();
  DioramaSceneryLightEffectParams light = layer->lighting;
  light.mode = blur > 0 ? 1 : 0;
  light.blur_radius = blur;
  light.attributes_uv = (ArRenderRectF){0, 0, 1, 1};
  light.texel_width = 1.f / SR_PPU_SURFACE_MAX_WIDTH;
  light.texel_height = 1.f / SR_PPU_SURFACE_MAX_HEIGHT;
  const bool bind_dof = !g->depth_sink && blur > 0 && !ArRenderTexture_IsValid(light.illumination);
  if (bind_dof) {
    const DioramaDofEdgeEffectParams params = {.texel_width = light.texel_width,
                                               .texel_height = light.texel_height,
                                               .blur_radius = blur,
                                               .u_max = 1,
                                               .v_max = 1};
    if (!DioramaEffectBackend_BindDofEdge(device, &params)) {
      (void)DioramaEffectBackend_Unbind(device);
      return false;
    }
  }
  bool ok = true;
  /* Keep distance order on painter backends as well as depth backends.
   * Only adjacent faces with the same native source share a submission. */
  for (int first = 0; first < count && ok;) {
    const bool rear_source = s_voxel_sides[s_voxel_order[first]].rear_source;
    const bool owned_source = s_voxel_sides[s_voxel_order[first]].owned_source;
    int end = first + 1;
    while (end < count && s_voxel_sides[s_voxel_order[end]].rear_source == rear_source &&
        s_voxel_sides[s_voxel_order[end]].owned_source == owned_source) ++end;
    const int packed = end - first;
    for (int i = 0; i < packed; ++i) {
      const VoxelSide *face = &s_voxel_sides[s_voxel_order[first+i]];
      memcpy(s_voxel_vertices+i*4,face->v,sizeof(face->v));
      if(g->depth_sink) {
        memcpy(s_voxel_depths+i*4,face->depths,sizeof(face->depths));
        memcpy(s_voxel_clip_w+i*4,face->clip_w,sizeof(face->clip_w));
      }
    }
    ok = RenderShadedDioramaDepthGeometryWithW(device,
        owned_source ? VoxelAuthoredAtlas() : rear_source ? layer->voxel_rear_texture : layer->texture,
        s_voxel_vertices,s_voxel_depths,g->depth_sink?s_voxel_clip_w:NULL,packed*4,quad_indices,packed*6,
        layer->blend,NULL,(ArRenderRectF){0,0,1,1},&light,g->depth_sink,layer->description->plane,false);
    first = end;
  }
  return (!bind_dof || DioramaEffectBackend_Unbind(device)) && ok;
}
static bool DrawVoxelEdges(ArRenderDevice *device, const DioramaCapture *capture,
                            const DioramaViewGeometry *g, const DioramaLayerDraw *layer,
                            float near_depth, float far_depth) {
  return DrawVoxelGeometry(device, capture, g, layer, near_depth, far_depth, kVoxelEdgeNative);
}
static bool DrawVoxelHighDecks(ArRenderDevice *device, const DioramaCapture *capture,
                                const DioramaViewGeometry *g, const DioramaLayerDraw *layer,
                                ArRenderTexture high) {
  if (!ActionVoxel_HasHighDecks(layer->voxel_geometry) || !ArRenderTexture_IsValid(high))
    return true;
  DioramaLayerDraw deck = *layer;
  deck.texture = high;
  return DrawVoxelGeometry(device, capture, g, &deck, 0, layer->voxel_depth,
                            kVoxelEdgeHighDeckBehind);
}
static bool DrawVoxelRearTrims(ArRenderDevice *device, const DioramaCapture *capture,
                                const DioramaViewGeometry *g, const DioramaLayerDraw *layer,
                                ArRenderTexture high) {
  unsigned count;
  ActionVoxel_RearTrims(layer->voxel_geometry, &count);
  if (!count || !ArRenderTexture_IsValid(high)) return true;
  DioramaLayerDraw rear = *layer;
  rear.texture = high;
  return DrawVoxelGeometry(device, capture, g, &rear, 0,
                            ActionVoxel_TrimDepth(layer->voxel_geometry), kVoxelEdgeRearTrim);
}

/* One continuous native-textured surface per pool, replacing stacked sheets.
 * Native priority and the captured texture stay authoritative; no water effect
 * toggle or authored emitter is required for the actual pool geometry. */
static bool DrawVoxelWaterSurfaces(ArRenderDevice *device, const DioramaCapture *capture,
    const DioramaViewGeometry *g, const DioramaLayerDraw *layer) {
  unsigned total;
  const VoxelWaterSurface *surfaces = ActionVoxel_WaterSurfaces(layer->voxel_geometry,&total);
  if (!total) return true;
  const bool bg2 = layer->description->plane == SR_PPU_OVERLAY_BG2 ||
      layer->description->plane == kDioramaPlane_Bg2Hi;
  const bool high = layer->description->plane == kDioramaPlane_Bg2Hi ||
      layer->description->plane == kDioramaPlane_Bg1Hi;
  /* Marahna's native water colour is main-screen BG2 plus subscreen BG1.
   * Extend both contributions over the same shoreline surface. BG2's UVs
   * are screen-aligned native samples, while placement follows BG1 terrain;
   * independently projecting the two would leave a black seam at the bank. */
  const bool jungle=ActionVoxel_Profile(layer->voxel_geometry)==kActionVoxelProfile_MarahnaJungle;
  const bool main_water=bg2 && !high && jungle;
  DioramaViewGeometry water_geometry;
  if(main_water) {
    water_geometry=*g;
    Diorama_TranslateCameraWorldY(water_geometry.matrix,-g->bg2_world_y_offset);
    g=&water_geometry;
  }
  const bool source_bg2=bg2 && !main_water;
  const int ox = (source_bg2?capture->bg2_camera_x:capture->bg1_camera_x)-(capture->width-256)/2-capture->obj_apron;
  const int oy = (source_bg2?capture->bg2_camera_y:capture->camera_y)-capture->authentic_y0+1;
  const float du = g->u1-g->u0, dv = g->v1-g->v0;
  int count = 0;
  for (unsigned i = 0; i < total; ++i) {
    const VoxelWaterSurface p = surfaces[i];
    if (main_water ? p.background || !p.high_priority :
        p.high_priority != high || p.background != bg2) continue;
    const float shore_offset=jungle?8:3;
    const float x0 = fmaxf(0,p.x-ox), x1 = fminf(capture->width,p.x+p.width-ox), y = p.y-oy+shore_offset;
    /* Both terms of native full-add water sample the same submerged band.
     * Pulling BG2 from a different 16px interval introduces a pale straight
     * strip which is absent from the native shore, and crosses into the next
     * tile. Keep the original colour animation paired across the whole cap. */
    const float sample_y=p.y-oy+p.sample_y;
    const unsigned sample_height=p.sample_height;
    if (x1 <= x0 || y < 0 || sample_y < 0 || sample_y+sample_height >= capture->height) continue;
    VoxelSide face = {0}; bool ok = true;
    for (unsigned j = 0; j < 4; ++j) {
      const bool end = j == 1 || j == 2, back = j >= 2;
      const float z = back ? layer->voxel_depth : 0;
      const float x = end ? x1 : x0, u = x/SR_PPU_SURFACE_MAX_WIDTH, v = y/SR_PPU_SURFACE_MAX_HEIGHT;
      const float scale = 1+z/fmaxf(1,g->camera.distance);
      const float wx = ((u-g->u0)/du-.5f)*g->aspect_x*scale;
      const float wy = (.5f-(v-g->v0)/dv)*g->height_scale*scale;
      ok = ok && VoxelProject(g,wx,wy,-z,&face.v[j].position, &face.distance, &face.depths[j]);
      if (!ok) break;
      face.v[j].tex_coord = (ArRenderPointF){(x+(end?-.5f:.5f))/SR_PPU_SURFACE_MAX_WIDTH,
          (sample_y+(back?sample_height-.5f:.5f))/SR_PPU_SURFACE_MAX_HEIGHT};
      /* The recessed bank softly shades the added horizontal water, keeping
       * its native colour from reading as a luminous masonry rim. Apply the
       * same light to both full-add terms; native vertical water is unchanged. */
      const float surface_light=jungle?(back?.72f:.86f):1.f;
      face.v[j].color = (ArRenderColorF){layer->shade.r*surface_light,
                                        layer->shade.g*surface_light,layer->shade.b*surface_light,
                                        layer->shade.a*(p.opaque || main_water?1.f:.85f)};
    }
    if (!ok) continue;
    memcpy(s_voxel_vertices+count*4,face.v,sizeof(face.v));
    if(g->depth_sink)memcpy(s_voxel_depths+count*4,face.depths,sizeof(face.depths));
    ++count;
  }
  return !count || RenderShadedDioramaDepthGeometry(device,layer->texture,s_voxel_vertices,s_voxel_depths,count*4,
      VoxelQuadIndices(),count*6,layer->blend,NULL,(ArRenderRectF){0,0,1,1},&layer->lighting,
      g->depth_sink,layer->description->plane,false);
}

/* Partition the native water face at compiled curtain bounds. The stream's
 * original pixels move to the walking plane; two transverse sides span the
 * entire path. Their rear portions draw before actors; the original curtain
 * and near portions keep native priority. All faces share the live texture. */
_Static_assert((2*kVoxelMaxWaterfalls+DIORAMA_SUBDIV_X)*(2*kVoxelMaxWaterfalls+DIORAMA_SUBDIV_Y)+4*kVoxelMaxWaterfalls<=kVoxelMaxVisibleFaces,
               "water curtain partitions fit the shared face scratch");
static int VoxelCutCompare(const void *a,const void *b) {
  const float x=*(const float *)a,y=*(const float *)b;
  return x<y?-1:x>y;
}
static unsigned VoxelUniqueCuts(float *cuts,unsigned count) {
  qsort(cuts,count,sizeof(*cuts),VoxelCutCompare);
  unsigned n=0;for(unsigned i=0;i<count;++i)if(!n || cuts[i]!=cuts[n-1])cuts[n++]=cuts[i];
  return n;
}
static bool VoxelSourceQuad(const DioramaViewGeometry *g,const DioramaLayerDraw *layer,
    float x0,float y0,float x1,float y1,float near,float far,bool side,bool reverse,
    float source_x0,float source_x1,float alpha,VoxelSide *face) {
  *face=(VoxelSide){0};
  for(unsigned j=0;j<4;++j) {
    const bool end=j==1 || j==2,bottom=j>=2;
    const float x=end?x1:x0,y=bottom?y1:y0,z=end?far:near;
    const float u=x/SR_PPU_SURFACE_MAX_WIDTH,v=y/SR_PPU_SURFACE_MAX_HEIGHT;
    const float scale=1+z/fmaxf(1,g->camera.distance);
    if(!VoxelProjectWithW(g,((u-g->u0)/(g->u1-g->u0)-.5f)*g->aspect_x*scale,
        (.5f-(v-g->v0)/(g->v1-g->v0))*g->height_scale*scale,-z,
        &face->v[j].position, &face->distance, &face->depths[j],&face->clip_w[j]))return false;
    face->v[j].tex_coord=(ArRenderPointF){side?
        ((end!=reverse?source_x1-.5f:source_x0+.5f)/SR_PPU_SURFACE_MAX_WIDTH):u,
        side?(y+(bottom?-.5f:.5f))/SR_PPU_SURFACE_MAX_HEIGHT:v};
    face->v[j].color=layer->shade;face->v[j].color.a*=alpha;
  }
  return true;
}
static bool VoxelCurveQuad(const DioramaViewGeometry *g,const DioramaLayerDraw *layer,
    const VoxelTrunkCurve *curve,float x0,float y0,float x1,float y1,float world_y0,
    bool column,VoxelSide *face) {
  *face=(VoxelSide){0};
  for(unsigned j=0;j<4;++j) {
    const float x=j==1||j==2?x1:x0,y=j>=2?y1:y0;
    const float z=column?ActionVoxel_ColumnFrontDepth(layer->voxel_geometry,curve,y+world_y0):
        ActionVoxel_TrunkFrontDepth(layer->voxel_geometry,curve,y+world_y0);
    const float u=x/SR_PPU_SURFACE_MAX_WIDTH,v=y/SR_PPU_SURFACE_MAX_HEIGHT;
    const float scale=1+z/fmaxf(1,g->camera.distance);
    if(!VoxelProjectWithW(g,((u-g->u0)/(g->u1-g->u0)-.5f)*g->aspect_x*scale,
        (.5f-(v-g->v0)/(g->v1-g->v0))*g->height_scale*scale,-z,
        &face->v[j].position,&face->distance,&face->depths[j],&face->clip_w[j]))return false;
    face->v[j].tex_coord=(ArRenderPointF){u,v};
    face->v[j].color=layer->shade;
  }
  return true;
}
static unsigned BuildVoxelWaterfallFaces(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer,bool rear) {
  unsigned total;const VoxelWaterfall *falls=ActionVoxel_Waterfalls(layer->voxel_geometry,&total);
  const bool high=layer->description->plane==kDioramaPlane_Bg2Hi;
  const float ox=capture->bg2_camera_x-(capture->width-256)*.5f-capture->obj_apron;
  const float oy=capture->bg2_camera_y-capture->authentic_y0+1;
  const float left=g->u0*SR_PPU_SURFACE_MAX_WIDTH,right=g->u1*SR_PPU_SURFACE_MAX_WIDTH;
  const float top=g->v0*SR_PPU_SURFACE_MAX_HEIGHT,bottom=g->v1*SR_PPU_SURFACE_MAX_HEIGHT;
  ArRenderRectF visible[kVoxelMaxWaterfalls];unsigned n=0;
  float xs[1+DIORAMA_SUBDIV_X+2*kVoxelMaxWaterfalls]={left,right};
  float ys[1+DIORAMA_SUBDIV_Y+2*kVoxelMaxWaterfalls]={top,bottom};
  unsigned nx=2,ny=2,count=0;
  /* Keep the ordinary perspective sampling density between curtain cuts. */
  for(unsigned x=1;x<DIORAMA_SUBDIV_X;++x)xs[nx++]=left+(right-left)*x/DIORAMA_SUBDIV_X;
  for(unsigned y=1;y<DIORAMA_SUBDIV_Y;++y)ys[ny++]=top+(bottom-top)*y/DIORAMA_SUBDIV_Y;
  for(unsigned i=0;i<total;++i) {
    const VoxelWaterfall p=falls[i];
    if(p.high_priority!=high)continue;
    const float x0=fmaxf(left,p.x-ox),x1=fminf(right,p.x+p.width-ox);
    const float y0=fmaxf(top,p.y-oy),y1=fminf(bottom,p.y+p.height-oy+3);
    if(x1<=x0 || y1<=y0)continue;
    if(!rear) {
      visible[n++]=(ArRenderRectF){x0,y0,x1-x0,y1-y0};
      xs[nx++]=x0;xs[nx++]=x1;ys[ny++]=y0;ys[ny++]=y1;
    }
    /* The transverse profile needs the complete captured native strip. */
    if(p.x-ox<left || p.x+p.width-ox>right)continue;
    /* Only actual stream boundaries get side curtains; cropping is not a wall. */
    for(unsigned side=0;side<2;++side) {
      const float x=p.x-ox+(side?p.width:0);
      if(x<left || x>right)continue;
      /* OBJ2 is .01 in front of the nominal walking plane. Split there so
       * far water cannot veil a player standing outside the native stream.
       * UVs remain continuous; neither pass duplicates the side curtain. */
      const float split=fmaxf(0,layer->voxel_recess-.01f);
      const float near=rear?split:0,far=rear?layer->voxel_depth:split;
      if(far<=near)continue;
      const float a=near/layer->voxel_depth,b=far/layer->voxel_depth;
      const float sx0=p.x-ox+p.width*(side?1-b:a),sx1=p.x-ox+p.width*(side?1-a:b);
      if(VoxelSourceQuad(g,layer,x,y0,x,y1,near,far,true,side!=0,
          sx0,sx1,.65f,&s_voxel_sides[count]))++count;
    }
  }
  if(rear) {
    SortVoxelSides(count);
    return count;
  }
  nx=VoxelUniqueCuts(xs,nx);ny=VoxelUniqueCuts(ys,ny);
  for(unsigned y=0;y+1<ny;++y)for(unsigned x=0;x+1<nx;++x) {
    const float cx=(xs[x]+xs[x+1])*.5f,cy=(ys[y]+ys[y+1])*.5f;
    float depth=0;
    for(unsigned i=0;i<n;++i)if(cx>visible[i].x && cx<visible[i].x+visible[i].w &&
        cy>visible[i].y && cy<visible[i].y+visible[i].h) {depth=layer->voxel_recess;break;}
    if(VoxelSourceQuad(g,layer,xs[x],ys[y],xs[x+1],ys[y+1],depth,depth,false,false,0,0,1,
        &s_voxel_sides[count]))++count;
  }
  SortVoxelSides(count);
  return count;
}

/* Keep material relief on its compiled depth (forest contact or temple
 * architecture). The native front texture separates rear plants and priorities;
 * partition at world cell boundaries so neighbouring materials cannot bend.
 * Short merged runs retain the existing lighting sampling density. */
static unsigned BuildVoxelFrontFaces(const DioramaCapture *capture,const DioramaViewGeometry *g,
    const DioramaLayerDraw *layer) {
  _Static_assert((SR_PPU_SURFACE_MAX_WIDTH/8+1)*(SR_PPU_SURFACE_MAX_HEIGHT/8+1)+
                 kVoxelMaxPixelRecessCells*63<=kVoxelMaxVisibleFaces,
                 "cell partitions plus bounded pixel masks fit the shared face budget");
  const int ox=capture->bg1_camera_x-(capture->width-256)/2-capture->obj_apron;
  const int oy=capture->camera_y-capture->authentic_y0+1;
  const float left=g->u0*SR_PPU_SURFACE_MAX_WIDTH,right=g->u1*SR_PPU_SURFACE_MAX_WIDTH;
  const float top=g->v0*SR_PPU_SURFACE_MAX_HEIGHT,bottom=g->v1*SR_PPU_SURFACE_MAX_HEIGHT;
  enum { columns=SR_PPU_SURFACE_MAX_WIDTH/8+1,rows=SR_PPU_SURFACE_MAX_HEIGHT/8+1 };
  unsigned char cells[columns*rows]={0};
  const int tx0=(int)floorf((left+ox)/8),ty0=(int)floorf((top+oy)/8);
  const int nx=(int)ceilf((right+ox)/8)-tx0,ny=(int)ceilf((bottom+oy)/8)-ty0;
  if(nx<=0 || ny<=0 || nx>columns || ny>rows)return 0;
  const bool high=layer->description->plane==kDioramaPlane_Bg1Hi;
  const bool hidden_fronts=ActionVoxel_HasHiddenNativeFronts(layer->voxel_geometry);
  for(int y=0;y<ny;++y)for(int x=0;x<nx;++x) {
    cells[y*nx+x]=(ActionVoxel_FrontRecess(layer->voxel_geometry,tx0+x,ty0+y)>0) |
        (ActionVoxel_TrunkCurveForCell(layer->voxel_geometry,tx0+x,ty0+y)?4:0) |
        (ActionVoxel_ColumnCurveForCell(layer->voxel_geometry,tx0+x,ty0+y)?8:0) |
        (!high && ActionVoxel_PixelRecessRows(layer->voxel_geometry,tx0+x,ty0+y)?16:0);
    const VoxelAuthoredCell *a=hidden_fronts?ActionVoxel_AuthoredCellAt(layer->voxel_geometry,
        (int)floorf((tx0+x)*.5f),(int)floorf((ty0+y)*.5f)):NULL;
    if(a && a->native_front_hidden)cells[y*nx+x]|=2;
  }
  /* A successfully compiled pure gate already supplies the transverse
   * grating. Omit its duplicate flat face, retaining the same unmodified
   * texture for the gate mesh. Mixed jamb/grille cells keep their native face. */
  unsigned gate_count;const VoxelGate *gates=ActionVoxel_Gates(layer->voxel_geometry,&gate_count);
  for(unsigned i=0;i<gate_count;++i) {
    const VoxelGate *gate=&gates[i];
    if(gate->width || gate->centered || gate->high_priority!=high || (gate->x&7) ||
        gate->x<ox || gate->x+8>ox+capture->width)continue;
    const int x=gate->x/8-tx0;if(x<0 || x>=nx)continue;
    for(int y=0;y<ny;++y)
      if((ty0+y)*8>=gate->y && (ty0+y+1)*8<=gate->y+gate->height)cells[y*nx+x]|=2;
  }
  unsigned count=0;
  for(int y=0;y<ny;++y)for(int x=0;x<nx;++x) {
    const unsigned tag=cells[y*nx+x];
    if(tag&2)continue;
    if(tag&16) {
      /* Preserve the native low-priority surface: only the reviewed wood
       * pixels in this mixed character follow its rear post. Water keeps
       * its exact source coordinates and zero depth. Merge equal pixel
       * rectangles within this cell; all ordinary cells use the fast path. */
      const uint8_t *mask=ActionVoxel_PixelRecessRows(layer->voxel_geometry,tx0+x,ty0+y);
      uint8_t visited[8]={0};
      const int sx=(tx0+x)*8-ox,sy=(ty0+y)*8-oy;
      for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
        if(visited[py]&(1u<<(7-px)))continue;
        const bool rear=(mask[py]&(1u<<(7-px)))!=0;
        unsigned ex=px+1,ey=py+1;
        while(ex<8 && !(visited[py]&(1u<<(7-ex))) &&
            !!(mask[py]&(1u<<(7-ex)))==rear)++ex;
        const unsigned bits=((1u<<(ex-px))-1)<<(8-ex);
        while(ey<8 && !(visited[ey]&bits) && (mask[ey]&bits)==(rear?bits:0))++ey;
        for(unsigned yy=py;yy<ey;++yy)visited[yy]|=bits;
        const float x0=fmaxf(left,sx+(int)px),x1=fminf(right,sx+(int)ex);
        const float y0=fmaxf(top,sy+(int)py),y1=fminf(bottom,sy+(int)ey);
        if(x1<=x0 || y1<=y0)continue;
        if(count==kVoxelMaxVisibleFaces)return 0;
        const float depth=rear?ActionVoxel_SceneryRecess(layer->voxel_geometry):0;
        if(VoxelSourceQuad(g,layer,x0,y0,x1,y1,depth,depth,false,false,0,0,1,
            &s_voxel_sides[count]))++count;
      }
      cells[y*nx+x]|=2;
      continue;
    }
    const VoxelTrunkCurve *curve=tag&4?
        ActionVoxel_TrunkCurveForCell(layer->voxel_geometry,tx0+x,ty0+y):tag&8?
        ActionVoxel_ColumnCurveForCell(layer->voxel_geometry,tx0+x,ty0+y):NULL;
    int ex=x+1,ey=y+1;
    /* World-fixed 64px limits match ordinary perspective sampling. Merge
     * coplanar cells in both axes before projection and shader submission. */
    while(!curve && ex<nx && (tx0+ex)%8 && cells[y*nx+ex]==tag)++ex;
    while(!curve && ey<ny && (ty0+ey)%8) {
      bool same=true;
      for(int xx=x;xx<ex;++xx)if(cells[ey*nx+xx]!=tag){same=false;break;}
      if(!same)break;
      ++ey;
    }
    for(int yy=y;yy<ey;++yy)for(int xx=x;xx<ex;++xx)cells[yy*nx+xx]|=2;
    const float x0=fmaxf(left,(tx0+x)*8-ox),x1=fminf(right,(tx0+ex)*8-ox);
    const float y0=fmaxf(top,(ty0+y)*8-oy),y1=fminf(bottom,(ty0+ey)*8-oy);
    if(curve) {
      float cuts[4];const unsigned n=VoxelTrunkCuts(curve,y0,y1,oy,false,cuts);
      for(unsigned i=0;i+1<n;++i) {
        if(count==kVoxelMaxVisibleFaces)return 0;
        if(VoxelCurveQuad(g,layer,curve,x0,cuts[i],x1,cuts[i+1],oy,!!(tag&8),&s_voxel_sides[count]))++count;
      }
      continue;
    }
    if(count==kVoxelMaxVisibleFaces)return 0;
    const float depth=tag?ActionVoxel_SceneryRecess(layer->voxel_geometry):0;
    if(VoxelSourceQuad(g,layer,x0,y0,x1,y1,depth,depth,false,false,0,0,1,
        &s_voxel_sides[count]))++count;
  }
  if(g->options->voxel_observe_face)for(unsigned i=0;i<count;++i) {
    float world_xy[8];
    for(unsigned j=0;j<4;++j) {
      world_xy[j*2]=s_voxel_sides[i].v[j].tex_coord.x*SR_PPU_SURFACE_MAX_WIDTH+ox;
      world_xy[j*2+1]=s_voxel_sides[i].v[j].tex_coord.y*SR_PPU_SURFACE_MAX_HEIGHT+oy;
    }
    ObserveVoxelFace(g,layer,&s_voxel_sides[i],world_xy,kDioramaVoxelFace_Front,NULL);
  }
  SortVoxelSides(count);
  return count;
}
