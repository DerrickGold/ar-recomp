/* Finite selection data from the same draw as the visible preview. No second
 * material classifier or browser projection model is involved. */
enum { kEditorVoxelFaces = 32768 };
static DioramaVoxelPickFace s_voxel_faces[kEditorVoxelFaces];
static unsigned s_voxel_face_count;
static bool s_voxel_pick_valid, s_voxel_pick_overflow;
static float s_voxel_view_x, s_voxel_view_y;

static void ObserveVoxelFace(void *unused, const DioramaVoxelPickFace *face) {
  (void)unused;
  if (s_voxel_face_count == kEditorVoxelFaces) { s_voxel_pick_overflow = true; return; }
  DioramaVoxelPickFace *copy = &s_voxel_faces[s_voxel_face_count++];
  *copy = *face;
  for (int i = 0; i < 4; ++i) {
    copy->screen_xy[i*2] += s_voxel_view_x;
    copy->screen_xy[i*2+1] += s_voxel_view_y;
  }
}

typedef struct VoxelJson { size_t length; bool failed; } VoxelJson;
static void VoxelJsonAdd(VoxelJson *out, const char *format, ...) {
  if (out->failed) return;
  const size_t capacity = DioramaPreview_Capacity();
  if (out->length >= capacity) { out->failed = true; return; }
  va_list args; va_start(args,format);
  const int n = vsnprintf((char *)DioramaPreview_Input()+out->length,
      capacity-out->length,format,args);
  va_end(args);
  if (n < 0 || (size_t)n >= capacity-out->length) out->failed = true;
  else out->length += (size_t)n;
}
static unsigned VoxelJsonDone(const VoxelJson *out) { return out->failed ? 0 : (unsigned)out->length; }

/* Audit copy-out of the complete observed draw, independent of selection
 * limits. A caller must reject overflow rather than count a truncated view
 * as inspected. This reads the existing editor observer; it changes no draw. */
unsigned RoomPreview_VoxelAuditCount(void) { return s_voxel_face_count; }
int RoomPreview_VoxelAuditComplete(void) { return s_voxel_pick_valid&&!s_voxel_pick_overflow; }
unsigned RoomPreview_VoxelAuditRead(unsigned first,unsigned maximum) {
  if(!RoomPreview_VoxelAuditComplete() || first>s_voxel_face_count || !maximum)return 0;
  if(maximum>512)maximum=512;
  const unsigned end=first+maximum<s_voxel_face_count?first+maximum:s_voxel_face_count;
  VoxelJson out={0};
  VoxelJsonAdd(&out,"{\"total\":%u,\"first\":%u,\"complete\":true,\"fields\":[\"kind\",\"priority\",\"plane\",\"cell_x\",\"cell_y\",\"owner\",\"authored\",\"art_index\",\"art_source_x\",\"art_source_y\",\"art_source_size\",\"sample_alpha\",\"world_xy\",\"depth\",\"screen_xy\",\"clip_w\",\"art_uv\"],\"faces\":[",s_voxel_face_count,first);
  for(unsigned i=first;i<end;++i) {
    const DioramaVoxelPickFace *f=s_voxel_faces+i;
    VoxelJsonAdd(&out,"%s[%d,%d,%d,%d,%d,%u,%u,%u,%d,%d,%u,%u",i==first?"":",",f->kind,f->priority,f->plane,f->cell_x,f->cell_y,f->owner,f->authored,f->art_index,f->art_source_x,f->art_source_y,f->art_source_size,f->sample_alpha);
    const float *values[]={f->world_xy,f->depth,f->screen_xy,f->clip_w,f->art_uv};
    const unsigned sizes[]={8,4,8,4,8};
    for(unsigned a=0;a<5;++a) {
      VoxelJsonAdd(&out,",[");
      for(unsigned j=0;j<sizes[a];++j)VoxelJsonAdd(&out,"%s%.7g",j?",":"",values[a][j]);
      VoxelJsonAdd(&out,"]");
    }
    VoxelJsonAdd(&out,"]");
  }
  VoxelJsonAdd(&out,"]}");return VoxelJsonDone(&out);
}

/* Clip in native source space, retaining homogeneous projected coordinates.
 * This outlines the selected cell, rather than an entire large backing mesh. */
