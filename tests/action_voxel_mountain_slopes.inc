/* Native $00:9084/$90A9 interprets collision6/9 as diagonal surfaces, not
 * quadrant blocks. A same-slope neighbor selects a half-grade segment;
 * isolated segments retain the full native grade. These IDs locate the
 * reviewed source artwork only; production uses semantic fingerprints. */
static bool MountainSlopeId(unsigned id) {
  return id==0x32 || id==0x41 || id==0x4b || id==0x58 || id==0x59 ||
      id==0x71 || id==0x80 || id==0x89 || id==0x8a;
}
static bool MountainSlopeFootId(unsigned id) {
  return id==0x31 || id==0x60 || id==0x61 || id==0x62 || id==0x63 || id==0x6a ||
      id==0x6b || id==0x78 || id==0x79 || id==0x88;
}
static unsigned MountainSlopeCollision(const ActionRoomScene *s,int x,int y) {
  uint16_t word;uint8_t id;
  if(!ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id))return 0;
  unsigned result=0;
  for(unsigned q=0;q<4;++q)result|=!!(s->bg[0].metatiles[id*8+q*2]&2)<<q;
  return result;
}
static int MountainSlopeContact(unsigned collision,unsigned left,unsigned right,unsigned x) {
  assert(collision==6 || collision==9);
  unsigned height=(collision==6?x:~x)&14;
  if((collision==6?left:right)==collision)height=height/2+8;
  else if((collision==6?right:left)==collision)height/=2;
  return 16-(int)height;
}
static void MountainSlopePixels(const ActionVoxelGeometry *g,int cx,int cy,
    unsigned collision,unsigned left,unsigned right) {
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned at=(cy*16+(int)y-g->origin_y)*g->width+cx*16+(int)x-g->origin_x;
    const unsigned pixel=g->front[at];if(!pixel)continue;
    const unsigned expected=(int)y<MountainSlopeContact(collision,left,right,x)?kVoxelRearOnly:kVoxelSolid;
    assert((pixel&kVoxelKindMask)==expected);
    assert(!!(pixel&kVoxelColliding)==!!(collision&(1u<<((y/8)*2+x/8))));
    assert(!(pixel&kVoxelHighPriority));
  }
}
static void MountainRampSpanCases(void) {
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  g->ready=true;g->profile=kActionVoxelProfile_AitosMountains;
  g->settings.depth=.2f;g->settings.play_recess=.08f;g->column_curve_count=2;
  for(unsigned owner=0;owner<2;++owner) {
    VoxelTrunkCurve *curve=g->column_curves+owner;
    *curve=(VoxelTrunkCurve){.contact_count=8,.linear=1};
    for(unsigned x=0;x<8;++x)curve->contact[x]=97-owner;
  }
  const VoxelEdge edge={.x=2,.y=100,.length=13,.side=3,.near_rank=2,.far_rank=2,
      .column_curve=1,.neighbor_column_curve=2,.high_priority=1,.colliding=1};
  VoxelEdgeSpan span;
  /* All three former samples miss this real front-band seam. */
  for(unsigned i=0;i<3;++i)
    assert(!ActionVoxel_ResolveEdgeSpanAtY(g,&edge,kVoxelEdgeNative,0,.07f,100+6.5f*i,&span));
  assert(ActionVoxel_ResolveEdgeSpanAtY(g,&edge,kVoxelEdgeNative,0,.07f,102,&span));
  assert(ActionVoxel_ResolveEdgeSpan(g,&edge,kVoxelEdgeNative,0,.07f,&span));
  assert(fabsf(span.near_depth)<.000001f && fabsf(span.far_depth-.07f)<.000001f);
  assert(ActionVoxel_ResolveEdgeSpan(g,&edge,kVoxelEdgeNative,.001f,.002f,&span));
  assert(fabsf(span.near_depth-.001f)<.000001f && fabsf(span.far_depth-.002f)<.000001f);
  g->column_curves[1]=g->column_curves[0];
  assert(!ActionVoxel_ResolveEdgeSpan(g,&edge,kVoxelEdgeNative,0,.07f,&span));
  /* A clipped empty endpoint belongs at the owner's front plane. Returning
   * the rear plane here connects the following real seam with a false wedge. */
  g->settings.depth=.24f;
  for(unsigned owner=0;owner<2;++owner)for(unsigned x=0;x<8;++x)
    g->column_curves[owner].contact[x]=99-owner;
  assert(!ActionVoxel_ResolveEdgeSpanAtY(g,&edge,kVoxelEdgeNative,0,.07f,102,&span));
  assert(span.near_depth==0 && span.far_depth==0);
  assert(ActionVoxel_ResolveEdgeSpanAtY(g,&edge,kVoxelEdgeNative,0,.07f,103,&span));
  assert(span.near_depth==0 && fabsf(span.far_depth-.02f)<.000001f);
  assert(!ActionVoxel_ResolveEdgeSpanAtY(g,&edge,kVoxelEdgeNative,.07f,.24f,102,&span));
  assert(fabsf(span.near_depth-.07f)<.000001f && span.far_depth==0);
  /* Ordinary column contracts retain their established empty-span result. */
  for(unsigned owner=0;owner<2;++owner)
    g->column_curves[owner]=(VoxelTrunkCurve){.top_y=103-owner,.base_y=115-owner};
  assert(!ActionVoxel_ResolveEdgeSpanAtY(g,&edge,kVoxelEdgeNative,0,.07f,102,&span));
  assert(fabsf(span.near_depth-.24f)<.000001f);
  ActionVoxel_Release(g);
}
static void MountainSlopeFixtures(const char *directory) {
  if(!directory)return;
  MountainRampSpanCases();
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0401.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(bytes,1,sizeof(bytes),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));original=snapshot.scene;
    const ActionRoomScene *s=&snapshot.scene;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,1);
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    assert(ActionVoxel_Build(base,s,NULL,settings));
    assert(base->column_curve_count==40);
    for(unsigned i=0;i<base->column_curve_count;++i)
      assert(base->column_curves[i].contact_count==8 && base->column_curves[i].linear);
    uint8_t *tops=calloc((size_t)base->width,base->height);assert(tops);
    unsigned edge_count;const VoxelEdge *edges=ActionVoxel_Edges(base,&edge_count);
    assert(edge_count<kVoxelMaxEdges);
    for(unsigned i=0;i<edge_count;++i) {
      const VoxelEdge *e=&edges[i];
      if(!e->side && e->far_rank==2) {
        uint16_t word;uint8_t id;
        assert(ActionRoomScene_LookupTile(s,1,e->x/8,e->y/8,&word,&id));
        if(MountainSlopeId(id) || MountainSlopeFootId(id)) {
          VoxelEdgeSpan span;
          assert(ActionVoxel_ResolveEdgeSpan(base,e,kVoxelEdgeNative,0,ActionVoxel_Depth(base),&span));
          assert(fabsf(span.near_depth)<.000001f && fabsf(span.far_depth-ActionVoxel_Depth(base))<.000001f);
        }
        for(unsigned px=0;px<e->length;++px)
          tops[(e->y-base->origin_y)*base->width+e->x+px-base->origin_x]=1;
      }
      if(!e->surface_texture)continue;
      /* Opaque rear cliff ink above a ramp is not a donor for its floor. */
      for(unsigned py=0;py<e->top_sample_size;++py)for(unsigned px=0;px<e->top_sample_size;++px) {
        const int x=e->top_sample_x+px,y=e->top_sample_y+py;uint16_t word;uint8_t id;
        assert(ActionRoomScene_LookupTile(s,1,x/8,y/8,&word,&id));
        if(!MountainSlopeId(id))continue;
        assert((base->front[(y-base->origin_y)*base->width+x-base->origin_x]&kVoxelKindMask)==kVoxelSolid);
      }
    }
    unsigned cells=0,ascending=0,descending=0,contact_pixels=0,rear_pixels=0,endpoint_pixels=0,support_pixels=0;
    const ActionRoomSceneFrameState frame={.brightness=15};
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
        uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
        if(MountainSlopeFootId(id)) {
          uint16_t above_word;uint8_t above_id;
          const bool above=y && ActionRoomScene_LookupTile(s,1,x*2,(y-1)*2,&above_word,&above_id) && MountainSlopeId(above_id);
          const unsigned c=above?MountainSlopeCollision(s,x,y-1):0;
          const unsigned l=above?MountainSlopeCollision(s,(int)x-1,y-1):0;
          const unsigned r=above?MountainSlopeCollision(s,x+1,y-1):0;
          uint32_t native[256],rear[256],front[256];
          for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
            native[py*16+px]=ForestNativeColor(s,x*16+px,y*16+py,&frame);
          assert(ActionVoxel_Split(base,x*16,y*16,16,16,16,&frame,native,16,rear,front));
          for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
            const unsigned at=(y*16+py-base->origin_y)*base->width+x*16+px-base->origin_x;
            const bool endpoint=above && !py && MountainSlopeContact(c,l,r,px)==16;
            const bool support=above && (int)py<MountainSlopeContact(c,l,r,px);
            assert((base->front[at]&kVoxelKindMask)==(support?kVoxelSolid:kVoxelRearOnly));
            assert(!(base->front[at]&(kVoxelColliding|kVoxelHighPriority)));
            assert(rear[py*16+px]==native[py*16+px]);
            assert(front[py*16+px]==(support?native[py*16+px]:0));
            endpoint_pixels+=endpoint;support_pixels+=support;
          }
        }
        if(!MountainSlopeId(id))continue;
        const unsigned collision=MountainSlopeCollision(s,x,y),left=MountainSlopeCollision(s,(int)x-1,y),
            right=MountainSlopeCollision(s,x+1,y);
        ++cells;ascending+=collision==6;descending+=collision==9;
        MountainSlopePixels(base,x,y,collision,left,right);
        uint32_t native[256],rear[256],front[256];
        for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
          native[py*16+px]=ForestNativeColor(s,x*16+px,y*16+py,&frame);
        assert(ActionVoxel_Split(base,x*16,y*16,16,16,16,&frame,native,16,rear,front));
        for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
          const bool backing=(int)py<MountainSlopeContact(collision,left,right,px);
          assert(rear[py*16+px]==native[py*16+px]);
          assert(front[py*16+px]==(backing?0:native[py*16+px]));rear_pixels+=backing;
        }
        for(unsigned px=0;px<16;++px) {
          const unsigned py=MountainSlopeContact(collision,left,right,px);
          const size_t at=(y*16+py-base->origin_y)*base->width+x*16+px-base->origin_x;
          /* The contact may land on the next native row at the low end. */
          if((base->front[at]&kVoxelKindMask)!=kVoxelSolid || !tops[at])
            fprintf(stderr,"ramp contact region%u cell%u,%u id%02x collision%u left%u right%u pixel%u,%u role%u top%u\n",
                region,x,y,id,collision,left,right,px,py,base->front[at]&kVoxelKindMask,tops[at]);
          assert((base->front[at]&kVoxelKindMask)==kVoxelSolid && tops[at]);++contact_pixels;
          /* A reviewed rear support continues the ledge to exactly16px
           * below contact, irrespective of the enclosing metatile row. */
          uint16_t below_word;uint8_t below_id;
          if(ActionRoomScene_LookupTile(s,1,x*2,(y+1)*2,&below_word,&below_id) && MountainSlopeFootId(below_id)) {
            const VoxelTrunkCurve *curve=ActionVoxel_ColumnCurveForCell(base,(x*16+px)/8,(y*16+py)/8);
            assert(curve && curve->contact_count==8 && curve->linear && curve->contact_x==(int)x*16);
            VoxelTrunkCurve section;ActionVoxel_ColumnSection(curve,x*16+px+.5f,&section);
            assert(!section.contact_count && section.linear && section.top_y==(int)(y*16+py+4) &&
                section.base_y==(int)(y*16+py+16));
            const float depth=ActionVoxel_Depth(base),contact=y*16+py;
            for(unsigned lip=0;lip<=4;++lip)assert(ActionVoxel_ColumnFrontDepth(base,&section,contact+lip)==0);
            assert(fabsf(ActionVoxel_ColumnFrontDepth(base,&section,contact+7)-depth*.25f)<.000001f);
            assert(fabsf(ActionVoxel_ColumnFrontDepth(base,&section,contact+10)-depth*.5f)<.000001f);
            assert(fabsf(ActionVoxel_ColumnFrontDepth(base,&section,contact+16)-depth)<.000001f);
            assert(base->semantic_cells[(y-base->origin_y/16)*(base->width/16)+x-base->origin_x/16]==kVoxelSemantic_Platform);
            assert(base->semantic_cells[(y+1-base->origin_y/16)*(base->width/16)+x-base->origin_x/16]==kVoxelSemantic_Platform);
            for(unsigned depth=0;depth<16;++depth) {
              if((base->front[at+depth*base->width]&kVoxelKindMask)!=kVoxelSolid)
                fprintf(stderr,"ramp body gap region%u cell%u,%u id%02x below%02x contact%u,%u depth%u kind%u\n",
                    region,x,y,id,below_id,px,py,depth,base->front[at+depth*base->width]&kVoxelKindMask);
              assert((base->front[at+depth*base->width]&kVoxelKindMask)==kVoxelSolid);
            }
            assert((base->front[at+16*base->width]&kVoxelKindMask)!=kVoxelSolid);
          }
        }
      }
    assert(cells==56 && ascending==36 && descending==20 && contact_pixels==cells*16 && endpoint_pixels==40);
    assert(support_pixels==5440);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(edited,&remapped,NULL,settings));SameGeometry(base,edited);
    assert(edited->column_curve_count==base->column_curve_count);
    for(unsigned i=0;i<base->column_curve_count;++i) {
      const VoxelTrunkCurve *a=base->column_curves+i,*b=edited->column_curves+i;
      assert(a->contact_x==b->contact_x && a->contact_count==b->contact_count && a->linear==b->linear &&
          !memcmp(a->contact,b->contact,sizeof(a->contact)));
    }
    /* Editor-native stamps retain the actual collision oracle, including
     * half/full grades and negative coordinates. No whole-room IDs or X
     * parity determine a ramp's structural mask. */
    DioramaTileStamp stamps[2]={{.x=-2,.y=-2,.metatile=0x32},{.x=-1,.y=-2,.metatile=0x4b}};
    for(unsigned i=0;i<2;++i) {
      int tx,ty;assert(FixtureCell(s,stamps[i].metatile,&tx,&ty));
      for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&stamps[i].words[q],NULL));
    }
    memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0]=(DioramaStampLayerOverride){
        .set_bounds=true,.x0=-2,.y0=-2,.x1=0,.y1=-1,.cells=stamps,.count=2};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    MountainSlopePixels(edited,-2,-2,6,0,6);MountainSlopePixels(edited,-1,-2,6,6,0);
    stamps[1].blank=true;assert(ActionVoxel_Build(edited,s,&terrain,settings));
    MountainSlopePixels(edited,-2,-2,6,0,0);
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    terrain.voxel.cells[0]=(DioramaVoxelCell){.x=200,.y=48,
        .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.03f,.back=.12f};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));VoxelCellGeometry authored;
    assert(ActionVoxel_CellGeometry(edited,200,48,&authored) && authored.authored);
    for(unsigned q=0;q<4;++q)assert(authored.front[q]==.03f && authored.back[q]==.12f);
    MountainSlopePixels(edited,201,48,6,6,0);
    /* Moving the overhanging source or hiding every face must not leave a
     * native endpoint at its old position in the next, independently owned
     * cell. Front-face-only visibility is handled separately by the editor. */
    for(unsigned hidden=0;hidden<2;++hidden) {
      terrain.voxel.cells[0]=(DioramaVoxelCell){.x=210,.y=15,
          .fields=hidden?kDioramaVoxelCell_Faces:kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,
          .front=.03f,.back=.12f,.faces=0};
      assert(ActionVoxel_Build(edited,s,&terrain,settings));
      assert(edited->column_curve_count==39);
      for(unsigned x=0;x<2;++x)
        assert((edited->front[(256-edited->origin_y)*edited->width+3360+x-edited->origin_x]&kVoxelKindMask)==kVoxelRearOnly);
      unsigned count;const VoxelEdge *e=ActionVoxel_Edges(edited,&count);
      for(unsigned i=0;i<count;++i)assert(e[i].side || e[i].far_rank!=2 || e[i].y!=256 ||
          e[i].x+e[i].length<=3360 || e[i].x>=3362);
    }
    /* Re-enabling all faces is a no-op, including the contextual endpoint
     * below a ramp. An actual source erasure still suppresses that pixel. */
    terrain.voxel.cells[0]=(DioramaVoxelCell){.x=210,.y=16,
        .fields=kDioramaVoxelCell_Faces,.faces=kDioramaVoxelFaceMask_All};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(base,edited);
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=210,.y=16};
    terrain.pixel_layers[0].edits[0].transparent[0]=0xc000;
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    for(unsigned x=0;x<2;++x)
      assert(!edited->front[(256-edited->origin_y)*edited->width+3360+x-edited->origin_x]);
    /* The continuation belongs to a reviewed ramp/support pair, including
     * stamps outside native map bounds. A cell elsewhere using the same
     * rock art remains a rear wall. Source/geometry edits are authoritative. */
    DioramaTileStamp band[4]={{.x=-2,.y=-2,.metatile=0x58},{.x=-1,.y=-2,.metatile=0x59},
        {.x=-2,.y=-1,.metatile=0x60},{.x=-1,.y=-1,.metatile=0x61}};
    for(unsigned i=0;i<4;++i) {
      int tx,ty;assert(FixtureCell(s,band[i].metatile,&tx,&ty));
      for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&band[i].words[q],NULL));
    }
    for(unsigned variant=0;variant<7;++variant) {
      memset(&terrain,0,sizeof(terrain));
      terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-2,.x1=0,.y1=0,.cells=band,.count=4};
      band[0].blank=variant==6;
      if(variant==1 || variant==2) {
        terrain.voxel.cell_count=1;
        terrain.voxel.cells[0]=(DioramaVoxelCell){.x=-2,.y=variant==1?-2:-1,
            .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.03f,.back=.12f};
      }
      if(variant>=3 && variant<=5) {
        terrain.pixel_layers[0].count=1;
        DioramaPixelEdit *p=&terrain.pixel_layers[0].edits[0];
        *p=(DioramaPixelEdit){.by_cell=true,.x=-2,.y=variant==5?-2:-1};
        if(variant==4)p->black[4]=0x0002;else p->transparent[variant==5?10:4]=0x0002;
      }
      assert(ActionVoxel_Build(edited,s,&terrain,settings));
      /* All-or-nothing taper ownership stops at an edited/missing source
       * column; no neighbouring source gains a guessed replacement shape. */
      assert(edited->column_curve_count==(!variant?2:1));
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned at=(-16+(int)y-edited->origin_y)*edited->width-32+x-edited->origin_x;
        const unsigned pixel=edited->front[at],kind=pixel&kVoxelKindMask;
        const bool support=y<(unsigned)MountainSlopeContact(9,0,9,x) &&
            variant!=1 && variant!=2 && variant!=6 &&
            !(x==14 && ((variant==3 || variant==4)?y>=4:variant==5));
        if(variant==3 && x==14 && y==4)assert(!pixel);
        else assert(kind==(support?kVoxelSolid:kVoxelRearOnly));
        assert(!(pixel&(kVoxelColliding|kVoxelHighPriority)));
      }
    }
    band[0].blank=false;
    assert(!memcmp(s,&original,sizeof(original)));
    printf("Aitos native ramps terrain%u: %u cells (%u ascending/%u descending), %u native contact caps (%u bounded endpoint pixels), %u continuous support pixels, %u preserved rear pixels, %u edges; paired/isolated stamps, remap and authored depth preserved\n",
        region,cells,ascending,descending,contact_pixels,endpoint_pixels,support_pixels,rear_pixels,edge_count);
    free(tops);ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
}
