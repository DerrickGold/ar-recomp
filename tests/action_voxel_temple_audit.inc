/* Reviewed native temple matte contract. Numeric IDs below locate fixture art
 * only; the production classification continues to use displayed-art keys. */
typedef struct TempleAuditEdgePixel {
  int x,y;
  const VoxelEdge *edge;
} TempleAuditEdgePixel;
static int TempleAuditEdgeOrder(const void *aa,const void *bb) {
  const TempleAuditEdgePixel *a=aa,*b=bb;
  if(a->y!=b->y)return a->y<b->y?-1:1;
  if(a->x!=b->x)return a->x<b->x?-1:1;
  return (int)a->edge->side-(int)b->edge->side;
}
static TempleAuditEdgePixel *TempleAuditEdgePixels(const ActionVoxelGeometry *g,unsigned *count) {
  unsigned n;const VoxelEdge *edges=ActionVoxel_Edges(g,&n);*count=0;
  for(unsigned i=0;i<n;++i){assert(edges[i].length>0);*count+=(unsigned)edges[i].length;}
  TempleAuditEdgePixel *pixels=malloc((size_t)*count*sizeof(*pixels));assert(pixels);
  unsigned at=0;
  for(unsigned i=0;i<n;++i)for(int j=0;j<edges[i].length;++j)
    pixels[at++]=(TempleAuditEdgePixel){edges[i].x+((edges[i].side&1)?0:j),
        edges[i].y+((edges[i].side&1)?j:0),&edges[i]};
  assert(at==*count);qsort(pixels,*count,sizeof(*pixels),TempleAuditEdgeOrder);
  for(unsigned i=1;i<*count;++i)assert(TempleAuditEdgeOrder(&pixels[i-1],&pixels[i])<0);
  return pixels;
}
static bool TempleAuditRaisedAt(const ActionRoomScene *source,int x,int y) {
  uint16_t word;uint8_t id;
  if(x<0 || y<0 || !ActionRoomScene_LookupTile(source,1,x/8,y/8,&word,&id))return false;
  return id==0x42 || id==0x43 || id==0x73 || id==0x74;
}
static void TempleAuditContours(const ActionVoxelGeometry *fixed,
    const ActionVoxelGeometry *tower,const ActionRoomScene *source) {
  /* The Tower reference now deliberately raises four column materials. Its
   * contour merges and column/arch boundaries therefore differ, but no old
   * contour may disappear and no unrelated geometry may change. Compare
   * native-pixel segments instead of requiring identical merged arrays. */
  unsigned a_count,b_count;
  TempleAuditEdgePixel *a=TempleAuditEdgePixels(fixed,&a_count),
      *b=TempleAuditEdgePixels(tower,&b_count);
  const int dx[]={0,1,0,-1},dy[]={-1,0,1,0};
  const float depth=ActionVoxel_Depth(fixed),lift=depth*.125f;
  unsigned ai=0,bi=0,shared=0,added=0;
  while(ai<a_count || bi<b_count) {
    assert(bi<b_count); /* No original contour may be removed. */
    const TempleAuditEdgePixel *bp=&b[bi];const VoxelEdge *be=bp->edge;
    const bool owner=TempleAuditRaisedAt(source,bp->x,bp->y),
        neighbor=TempleAuditRaisedAt(source,bp->x+dx[be->side],bp->y+dy[be->side]);
    assert(be->raised_scenery==(owner && be->far_rank==1));
    assert(be->neighbor_raised_scenery==(neighbor && be->near_rank==1));
    const int order=ai<a_count?TempleAuditEdgeOrder(&a[ai],bp):1;
    assert(order>=0);
    if(order>0) {
      /* The only additional wall is the .03 lip where a raised column
       * projects in front of ordinary rear architecture. */
      assert(be->raised_scenery && !be->neighbor_raised_scenery &&
          be->far_rank==1 && be->near_rank==1 && !be->colliding && !be->high_priority);
      VoxelEdgeSpan span;
      assert(ActionVoxel_ResolveEdgeSpan(tower,be,kVoxelEdgeNative,0,depth,&span));
      assert(fabsf(span.near_depth-depth*.625f)<.00001f &&
          fabsf(span.far_depth-depth*.75f)<.00001f);
      ++added;++bi;continue;
    }
    const VoxelEdge *ae=a[ai].edge;assert(!ae->raised_scenery && !ae->neighbor_raised_scenery);
    VoxelEdgeSpan before,after;
    const bool have_before=ActionVoxel_ResolveEdgeSpan(fixed,ae,kVoxelEdgeNative,0,depth,&before),
        have_after=ActionVoxel_ResolveEdgeSpan(tower,be,kVoxelEdgeNative,0,depth,&after);
    assert(have_before==have_after);
    if(have_before) {
      const float near=before.near_depth-(be->raised_scenery?lift:0),
          far=be->neighbor_raised_scenery && be->far_rank>=2?
              fminf(before.far_depth,depth*.625f):before.far_depth;
      assert(fabsf(after.near_depth-near)<.00001f && fabsf(after.far_depth-far)<.00001f);
    }
    VoxelEdge ac=*ae,bc=*be;
    ac.x=bc.x=(short)bp->x;ac.y=bc.y=(short)bp->y;ac.length=bc.length=1;
    /* Cached mount flags now express the same physical plane explicitly;
     * resolved spans above check their semantics. New owned-art indices may
     * shift after the additional boundaries, so compare their pixels. */
    ac.rear_mounted=bc.rear_mounted=ac.neighbor_rear_mounted=bc.neighbor_rear_mounted=0;
    ac.raised_scenery=bc.raised_scenery=ac.neighbor_raised_scenery=bc.neighbor_raised_scenery=0;
    assert(!!ac.top_owned_art==!!bc.top_owned_art);
    if(ac.top_owned_art)assert(!memcmp(ActionVoxel_AuthoredArt(fixed,ac.top_owned_art-1),
        ActionVoxel_AuthoredArt(tower,bc.top_owned_art-1),256*sizeof(uint16_t)));
    ac.top_owned_art=bc.top_owned_art=0;
    assert(!memcmp(&ac,&bc,sizeof(ac)));++shared;++ai;++bi;
  }
  assert(shared==a_count && shared+added==b_count);free(a);free(b);
}
static void TempleAuditFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene tower_reference;
  static DioramaRoomOverride terrain;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/0103-%u.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));
    const ActionRoomScene *source=&fixture.scene;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,3);
    ActionVoxelGeometry *fixed=ActionVoxel_Create(),*reference=ActionVoxel_Create(),*edited=ActionVoxel_Create();
    assert(fixed&&reference&&edited);
    assert(ActionVoxel_Build(fixed,source,NULL,settings));
    /* Tower still supplies the previous matte masks. Its four raised column
     * materials are independently accounted for in the contour comparison;
     * only indoor0103 owns the repaired backing role. */
    tower_reference=*source;tower_reference.map=4;
    assert(ActionVoxel_Build(reference,&tower_reference,NULL,settings));
    unsigned a,b;
    TempleAuditContours(fixed,reference,source);
    const VoxelPatch *pa=ActionVoxel_Patches(fixed,&a),*pb=ActionVoxel_Patches(reference,&b);
    assert(a==b && !memcmp(pa,pb,a*sizeof(*pa)));
    unsigned checked=0;
    const unsigned artwork[]={0x33,0x5f}; /* Column shoulder; carved capital corner. */
    for(unsigned art=0;art<sizeof(artwork)/sizeof(*artwork);++art) {
      int tx,ty;assert(FixtureCell(source,artwork[art],&tx,&ty));
      uint16_t matte[16]={0};
      for(unsigned q=0;q<4;++q) {
        uint8_t old[8]={0},now[8]={0};
        ActionVoxel_CutoutRows(reference,tx+(q&1),ty+(q>>1),old);
        assert(!ActionVoxel_CutoutRows(fixed,tx+(q&1),ty+(q>>1),now));
        for(unsigned y=0;y<8;++y) {
          assert(!now[y]);
          matte[(q>>1)*8+y]|=(uint16_t)old[y]<<((q&1)?0:8);
        }
      }
      uint32_t original[256],cut[256],rear[256],front[256],old_rear[256],old_front[256];
      unsigned first=256,interior=0;
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned at=y*16+x,pixel=FixturePixel(source,tx*8+x,ty*8+y);
        original[at]=pixel?UINT32_C(0xff030507):0;
        cut[at]=(matte[y]&(1u<<(15-x)))?0:original[at];
        if(original[at] && !cut[at]) {if(first==256)first=at;++checked;}
        if(cut[at])++interior;
      }
      assert(first<256 && interior);
      for(unsigned dim=0;dim<2;++dim) {
        const ActionRoomSceneFrameState frame={.brightness=dim?2:15,.cgadsub=0x81,.fixed_color=0x421};
        assert(ActionVoxel_Split(fixed,tx*8,ty*8,16,16,16,&frame,original,16,rear,front));
        assert(ActionVoxel_Split(reference,tx*8,ty*8,16,16,16,&frame,cut,16,old_rear,old_front));
        /* Carved faces/enclosed shadows keep their exact native foreground.
         * Exterior matte retains its source colour only at the rear plane. */
        assert(!memcmp(front,old_front,sizeof(front)));
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
          const unsigned at=y*16+x;
          if(matte[y]&(1u<<(15-x)))assert(rear[at]==original[at] && !front[at]);
          else assert(rear[at]==old_rear[at]);
        }
      }
      const ActionRoomSceneFrameState frame={.brightness=15};
      /* Missing/native-transparent input stays transparent; no invented wall. */
      original[first]=0;
      assert(ActionVoxel_Split(fixed,tx*8,ty*8,16,16,16,&frame,original,16,rear,front));
      assert(!rear[first] && !front[first]);
      original[first]=UINT32_C(0xff000000);
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
      DioramaPixelEdit *paint=&terrain.pixel_layers[0].edits[0];
      *paint=(DioramaPixelEdit){.by_cell=true,.x=tx/2,.y=ty/2};
      paint->black[first/16]=(uint16_t)(1u<<(15-first%16));
      assert(ActionVoxel_Build(edited,source,&terrain,settings));
      assert(ActionVoxel_Split(edited,tx*8,ty*8,16,16,16,&frame,original,16,rear,front));
      assert(front[first]==original[first]); /* Explicit opaque paint wins. */
      paint->black[first/16]=0;
      paint->transparent[first/16]=(uint16_t)(1u<<(15-first%16));
      assert(ActionVoxel_Build(edited,source,&terrain,settings));
      original[first]=0; /* Native source rasterizer applies authored transparency. */
      assert(ActionVoxel_Split(edited,tx*8,ty*8,16,16,16,&frame,original,16,rear,front));
      assert(!rear[first] && !front[first]);
    }
    /* Blue spikes use independent silhouette rules, including their black
     * exterior: never convert the hazard's empty outline into a backstop. */
    const unsigned spikes[]={0x18,0x19,0x20};
    unsigned spike_masks=0;
    for(unsigned i=0;i<sizeof(spikes)/sizeof(*spikes);++i) {
      int tx,ty;if(!FixtureCell(source,spikes[i],&tx,&ty))continue;
      for(unsigned q=0;q<4;++q) {
        uint8_t old[8]={0},now[8]={0};
        ActionVoxel_CutoutRows(reference,tx+(q&1),ty+(q>>1),old);
        spike_masks+=ActionVoxel_CutoutRows(fixed,tx+(q&1),ty+(q>>1),now);
        assert(!memcmp(old,now,8));
      }
    }
    assert(checked && spike_masks);
    printf("temple matte terrain %u: %u native rear pixels, preserved contours/front art/spikes, tower relief accounted\n",region,checked);
    ActionVoxel_Release(fixed);ActionVoxel_Release(reference);ActionVoxel_Release(edited);
  }
}