typedef struct VoxelPickVertex { float x,y,xw,yw,w; } VoxelPickVertex;
static unsigned VoxelClipCell(const DioramaVoxelPickFace *f,int x,int y,
    ArRenderPointF points[12]) {
  VoxelPickVertex a[12],b[12]; unsigned count=4;
  for(unsigned i=0;i<4;++i)a[i]=(VoxelPickVertex){f->world_xy[i*2],f->world_xy[i*2+1],
      f->screen_xy[i*2]*f->clip_w[i],f->screen_xy[i*2+1]*f->clip_w[i],f->clip_w[i]};
  for(int edge=0;edge<4 && count;++edge) {
    const float bound=(edge<2?x:y)*16.f+((edge&1)?16.f:0.f);
    unsigned next=0;
    for(unsigned i=0;i<count;++i) {
      const VoxelPickVertex p=a[i],q=a[(i+1)%count];
      const float pv=edge<2?p.x:p.y,qv=edge<2?q.x:q.y;
      const float dp=(edge&1)?bound-pv:pv-bound,dq=(edge&1)?bound-qv:qv-bound;
      if(dp>=-.001f && next<12)b[next++]=p;
      if((dp<0)!=(dq<0) && next<12) {
        const float t=dp/(dp-dq);
        b[next++]=(VoxelPickVertex){p.x+(q.x-p.x)*t,p.y+(q.y-p.y)*t,
            p.xw+(q.xw-p.xw)*t,p.yw+(q.yw-p.yw)*t,p.w+(q.w-p.w)*t};
      }
    }
    count=next;memcpy(a,b,count*sizeof(*a));
  }
  for(unsigned i=0;i<count;++i) {
    if(!(a[i].w>0))return 0;
    points[i]=(ArRenderPointF){a[i].xw/a[i].w,a[i].yw/a[i].w};
  }
  return count;
}
typedef struct VoxelOutline {
  ArRenderPointF *points;
  unsigned count,capacity;
} VoxelOutline;
static bool VoxelOutlineAppend(VoxelOutline *out,const ArRenderPointF *points,unsigned count) {
  /* Each observed quad clips to at most twelve vertices. Reserve enough for
   * every observation and the hull workspace; never discard later patches. */
  const unsigned limit=kEditorVoxelFaces*12,wanted=out->count+count;
  if(wanted>limit)return false;
  if(wanted>out->capacity) {
    unsigned capacity=out->capacity?out->capacity:128;
    while(capacity<wanted)capacity=capacity>limit/2?limit:capacity*2;
    void *next=realloc(out->points,(size_t)capacity*3*sizeof(*out->points));
    if(!next)return false;
    out->points=next;out->capacity=capacity;
  }
  memcpy(out->points+out->count,points,count*sizeof(*points));out->count=wanted;return true;
}
/* Keep the historical generic Side coordinate query as an aggregate.
 * Direct underside picks use Bottom, so sampling cannot repaint lateral art. */
static bool VoxelObservedKindMatches(const DioramaVoxelPickFace *face,int kind) {
  return face->kind==kind || (kind==kDioramaVoxelFace_Side && !face->authored &&
      face->kind==kDioramaVoxelFace_Bottom);
}
static bool VoxelTileOutline(int x,int y,int kind,VoxelOutline *out,int *priority) {
  for(unsigned i=0;i<s_voxel_face_count;++i) {
    const DioramaVoxelPickFace *f=&s_voxel_faces[i];
    if(!VoxelObservedKindMatches(f,kind) || ((f->authored||f->cell_x>=0)&&(f->cell_x!=x||f->cell_y!=y)))continue;
    ArRenderPointF clipped[12];const unsigned n=VoxelClipCell(f,x,y,clipped);
    if(n<3)continue;
    if(!VoxelOutlineAppend(out,clipped,n))return false;
    *priority=f->priority;
  }
  if(out->count) {
    ArRenderPointF *hull=out->points+out->capacity;
    out->count=EditorVoxelPick_Hull(out->points,out->count,hull);
    memmove(out->points,hull,out->count*sizeof(*hull));
  }
  return true;
}
/* A direct pick identifies one rendered patch. Coordinate selections may cover
 * several material bands, so only expose a native donor when every visible
 * patch agrees. A refresh must also explicitly clear a former donor. */
static void VoxelJsonSampleSource(VoxelJson *json,int x,int y,int kind,
    const VoxelAuthoredCell *authored,const DioramaVoxelPickFace *picked,bool observed) {
  int source_x=0,source_y=0;unsigned size=0;bool ambiguous=false;
  if(picked) {
    source_x=picked->art_source_x;source_y=picked->art_source_y;size=picked->art_source_size;
  } else if(authored&&authored->surface) {
    static const int art_indices[]={0,1,2,3,-1,4,5,6};
    const int art=art_indices[kind];
    if(art>=0) {
      source_x=authored->surface_sample_x[art];source_y=authored->surface_sample_y[art];
      size=authored->surface_sample_size[art];
    }
  } else if(observed) {
    bool found=false;
    for(unsigned i=0;i<s_voxel_face_count;++i) {
      const DioramaVoxelPickFace *f=&s_voxel_faces[i];
      if(!VoxelObservedKindMatches(f,kind) || ((f->authored||f->cell_x>=0)&&(f->cell_x!=x||f->cell_y!=y)))continue;
      ArRenderPointF clipped[12];if(VoxelClipCell(f,x,y,clipped)<3)continue;
      if(found&&(size!=f->art_source_size || (size&&
          (source_x!=f->art_source_x||source_y!=f->art_source_y)))) {ambiguous=true;break;}
      found=true;source_x=f->art_source_x;source_y=f->art_source_y;size=f->art_source_size;
    }
  }
  if(size&&!ambiguous)VoxelJsonAdd(json,
      ",\"sampleSource\":{\"x\":%d,\"y\":%d,\"size\":%u,\"bg\":0}",source_x,source_y,size);
  else VoxelJsonAdd(json,",\"sampleSource\":null");
  VoxelJsonAdd(json,",\"sampleSourceAmbiguous\":%s",ambiguous?"true":"false");
}
static bool VoxelFaceInk(const DioramaVoxelPickFace *face,const EditorVoxelPickHit *hit,
    float screen_x,float screen_y) {
  if(face->authored) {
    const VoxelAuthoredCell *cell=ActionVoxel_AuthoredCellAt(EditorRoomScene_Scene(s_room)->voxel_geometry,
        face->cell_x,face->cell_y);
    if(!cell || face->art_index>=7 || !cell->pixels[face->art_index])return false;
    DioramaVoxelPickFace source=*face;EditorVoxelPickHit uv;
    memcpy(source.world_xy,face->art_uv,sizeof(source.world_xy));
    if(!EditorVoxelPick_Hit(&source,screen_x,screen_y,&uv))return false;
    const int x=(int)fminf(15,fmaxf(0,floorf(uv.world_x)));
    const int y=(int)fminf(15,fmaxf(0,floorf(uv.world_y)));
    return (cell->pixels[face->art_index][y*16+x]&0x8000)!=0;
  }
  const bool native_actor=face->kind==kDioramaVoxelFace_Object&&!face->owner;
  if(face->kind!=kDioramaVoxelFace_Front && face->kind!=kDioramaVoxelFace_Backing&&!native_actor)return true;
  if(face->plane<0)return true; /* Owned art is already a bounded opaque face. */
  const DioramaCapture *capture=EditorRoomScene_Capture(s_room);
  const int x=(int)floorf(hit->world_x-(capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron));
  const int y=(int)floorf(hit->world_y-(capture->camera_y-capture->authentic_y0+(native_actor?0:1)));
  if(face->plane>=kDioramaPlane_Count||x<0||y<0||x>=(int)SR_PPU_SURFACE_MAX_WIDTH||y>=(int)SR_PPU_SURFACE_MAX_HEIGHT||!s_pixels[face->plane])return false;
  uint32_t pixel;memcpy(&pixel,s_pixels[face->plane]+((size_t)y*SR_PPU_SURFACE_MAX_WIDTH+x)*4,4);
  return (pixel>>24)!=0;
}

