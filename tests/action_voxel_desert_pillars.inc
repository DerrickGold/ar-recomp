/* Exact displayed-art contract; IDs are absent from the eligibility oracle. */
static bool DesertPillarWords(const ActionRoomScene *s,const uint16_t w[4],unsigned collision) {
  static const uint64_t keys[]={UINT64_C(0x54514daec1abcef0),UINT64_C(0x8411c7075a7251e8),
    UINT64_C(0xb5e5ed801cf90280),UINT64_C(0xe63cc5b4a7d7283c)};
  for(unsigned flip=0;flip<4;++flip)for(unsigned k=0;k<4;++k)
    if(TombCapKey(s,w,collision,flip)==keys[k])return true;
  return false;
}
/* Independent native-map census: ground endpoints are colliding sand, while
 * the other supports terminate in pointed bases or the finite map edge. */
static bool DesertGroundedExpected(const ActionRoomScene *s,unsigned region,int x,int y) {
  if(s->map==1)return ((x==160 || x==161)&&y>=30&&y<=33) ||
      ((x==148 || x==149)&&y>=33&&y<=34);
  return ((x==102 || x==103)||(region==1&&(x==112 || x==113)))&&y>=41&&y<=43;
}
static float DesertGroundedFront(const ActionRoomScene *s,int x,float y) {
  const float top=s->map==1?(x>=160?480.f:528.f):656.f;
  const float t=fmaxf(0,fminf(1,(y-top)/8.f));return .08f*t*t*(3-2*t);
}
static uint64_t DesertPillarMix(uint64_t hash,const void *data,size_t size) {
  const uint8_t *p=data;for(size_t i=0;i<size;++i)hash=(hash^p[i])*UINT64_C(1099511628211);return hash;
}
static void DesertPillarCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool baseline,unsigned region,bool print) {
  uint64_t other_shapes=UINT64_C(14695981039346656037),other_pixels=other_shapes,native_flags=other_shapes;
  unsigned count=0,grounded_count=0,ground_join_pixels=0,joins=0,joined_pixels=0,side_spans=0;unsigned ne;const VoxelEdge *edges=ActionVoxel_Edges(g,&ne);
  const float depth=ActionVoxel_Depth(g);assert(fabsf(depth-.20f)<.000001f);
  for(int y=g->origin_y/16;y<(g->origin_y+g->height)/16;++y)for(int x=g->origin_x/16;x<(g->origin_x+g->width)/16;++x) {
    uint16_t w[4];unsigned collision;
    if(!TombCapWords(s,terrain,x,y,w,&collision))continue;
    const bool target=DesertPillarWords(s,w,collision);
    const bool grounded=target&&DesertGroundedExpected(s,region,x,y)&&!baseline&&!getenv("AR_DESERT_GROUNDED_CONTROL");
    VoxelCellGeometry shape;
    assert(ActionVoxel_CellGeometry(g,x,y,&shape));
    if(!target)other_shapes=DesertPillarMix(other_shapes,&shape,sizeof(shape));
    else {
      ++count;grounded_count+=grounded;assert(!collision&&!shape.authored);
      for(unsigned q=0;q<4;++q)assert(fabsf(shape.front[q]-(grounded?DesertGroundedFront(s,x,y*16+(q>>1)*8+4):0))<.000001f&&fabsf(shape.back[q]-(baseline?.05f:.20f))<.000001f);
    }
    uint32_t native[256],copy[256],rear[256],front[256];
    for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
      const unsigned q=(py/8)*2+px/8,word=w[q],ink=RockFixtureWordPixel(s,word,px&7,py&7);
      const size_t at=(size_t)(y*16+py-g->origin_y)*g->width+x*16+px-g->origin_x;
      const unsigned role=g->front[at];
      if(target) {
        assert(!(word&0x2000));
        assert((role&kVoxelKindMask)==(ink?(baseline?kVoxelForeground:kVoxelSolid):0));
        assert(!(role&(kVoxelHighPriority|kVoxelColliding)));
        const unsigned flags=role&~kVoxelKindMask;native_flags=DesertPillarMix(native_flags,&flags,sizeof(flags));
        const unsigned index=(((word>>10)&7)*16+ink)*2;
        const uint16_t color=s->palette[index]|s->palette[index+1]<<8;
        native[py*16+px]=ink?CastleBackingColor32(color,&(ActionRoomSceneFrameState){.brightness=15}):0;
      } else other_pixels=DesertPillarMix(other_pixels,&role,sizeof(role));
    }
    if(!target)continue;
    memcpy(copy,native,sizeof(copy));assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,
      &(ActionRoomSceneFrameState){.brightness=15},native,16,rear,front));
    assert(!memcmp(native,copy,sizeof(copy))&&!memcmp(front,native,sizeof(front)));
    uint16_t above[4];unsigned ac;
    if(TombCapWords(s,terrain,x,y-1,above,&ac)&&!DesertPillarWords(s,above,ac)) {
      VoxelCellGeometry cap;assert(ActionVoxel_CellGeometry(g,x,y-1,&cap));++joins;
      for(unsigned q=0;q<4;++q)assert(fabsf(cap.front[q])<.000001f&&fabsf(cap.back[q]-.20f)<.000001f);
      for(unsigned px=0;px<16;++px) {
        const unsigned own=RockFixtureWordPixel(s,w[px/8],px&7,0);
        const unsigned top=RockFixtureWordPixel(s,above[2+px/8],px&7,7);
        if(!own||!top)continue;++joined_pixels;
        for(unsigned i=0;i<ne;++i)if(!baseline && edges[i].far_rank==2 &&
            ((edges[i].side==0&&edges[i].y==y*16)||(edges[i].side==2&&edges[i].y==y*16-1))&&
            edges[i].x<=x*16+(int)px&&x*16+(int)px<edges[i].x+edges[i].length) {
          fprintf(stderr,"pillar unclosed cap join %d,%d px%u edge%d,%d,%d side%u near%u\n",x,y,px,edges[i].x,edges[i].y,edges[i].length,edges[i].side,edges[i].near_rank);assert(false);
        }
      }
    }
    uint16_t below[4];unsigned bc;
    if(grounded && TombCapWords(s,terrain,x,y+1,below,&bc) && !DesertPillarWords(s,below,bc)) {
      assert((bc&3)==3);
      for(unsigned px=0;px<16;++px) {
        const unsigned own=RockFixtureWordPixel(s,w[2+px/8],px&7,7);
        const unsigned ground=RockFixtureWordPixel(s,below[px/8],px&7,0);
        if(!own||!ground)continue;
        unsigned found=0;
        for(unsigned i=0;i<ne;++i)if(edges[i].side==0 && edges[i].y==(y+1)*16 &&
            edges[i].x<=x*16+(int)px && x*16+(int)px<edges[i].x+edges[i].length) {
          VoxelEdgeSpan span;assert(ActionVoxel_ResolveEdgeSpan(g,edges+i,kVoxelEdgeNative,-INFINITY,INFINITY,&span));
          /* Sand fills the clear front .00..08 path exactly; the rear
           * .08..20 is occupied by the column, with no gap or second shelf. */
          assert(fabsf(span.near_depth)<.000001f&&fabsf(span.far_depth-.08f)<.000001f);++found;
        }
        assert(found==1);++ground_join_pixels;
      }
    }
    for(unsigned i=0;i<ne;++i)if((edges[i].side&1)&&edges[i].near_rank==0&&
        edges[i].far_rank==(baseline?1:2)&&edges[i].x>=x*16&&edges[i].x<x*16+16&&
        edges[i].y>=y*16&&edges[i].y<y*16+16) {
      VoxelEdgeSpan span;assert(ActionVoxel_ResolveEdgeSpan(g,edges+i,kVoxelEdgeNative,-INFINITY,INFINITY,&span));
      assert(fabsf(span.near_depth-(grounded?DesertGroundedFront(s,x,edges[i].y):0))<.000001f&&fabsf(span.far_depth-(baseline?.05f:.2f))<.000001f);
      for(unsigned sample=0;sample<3;++sample) {
        const float y_at=edges[i].y+edges[i].length*sample*.5f;
        assert(ActionVoxel_ResolveEdgeSpanAtY(g,edges+i,kVoxelEdgeNative,-INFINITY,INFINITY,y_at,&span));
        const float want=grounded?DesertGroundedFront(s,x,y_at):0;
        assert(fabsf(span.near_depth-want)<.000001f&&fabsf(span.far_depth-(baseline?.05f:.2f))<.000001f);
      }
      ++side_spans;
    }
  }
  if(g->origin_x>=0) {
    assert(count==(s->map==1?12:region==1?210:208));
    assert(joins==(s->map==1?4:region==1?30:28));assert(joined_pixels&&side_spans);
    if(!baseline&&!getenv("AR_DESERT_GROUNDED_CONTROL"))assert(grounded_count==(s->map==1?12:region==1?12:6)&&ground_join_pixels==(s->map==1?64:region==1?64:32));
  } else assert(count==1);
  if(print) {
    fprintf(stderr,"PILLAR_INVARIANTS %u %u %016llx %016llx %016llx\n",region,s->map,(unsigned long long)other_shapes,(unsigned long long)other_pixels,(unsigned long long)native_flags);
    fprintf(stderr,"pillar region%u room%u cells%u capJoins%u joinedPixels%u sideSpans%u depth%.2f grounded%u\n",region,s->map,count,joins,joined_pixels,side_spans,baseline?.05f:.20f,grounded_count);
  }
}
static void DesertPillarEdits(const ActionRoomScene *s,const ActionVoxelGeometry *base,ActionVoxelGeometry *g,ActionVoxelSettings settings) {
  static DioramaRoomOverride t;const int x=160,y=30;VoxelCellGeometry cap,after;
  assert(ActionVoxel_CellGeometry(base,x,y-1,&cap));uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,x,y,words,&collision));
  memset(&t,0,sizeof(t));t.voxel.cell_count=1;DioramaVoxelCell *e=&t.voxel.cells[0];
  *e=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|kDioramaVoxelCell_Width,.front=.03f,.back=.11f,.width=9};
  assert(ActionVoxel_Build(g,s,&t,settings));assert(ActionVoxel_CellGeometry(g,x,y,&after)&&after.authored&&after.width==9);
  for(unsigned q=0;q<4;++q)assert(after.front[q]==.03f&&after.back[q]==.11f);
  assert(ActionVoxel_CellGeometry(g,x,y-1,&after)&&!memcmp(&cap,&after,sizeof(cap)));
  *e=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Faces,.faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front};
  assert(ActionVoxel_Build(g,s,&t,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,x,y);assert(a&&a->native_front_hidden);
  e->faces=kDioramaVoxelFaceMask_All;assert(ActionVoxel_Build(g,s,&t,settings));SameGeometry(base,g);
  *e=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Transform|kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.03f,.back=.11f,.transform={.px=x*16,.py=y*16,.pz=0,.dx=7,.dy=-4,.sx=1.25f,.sy=.75f,.sz=1}};
  assert(ActionVoxel_Build(g,s,&t,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);assert(a);
  float tx,ty,tz;ActionVoxel_TransformPoint(&a->transform,x*16+8,y*16+8,0,&tx,&ty,&tz);assert(tx==x*16+17&&ty==y*16+2&&tz==0);
  t.voxel.cell_count=2;t.voxel.cells[1]=(DioramaVoxelCell){.x=x,.y=y};assert(ActionVoxel_Build(g,s,&t,settings));SameGeometry(base,g);
  for(unsigned flip=0;flip<2;++flip) {
    memset(&t,0,sizeof(t));DioramaTileStamp stamp={.x=-2,.y=-3};
    for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,x*2+((q&1)^flip),y*2+(q>>1),&stamp.words[q],&stamp.metatile));
    if(flip)for(unsigned q=0;q<4;++q)stamp.words[q]^=0x4000;
    t.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-3,.x1=-1,.y1=-2,.cells=&stamp,.count=1};
    assert(ActionVoxel_Build(g,s,&t,settings));DesertPillarCheck(s,&t,g,false,0,false);
  }
}
static void DesertGroundedStampControls(const ActionRoomScene *s,ActionVoxelGeometry *g,ActionVoxelSettings settings) {
  static DioramaRoomOverride t;DioramaTileStamp stamps[8];VoxelCellGeometry shape;
  memset(&t,0,sizeof(t));
  for(unsigned row=0;row<4;++row)for(unsigned col=0;col<2;++col) {
    const unsigned i=row*2+col;stamps[i]=(DioramaTileStamp){.x=(int)col-2,.y=(int)row-4};
    const int sy=(int[]){29,30,33,34}[row];
    for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,(160+col)*2+(q&1),sy*2+(q>>1),&stamps[i].words[q],&stamps[i].metatile));
  }
  t.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-4,.x1=0,.y1=0,.cells=stamps,.count=8};
  for(unsigned mode=0;mode<4;++mode) {
    DioramaTileStamp ground[2]={stamps[6],stamps[7]};
    if(mode==1)stamps[6].blank=stamps[7].blank=true;
    if(mode==2)for(unsigned col=0;col<2;++col)for(unsigned q=0;q<4;++q)
      assert(ActionRoomScene_LookupTile(s,1,(160+col)*2+(q&1),26*2+(q>>1),&stamps[6+col].words[q],&stamps[6+col].metatile));
    if(mode==3)stamps[6].blank=true; /* Partial support rejects the whole component. */
    assert(ActionVoxel_Build(g,s,&t,settings));
    for(unsigned col=0;col<2;++col)for(unsigned row=0;row<2;++row) {
      assert(ActionVoxel_CellGeometry(g,(int)col-2,(int)row-3,&shape));
      for(unsigned q=0;q<4;++q)assert(fabsf(shape.front[q]-(mode?0:row||q>=2?.08f:.04f))<.000001f&&fabsf(shape.back[q]-.2f)<.000001f);
    }
    for(unsigned col=0;col<2;++col) {
      assert(ActionVoxel_CellGeometry(g,(int)col-2,-4,&shape));
      for(unsigned q=0;q<4;++q)assert(shape.front[q]==0&&fabsf(shape.back[q]-.2f)<.000001f);
    }
    stamps[6]=ground[0];stamps[7]=ground[1];
  }
  /* Explicit native editor depth remains authoritative on the same grounded object. */
  t.voxel.cell_count=1;t.voxel.cells[0]=(DioramaVoxelCell){.x=-2,.y=-2,
      .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.02f,.back=.12f};
  assert(ActionVoxel_Build(g,s,&t,settings));assert(ActionVoxel_CellGeometry(g,-2,-2,&shape)&&shape.authored);
  for(unsigned q=0;q<4;++q)assert(shape.front[q]==.02f&&shape.back[q]==.12f);
  assert(ActionVoxel_CellGeometry(g,-1,-2,&shape));for(unsigned q=0;q<4;++q)assert(fabsf(shape.front[q]-.08f)<.000001f);
  t.voxel.cell_count=0;settings.depth=.3f;settings.play_recess=.07f;
  assert(ActionVoxel_Build(g,s,&t,settings));assert(ActionVoxel_CellGeometry(g,-2,-2,&shape));
  for(unsigned q=0;q<4;++q)assert(fabsf(shape.front[q]-.085f)<.000001f&&fabsf(shape.back[q]-.3f)<.000001f);
}
static void DesertPillarFixtures(void) {
  const char *root=getenv("AR_DESERT_PILLAR_SCENES");assert(root);const bool baseline=getenv("AR_DESERT_PILLAR_BASELINE")!=NULL;
  static ActionSceneSnapshot snapshot;static ActionRoomScene remap;static DioramaRoomOverride terrain;static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=2;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/030%u.arscene",root,region,room);FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,n,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;uint64_t source=ForestBackingHash(s,sizeof(*s));ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(3,room);assert(ActionVoxel_Build(g,s,NULL,settings));DesertPillarCheck(s,NULL,g,baseline,region,true);
    if(!baseline) {
      remap=*s;Renumber(&remap);assert(ActionVoxel_Build(other,&remap,NULL,settings));DesertPillarCheck(&remap,NULL,other,false,region,false);assert(TombCapGeometryHash(g)==TombCapGeometryHash(other));
      TombCapTerrain(3,room,region,&terrain);assert(ActionVoxel_Build(other,s,&terrain,settings));DesertPillarCheck(s,&terrain,other,false,region,false);
      if(room==1 && !getenv("AR_DESERT_GROUNDED_CONTROL")){DesertPillarEdits(s,g,other,settings);DesertGroundedStampControls(s,other,settings);}
    }
    assert(source==ForestBackingHash(s,sizeof(*s)));ActionVoxel_Release(g);ActionVoxel_Release(other);
  }
}