static void VoxelJsonPolygon(VoxelJson *json,const ArRenderPointF *points,unsigned count) {
  VoxelJsonAdd(json,"\"quad\":[");
  for(unsigned i=0;i<count;++i)VoxelJsonAdd(json,"%s[%.4f,%.4f]",i?",":"",points[i].x,points[i].y);
  VoxelJsonAdd(json,"]");
}
static void VoxelJsonArt(VoxelJson *json,const char *key,const DioramaVoxelArt *art) {
  if(!art||!art->set)VoxelJsonAdd(json,",\"%s\":\"auto\"",key);
  else VoxelJsonAdd(json,",\"%s\":\"%u,%04X,%04X,%04X,%04X,%u,%u,%u\"",key,
      art->bg,art->words[0],art->words[1],art->words[2],art->words[3],art->x,art->y,art->size);
}
/* Hidden cells remain addressable through the native map. This is the same
 * shared projection and compensation used by authored C geometry, never a
 * second browser model. The outline is tagged visible=false when it is only
 * a coordinate selection guide rather than a submitted face. */
static bool VoxelCellPoint(float x,float y,float z,ArRenderPointF *point) {
  const DioramaProjection *p=&s_receiver_projection;
  const DioramaCapture *capture=EditorRoomScene_Capture(s_room);
  const DioramaPlaneProjection *plane=&p->bg1_high_plane;
  if(!plane->valid)plane=&p->bg1_plane;
  const float du=plane->u1-plane->u0,dv=plane->v1-plane->v0;
  if(!plane->valid||du==0||dv==0)return false;
  const float tx=x-(capture->bg1_camera_x-(capture->width-256)*.5f-capture->obj_apron);
  const float ty=y-(capture->camera_y-capture->authentic_y0+1);
  const float wx=((tx/SR_PPU_SURFACE_MAX_WIDTH-plane->u0)/du-.5f)*p->aspect_x;
  const float wy=(.5f-(ty/SR_PPU_SURFACE_MAX_HEIGHT-plane->v0)/dv)*p->height_scale;
  const float scale=1+z/fmaxf(1,p->camera_distance);
  Scene3DPoint q;
  if(!Scene3D_ProjectWorldPoint(p->matrix,wx*scale,wy*scale,-z,p->output_width,p->output_height,&q))return false;
  *point=(ArRenderPointF){q.x+s_voxel_view_x,q.y+s_voxel_view_y};return true;
}
static float VoxelCellDepth(const VoxelCellGeometry *cell,const VoxelAuthoredCell *authored,
    unsigned quadrant,float y,bool back) {
  float front=cell->front[quadrant],rear=cell->back[quadrant];
  if(authored)ActionVoxel_AuthoredDepths(authored,quadrant,y,&front,&rear);
  return back?rear:front;
}
static unsigned VoxelCoordinateOutline(int x,int y,const VoxelCellGeometry *cell,
    const VoxelAuthoredCell *authored,int kind,ArRenderPointF *points) {
  for(unsigned i=0;i<4;++i) {
    const bool right=i==1||i==2,bottom=i>=2;
    const float px=x*16+8+cell->offset_x+(right?.5f:-.5f)*cell->width;
    const float py=y*16+(bottom?16:0);
    const float z=VoxelCellDepth(cell,authored,(bottom?2:0)+(right?1:0),py,kind==kDioramaVoxelFace_Backing);
    float dx=0,dy=0,dz=0;
    if(kind!=kDioramaVoxelFace_Backing)
      ActionVoxel_BendOffset(&cell->bend,x*16+(right?16:0),py,&dx,&dy,&dz);
    float tx=px+dx,ty=py+dy,tz=z+dz;
    if(kind!=kDioramaVoxelFace_Backing)
      ActionVoxel_TransformPoint(&cell->transform,tx,ty,tz,&tx,&ty,&tz);
    if(!VoxelCellPoint(tx,ty,tz,&points[i]))return 0;
  }
  return 4;
}
static void VoxelDepthHandle(VoxelJson *json,int x,int y,const VoxelCellGeometry *cell,
    const VoxelAuthoredCell *authored,const DioramaVoxelCell *edit,bool back) {
  ArRenderPointF a,b;
  const float px=x*16.f+8+cell->offset_x,py=y*16.f+8;
  const float z=VoxelCellDepth(cell,authored,0,py,back);
  float next=z+.1f;
  if(authored) {
    VoxelAuthoredCell shifted=*authored;
    for(unsigned q=0;q<4;++q) {
      if(back) {
        shifted.back[q]+=.1f;
        if(!edit||!(edit->fields&kDioramaVoxelCell_BackEnd))shifted.back_end[q]+=.1f;
        if(!edit||!(edit->fields&kDioramaVoxelCell_FrontEnd))shifted.front_end[q]+=.1f;
      } else shifted.front[q]+=.1f;
    }
    next=VoxelCellDepth(cell,&shifted,0,py,back);
  }
  float dx,dy,dz;ActionVoxel_BendOffset(&cell->bend,x*16.f+8,py,&dx,&dy,&dz);
  float ax=px+dx,ay=py+dy,az=z+dz,bx=ax,by=ay,bz=next+dz;
  ActionVoxel_TransformPoint(&cell->transform,ax,ay,az,&ax,&ay,&az);
  ActionVoxel_TransformPoint(&cell->transform,bx,by,bz,&bx,&by,&bz);
  if(!VoxelCellPoint(ax,ay,az,&a)||!VoxelCellPoint(bx,by,bz,&b))return;
  if(hypotf(b.x-a.x,b.y-a.y)<=4){b.x=a.x+(back?18:-18);b.y=a.y-42;}
  VoxelJsonAdd(json,",\"%sDepthOrigin\":[%.4f,%.4f],\"%sDepthHandle\":[%.4f,%.4f]",
      back?"back":"front",a.x,a.y,back?"back":"front",b.x,b.y);
  if(!back)VoxelJsonAdd(json,",\"depthHandle\":[%.4f,%.4f],\"depthDelta\":0.1",b.x,b.y);
}
static unsigned VoxelTileJson(int x,int y,int kind,const DioramaVoxelPickFace *picked) {
  if(!s_room)return 0;
  const DioramaScene *scene=EditorRoomScene_Scene(s_room);
  VoxelCellGeometry cell;
  if(!ActionVoxel_CellGeometry(scene->voxel_geometry,x,y,&cell))return 0;
  const DioramaRoomOverride *room=EditorRoomScene_Terrain(s_room);
  const DioramaVoxelCell *edit=DioramaVoxelEdits_Cell(&room->voxel,x,y);
  const VoxelAuthoredCell *authored=ActionVoxel_AuthoredCellAt(scene->voxel_geometry,x,y);
  static const char *const names[]={"front","top","side","back","object","left","right","bottom"};
  static const char *const backing[]={"auto","none","fill","replace"};
  static const char *const shapes[]={"box","taper","curve"};
  const bool metadata_only=kind<0;
  if(metadata_only)kind=kDioramaVoxelFace_Front;
  VoxelOutline outline={0};ArRenderPointF coordinate[4];int priority=0;
  if(!metadata_only&&!VoxelTileOutline(x,y,kind,&outline,&priority)){free(outline.points);return 0;}
  ArRenderPointF *points=outline.points;unsigned count=outline.count;
  const bool visible=count>0;
  if(!metadata_only&&!count){points=coordinate;count=VoxelCoordinateOutline(x,y,&cell,authored,kind,points);}
  VoxelJson json={0};
  VoxelJsonAdd(&json,"{\"kind\":\"tile\",\"x\":%d,\"y\":%d,\"face\":\"%s\",\"priority\":%d,\"front\":%.6f,\"back\":%.6f,\"width\":%.4f,\"offset\":%.4f,\"edited\":%s,\"backing\":\"%s\",\"visible\":%s,\"faces\":%u,\"shape\":\"%s\",\"frontEnd\":%.6f,\"backEnd\":%.6f,\"shapeY\":%d,\"shapeH\":%u,",
      x,y,names[kind],priority,cell.front[0],cell.back[0],cell.width,cell.offset_x,edit&&edit->fields?"true":"false",
      backing[edit&&(edit->fields&kDioramaVoxelCell_Backing)?edit->backing:0],visible?"true":"false",
      cell.faces,shapes[cell.shape<=kDioramaVoxelShape_Curve?cell.shape:0],cell.front_end,cell.back_end,cell.shape_y,cell.shape_h);
  VoxelJsonPolygon(&json,points,count);
  free(outline.points);
  if(!metadata_only) {
    VoxelDepthHandle(&json,x,y,&cell,authored,edit,false);
    VoxelDepthHandle(&json,x,y,&cell,authored,edit,true);
  }
  if(edit)VoxelJsonAdd(&json,",\"region\":{\"x\":%d,\"y\":%d,\"w\":%u,\"h\":%u}",edit->x,edit->y,DioramaVoxelCell_Width(edit),DioramaVoxelCell_Height(edit));
  if(cell.bend.axis)VoxelJsonAdd(&json,",\"bend\":\"%s,%d,%d,%.9g,%.9g,%.9g\"",
      cell.bend.axis==kDioramaVoxelBend_X?"x":"y",cell.bend.anchor,cell.bend.tip,
      (double)cell.bend.dx,(double)cell.bend.dy,(double)cell.bend.dz);
  else VoxelJsonAdd(&json,",\"bend\":null");
  const DioramaVoxelTransform *transform=&cell.transform;
  if(transform->sx>0)VoxelJsonAdd(&json,",\"transform\":\"%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\"",
      (double)transform->px,(double)transform->py,(double)transform->pz,
      (double)transform->dx,(double)transform->dy,(double)transform->dz,
      (double)transform->sx,(double)transform->sy,(double)transform->sz);
  else VoxelJsonAdd(&json,",\"transform\":null");
  const char *transform_reason=ActionVoxel_TransformUnsupportedReason(scene->voxel_geometry,x,y);
  VoxelJsonAdd(&json,",\"transformSupported\":%s,\"nativeGeometry\":%s",
      transform_reason?"false":"true",authored&&authored->surface?"false":"true");
  if(transform_reason)VoxelJsonAdd(&json,",\"transformReason\":\"%s\"",transform_reason);
  VoxelJsonArt(&json,"frontArt",edit?&edit->front_art:NULL);
  VoxelJsonArt(&json,"topArt",edit?&edit->top_art:NULL);
  VoxelJsonArt(&json,"sideArt",edit?&edit->side_art:NULL);
  VoxelJsonArt(&json,"backArt",edit?&edit->backing_art:NULL);
  VoxelJsonArt(&json,"leftArt",edit?&edit->left_art:NULL);
  VoxelJsonArt(&json,"rightArt",edit?&edit->right_art:NULL);
  VoxelJsonArt(&json,"bottomArt",edit?&edit->bottom_art:NULL);
  VoxelJsonSampleSource(&json,x,y,kind,authored,picked,!metadata_only);
  VoxelJsonAdd(&json,"}");return VoxelJsonDone(&json);
}

static bool VoxelObjectQuad(const EditorObjectPreviewItem *item,DioramaVoxelPickFace *face) {
  const DioramaObjectSnapshotView *v=&s_objects.snapshot.view;
  const DioramaCapture *capture=EditorRoomScene_Capture(s_room);
  *face=(DioramaVoxelPickFace){.kind=kDioramaVoxelFace_Object,.owner=item->owner,
    .plane=DioramaPlaneForObjectPriority(item->priority),.priority=item->priority,.cell_x=-1,.cell_y=-1};
  for(int i=0;i<4;++i) {
    const float x=item->capture_x+((i==1||i==2)?item->width:0);
    const float y=item->capture_y+(i>=2?item->height:0);
    ArRenderPointF q;
    if(!Diorama_ProjectCapturedPoint(&s_receiver_projection,
        x+v->world_x-(s_x-s_extra),y+v->world_y-(s_y-capture->authentic_y0),item->priority,&q,NULL,NULL))return false;
    face->screen_xy[i*2]=q.x;face->screen_xy[i*2+1]=q.y;
    face->world_xy[i*2]=x+v->world_x;face->world_xy[i*2+1]=y+v->world_y;
    face->clip_w[i]=1;face->depth[i]=1;
  }
  return true;
}
static void VoxelObjectJson(VoxelJson *json,const EditorObjectPreviewItem *item) {
  ArRenderPointF points[128],hull[256];unsigned count=0;
  for(unsigned i=0;i<s_voxel_face_count&&count<124;++i) {
    const DioramaVoxelPickFace *f=&s_voxel_faces[i];
    if(f->kind!=kDioramaVoxelFace_Object||f->owner!=item->owner)continue;
    for(int j=0;j<4;++j)points[count++]=(ArRenderPointF){f->screen_xy[j*2],f->screen_xy[j*2+1]};
  }
  DioramaVoxelPickFace face;
  if(!count&&s_voxel_inspection!=kDioramaVoxelInspect_Backing&&item->visible&&VoxelObjectQuad(item,&face))for(int j=0;j<4;++j)
    points[count++]=(ArRenderPointF){face.screen_xy[j*2],face.screen_xy[j*2+1]};
  count=EditorVoxelPick_Hull(points,count,hull);
  static const char *const modes[]={"auto","flat","extruded"};
  const DioramaObjectSnapshotView *v=&s_objects.snapshot.view;
  VoxelJsonAdd(json,"{\"kind\":\"object\",\"index\":%u,\"source\":%u,\"parent\":%u,\"animation\":%u,\"priority\":%u,\"label\":\"%s\",\"x\":%.3f,\"y\":%.3f,\"width\":%.3f,\"height\":%.3f,\"mode\":\"%s\",\"offset\":%.6f,\"thickness\":%.6f,\"widthScale\":%.5f,\"canExtrude\":%s,\"captured\":true,",
      item->index,item->source,item->parent,item->animation,item->priority,item->player?"Native player":"Native object",
      item->capture_x+v->world_x,item->capture_y+v->world_y,item->width,item->height,modes[item->mode],
      item->offset,item->thickness,item->width_scale,item->can_extrude?"true":"false");
  VoxelJsonAdd(json,"\"active\":%s,\"supported\":%s,",item->active?"true":"false",
      item->source&&EditorRoomScene_Scene(s_room)->voxel_geometry?"true":"false");
  VoxelJsonPolygon(json,hull,count);VoxelJsonAdd(json,"}");
}
unsigned RoomPreview_VoxelObjects(void) {
  if(!s_room||!s_voxel_pick_valid||s_voxel_pick_overflow)return 0;
  VoxelJson json={0};VoxelJsonAdd(&json,"[");unsigned written=0;
  for(unsigned i=0;i<EditorObjectPreview_ItemCount(&s_objects);++i) {
    EditorObjectPreviewItem item;
    if(!EditorObjectPreview_Item(&s_objects,EditorRoomScene_Scene(s_room),i,&item))continue;
    if(written++)VoxelJsonAdd(&json,",");
    VoxelObjectJson(&json,&item);
  }
  VoxelJsonAdd(&json,"]");return VoxelJsonDone(&json);
}
unsigned RoomPreview_VoxelTile(int x,int y,int face) {
  const bool valid_face=face>=-1&&face<=kDioramaVoxelFace_Bottom&&face!=kDioramaVoxelFace_Object;
  return s_room&&EditorRoomScene_3DMode(s_room)==kAction3D_Voxel&&valid_face&&
      (face==-1||(s_voxel_pick_valid&&!s_voxel_pick_overflow))?VoxelTileJson(x,y,face,NULL):0;
}
unsigned RoomPreview_VoxelGroup(int x,int y) {
  if(!s_room || EditorRoomScene_3DMode(s_room)!=kAction3D_Voxel)return 0;
  const ActionVoxelGeometry *geometry=EditorRoomScene_Scene(s_room)->voxel_geometry;
  VoxelGroupCell cells[kVoxelSemanticGroupMaxCells];VoxelSemanticGroup group;
  const ActionVoxelGroupStatus status=ActionVoxel_SemanticGroup(geometry,x,y,&group,cells,kVoxelSemanticGroupMaxCells);
  VoxelJson json={0};
  if(status!=kActionVoxelGroup_Complete) {
    const char *reason=status==kActionVoxelGroup_Ambiguous
        ?"This continuous ground has no clear object boundary. Select a smaller area in Map and convert it before moving or resizing."
        :status==kActionVoxelGroup_TooLarge
        ?"This connected surface is too large to select as one object. Use Map selection or select smaller groups."
        :status==kActionVoxelGroup_Empty?"No connected classified object here. Use Tile or Map selection."
        :"Smart grouping is unavailable for this surface. Use its dedicated controls or Tile selection.";
    VoxelJsonAdd(&json,"{\"supported\":false,\"status\":\"%s\",\"reason\":\"%s\"}",
        status==kActionVoxelGroup_Ambiguous?"ambiguous":status==kActionVoxelGroup_TooLarge?"tooLarge":status==kActionVoxelGroup_Empty?"empty":"unsupported",reason);
    return VoxelJsonDone(&json);
  }
  float near=INFINITY,far=-INFINITY;const char *unsupported=NULL;
  for(unsigned i=0;i<group.count;++i) {
    VoxelCellGeometry cell;
    if(ActionVoxel_CellGeometry(geometry,cells[i].x,cells[i].y,&cell)) {
      for(unsigned q=0;q<4;++q){near=fminf(near,cell.front[q]);far=fmaxf(far,cell.back[q]);}
    }
    if(!unsupported)unsupported=ActionVoxel_TransformUnsupportedReason(geometry,cells[i].x,cells[i].y);
  }
  const float pivot_depth=isfinite(near)&&isfinite(far)?fmaxf(-2,fminf(2,(near+far)*.5f)):0;
  VoxelJsonAdd(&json,"{\"supported\":true,\"status\":\"ok\",\"kind\":%u,\"label\":\"%s\","
      "\"bounds\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},\"pivotDepth\":%.9g,\"transformSupported\":%s",
      group.kind,group.label,group.x,group.y,group.width,group.height,(double)pivot_depth,unsupported?"false":"true");
  if(unsupported)VoxelJsonAdd(&json,",\"transformReason\":\"%s\"",unsupported);
  VoxelJsonAdd(&json,",\"cells\":[");
  for(unsigned i=0;i<group.count;++i)VoxelJsonAdd(&json,"%s[%d,%d]",i?",":"",cells[i].x,cells[i].y);
  VoxelJsonAdd(&json,"]}");return VoxelJsonDone(&json);
}
/* A group has one pivot in native world space. Move handles alter the shared
 * translation after scale, so the projected unit is independent of object size.
 * Bounds describe source membership, not a second browser geometry model. */
unsigned RoomPreview_VoxelTransformHandles(float x,float y,float width,float height,
    float px,float py,float pz,float dx,float dy,float dz,float sx,float sy,float sz) {
  const DioramaVoxelTransform transform={px,py,pz,dx,dy,dz,sx,sy,sz};
  if(!s_room || EditorRoomScene_3DMode(s_room)!=kAction3D_Voxel || !s_voxel_pick_valid ||
      !isfinite(x)||!isfinite(y)||!isfinite(width)||!isfinite(height)||
      width<=0||height<=0||x < -8192||y < -8192||x+width>8192||y+height>8192||
      !ActionVoxel_TransformValid(&transform))return 0;
  float ax,ay,az;ActionVoxel_TransformPoint(&transform,px,py,pz,&ax,&ay,&az);
  ArRenderPointF origin;if(!VoxelCellPoint(ax,ay,az,&origin))return 0;
  VoxelJson json={0};VoxelJsonAdd(&json,"{\"supported\":true,\"pivot\":[%.4f,%.4f],\"handles\":[",origin.x,origin.y);
  const float steps[]={16,16,.1f};const char *names[]={"x","y","z"};
  for(unsigned i=0;i<3;++i) {
    ArRenderPointF end;
    if(!VoxelCellPoint(ax+(i==0?steps[i]:0),ay+(i==1?steps[i]:0),az+(i==2?steps[i]:0),&end))return 0;
    const bool fallback=hypotf(end.x-origin.x,end.y-origin.y)<4;
    if(fallback)end=(ArRenderPointF){origin.x+(i==0?42:i==1?0:-28),origin.y+(i==1?42:i==0?0:-42)};
    VoxelJsonAdd(&json,"%s{\"axis\":\"%s\",\"origin\":[%.4f,%.4f],\"end\":[%.4f,%.4f],\"step\":%.6f,\"fallback\":%s}",
        i?",":"",names[i],origin.x,origin.y,end.x,end.y,steps[i],fallback?"true":"false");
  }
  VoxelJsonAdd(&json,"]}");return VoxelJsonDone(&json);
}
/* The attached-pull gizmo uses the same native projection as submitted faces.
 * Its coordinates are selection boundaries in source artwork space. Sampling
 * just inside each boundary selects the limb's own material depth, rather than
 * the neighbouring trunk/empty cell. The displayed boundary itself is exact. */
static bool VoxelBendBoundary(float x,float y,float sample_x,float sample_y,
    const DioramaVoxelBend *bend,float *px,float *py,float *pz,DioramaVoxelTransform *transform) {
  const int cx=(int)floorf(sample_x/16),cy=(int)floorf(sample_y/16);
  const ActionVoxelGeometry *geometry=EditorRoomScene_Scene(s_room)->voxel_geometry;
  VoxelCellGeometry cell;if(!ActionVoxel_CellGeometry(geometry,cx,cy,&cell))return false;
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(geometry,cx,cy);
  const unsigned q=((sample_y-cy*16>=8)?2:0)+((sample_x-cx*16>=8)?1:0);
  float dx,dy,dz;ActionVoxel_BendOffset(bend,x,y,&dx,&dy,&dz);
  *px=cx*16+8+cell.offset_x+(x-cx*16-8)*cell.width/16+dx;
  *py=y+dy;*pz=VoxelCellDepth(&cell,a,q,y,false)+dz;
  ActionVoxel_TransformPoint(&cell.transform,*px,*py,*pz,px,py,pz);
  if(transform)*transform=cell.transform;
  return true;
}
unsigned RoomPreview_VoxelBendHandles(float x,float y,float width,float height,
    int axis,int anchor,int tip,float dx,float dy,float dz) {
  if(!s_room||EditorRoomScene_3DMode(s_room)!=kAction3D_Voxel||!s_voxel_pick_valid||
      !EditorRoomScene_Scene(s_room)->voxel_geometry||
      (axis!=kDioramaVoxelBend_X&&axis!=kDioramaVoxelBend_Y)||anchor==tip||
      anchor < -8192||anchor>8192||tip < -8192||tip>8192||
      !isfinite(x)||!isfinite(y)||!isfinite(width)||!isfinite(height)||
      width<=0||height<=0||x < -8192||y < -8192||x+width>8192||y+height>8192||
      !isfinite(dx)||!isfinite(dy)||!isfinite(dz)||fabsf(dx)>256||fabsf(dy)>256||fabsf(dz)>1.2f)return 0;
  const DioramaVoxelBend bend={.axis=axis,.anchor=anchor,.tip=tip,.dx=dx,.dy=dy,.dz=dz};
  const float inward=tip>anchor?.001f:-.001f;
  const float ax=axis==kDioramaVoxelBend_X?anchor:x+width*.5f;
  const float ay=axis==kDioramaVoxelBend_Y?anchor:y+height*.5f;
  const float tx=axis==kDioramaVoxelBend_X?tip:ax,ty=axis==kDioramaVoxelBend_Y?tip:ay;
  float apx,apy,apz,tpx,tpy,tpz;
  DioramaVoxelTransform transform={0};
  if(!VoxelBendBoundary(ax,ay,ax+(axis==1?inward:0),ay+(axis==2?inward:0),&bend,&apx,&apy,&apz,NULL)||
      !VoxelBendBoundary(tx,ty,tx-(axis==1?inward:0),ty-(axis==2?inward:0),&bend,&tpx,&tpy,&tpz,&transform))return 0;
  ArRenderPointF a,t;if(!VoxelCellPoint(apx,apy,apz,&a)||!VoxelCellPoint(tpx,tpy,tpz,&t))return 0;
  VoxelJson json={0};VoxelJsonAdd(&json,"{\"supported\":true,\"anchor\":[%.4f,%.4f],\"tip\":[%.4f,%.4f],\"handles\":[",a.x,a.y,t.x,t.y);
  const float step[]={16,16,.1f};const char *names[]={"x","y","z"};
  for(unsigned i=0;i<3;++i) {
    const float scale=transform.sx>0?(i==0?transform.sx:i==1?transform.sy:transform.sz):1;
    ArRenderPointF end;if(!VoxelCellPoint(tpx+(i==0?step[i]*scale:0),tpy+(i==1?step[i]*scale:0),tpz+(i==2?step[i]*scale:0),&end))return 0;
    const bool fallback=hypotf(end.x-t.x,end.y-t.y)<4;
    if(fallback)end=(ArRenderPointF){t.x+(i==0?42:i==1?0:-28),t.y+(i==1?42:i==0?0:-42)};
    VoxelJsonAdd(&json,"%s{\"axis\":\"%s\",\"origin\":[%.4f,%.4f],\"end\":[%.4f,%.4f],\"step\":%.6f,\"fallback\":%s}",i?",":"",names[i],t.x,t.y,end.x,end.y,step[i],fallback?"true":"false");
  }
  VoxelJsonAdd(&json,"]}");return VoxelJsonDone(&json);
}
unsigned RoomPreview_VoxelPick(float x,float y,int mask) {
  if(!s_room||!s_voxel_pick_valid||!isfinite(x)||!isfinite(y)||mask<1||mask>3)return 0;
  if(s_voxel_pick_overflow){VoxelJson j={0};VoxelJsonAdd(&j,"{\"supported\":false,\"reason\":\"Selection geometry exceeds the preview budget. Zoom into a smaller area.\"}");return VoxelJsonDone(&j);}
  const DioramaVoxelPickFace *best=NULL;EditorVoxelPickHit found={0};float depth=INFINITY;
  for(unsigned i=0;i<s_voxel_face_count;++i) {
    const DioramaVoxelPickFace *face=&s_voxel_faces[i];EditorVoxelPickHit hit;
    if(!(mask&(face->kind==kDioramaVoxelFace_Object?2:1))||!EditorVoxelPick_Hit(face,x,y,&hit)||!VoxelFaceInk(face,&hit,x,y))continue;
    if(face->kind==kDioramaVoxelFace_Object&&face->owner&&face->sample_alpha) {
      bool ink=false;
      for(unsigned k=0;k<EditorObjectPreview_ItemCount(&s_objects);++k) {
        EditorObjectPreviewItem item;
        if(EditorObjectPreview_Item(&s_objects,EditorRoomScene_Scene(s_room),k,&item)&&item.owner==face->owner&&
            EditorObjectPreview_ItemInk(&s_objects,k,(int)floorf(hit.world_x-s_objects.snapshot.view.world_x),
                (int)floorf(hit.world_y-s_objects.snapshot.view.world_y))){ink=true;break;}
      }
      if(!ink)continue;
    }
    const bool native_actor=face->kind==kDioramaVoxelFace_Object&&!face->owner;
    if(!native_actor&&hit.depth>depth)continue;
    best=face;found=hit;depth=native_actor?INFINITY:hit.depth;
  }
  if(best&&best->kind!=kDioramaVoxelFace_Object)
    return VoxelTileJson(best->authored||best->cell_x>=0?best->cell_x:(int)floorf(found.world_x/16),
        best->authored||best->cell_y>=0?best->cell_y:(int)floorf(found.world_y/16),best->kind,best);
  if((mask&2)&&s_voxel_inspection!=kDioramaVoxelInspect_Backing)for(unsigned i=0;i<EditorObjectPreview_ItemCount(&s_objects);++i) {
    EditorObjectPreviewItem item;DioramaVoxelPickFace face;EditorVoxelPickHit hit;
    if(!EditorObjectPreview_Item(&s_objects,EditorRoomScene_Scene(s_room),i,&item))continue;
    if(best) {
      if(best->owner ? best->owner!=item.owner :
          !EditorObjectPreview_ItemPixel(&s_objects,i,found.world_x-s_objects.snapshot.view.world_x,
              found.world_y-s_objects.snapshot.view.world_y))continue;
    } else {
      if(!item.visible||!VoxelObjectQuad(&item,&face)||!EditorVoxelPick_Hit(&face,x,y,&hit)||
          !EditorObjectPreview_ItemPixel(&s_objects,i,hit.world_x-s_objects.snapshot.view.world_x,
              hit.world_y-s_objects.snapshot.view.world_y))continue;
    }
    VoxelJson json={0};VoxelObjectJson(&json,&item);return VoxelJsonDone(&json);
  }
  if(!EditorRoomScene_Scene(s_room)->voxel_geometry) {
    VoxelJson j={0};VoxelJsonAdd(&j,"{\"supported\":false,\"reason\":\"This room uses the curved Tree enclosure. Use its Tree controls for geometry.\"}");return VoxelJsonDone(&j);
  }
  return 0;
}
