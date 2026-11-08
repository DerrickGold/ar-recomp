/* These fixture IDs locate reviewed diagonal ice art only. Runtime ownership
 * uses displayed-art fingerprints; matte is native opaque cave shadow, not
 * a new fill or a physical part of the slope. */
static void IceMatteContours(const ActionVoxelGeometry *fixed,
    const ActionVoxelGeometry *cutout) {
  unsigned a_count,b_count;
  TempleAuditEdgePixel *a=TempleAuditEdgePixels(fixed,&a_count),
      *b=TempleAuditEdgePixels(cutout,&b_count);
  assert(a_count==b_count);
  for(unsigned i=0;i<a_count;++i) {
    assert(!TempleAuditEdgeOrder(&a[i],&b[i]));
    const VoxelEdge *ae=a[i].edge,*be=b[i].edge;
    assert(ae->near_rank==be->near_rank && ae->far_rank==be->far_rank &&
        ae->high_priority==be->high_priority && ae->colliding==be->colliding &&
        ae->rear_mounted==be->rear_mounted && ae->soft_surface==be->soft_surface &&
        ae->column_curve==be->column_curve && ae->trunk_curve==be->trunk_curve &&
        ae->rear_fold_top==be->rear_fold_top && ae->rear_fold_bottom==be->rear_fold_bottom);
    for(unsigned pass=kVoxelEdgeNative;pass<=kVoxelEdgeRearTrim;++pass) {
      VoxelEdgeSpan as,bs;
      const bool av=ActionVoxel_ResolveEdgeSpan(fixed,ae,pass,0,ActionVoxel_Depth(fixed),&as),
          bv=ActionVoxel_ResolveEdgeSpan(cutout,be,pass,0,ActionVoxel_Depth(cutout),&bs);
      assert(av==bv);
      if(av)assert(as.near_depth==bs.near_depth && as.far_depth==bs.far_depth);
    }
  }
  free(a);free(b);
}
static bool IceSlopeFixtureId(unsigned id) {
  return id==0x53 || id==0x55 || id==0x5a || id==0x5e;
}
static unsigned IceSlopeCaps(const ActionVoxelGeometry *g,const ActionRoomScene *s) {
  const int columns=ActionRoomScene_TileWidth(s,1)/2,rows=ActionRoomScene_TileHeight(s,1)/2;
  unsigned char *seen=calloc((size_t)columns,rows);assert(seen);
  unsigned expected=0,covered=0,risers=0,count;unsigned char checked_donors[256]={0};
  for(int y=0;y<rows;++y)for(int x=0;x<columns;++x) {
    uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
    expected+=IceSlopeFixtureId(id);
  }
  const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->surface_top_material)assert((e->side&1) && e->soft_surface &&
        (e->fold_length?e->fold_length:e->length)<=2);
    if(e->side==2)continue;
    uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,e->x/8,e->y/8,&word,&id));
    if(!IceSlopeFixtureId(id))continue;
    const bool riser=(e->side&1) && e->soft_surface && (e->fold_length?e->fold_length:e->length)<=2;
    if(e->side && !riser){assert(!e->surface_top_material);continue;}
    assert(!!e->surface_top_material==riser);risers+=riser;
    assert(e->surface_texture && e->top_sample_size==16 && e->top_owned_art);
    uint16_t donor_word;uint8_t donor;
    assert(ActionRoomScene_LookupTile(s,1,e->top_sample_x/8,e->top_sample_y/8,&donor_word,&donor));
    assert(!((word^donor_word)&0x3c00));
    if(!checked_donors[donor]) {
      bool matches=false;
      for(unsigned original_id=0x50;original_id<=0x51;++original_id) {
        int tx,ty;assert(FixtureCell(s,original_id,&tx,&ty));
        for(unsigned flip=0;flip<4;++flip) {
          bool same=true;
          for(unsigned y=0;y<16 && same;++y)for(unsigned x=0;x<16;++x)
            if(FixturePixel(s,e->top_sample_x+x,e->top_sample_y+y)!=
                FixturePixel(s,tx*8+(flip&1?15-x:x),ty*8+(flip&2?15-y:y))) {same=false;break;}
          matches|=same;
        }
      }
      /* Native6b/6c reuse50/51 reflected in both axes; the material-key
       * contract intentionally accepts them. High-priority b8/b9 aliases
       * still fail the palette/priority comparison above. */
      assert(matches);checked_donors[donor]=1;
    }
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
      assert(FixturePixel(s,e->top_sample_x+x,e->top_sample_y+y));
    const unsigned at=(e->y/16)*columns+e->x/16;
    if(!e->side && !seen[at]){seen[at]=1;++covered;}
  }
  assert(expected && covered==expected && risers);free(seen);return covered;
}
static void IceMatteFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot fixture;static ActionRoomScene original;
  static DioramaRoomOverride cutout,terrain;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  const unsigned ids[]={0x53,0x55,0x5a,0x5e};
  for(unsigned region=0;region<3;++region)for(unsigned room=2;room<=4;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/060%u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));original=fixture.scene;
    const ActionRoomScene *s=&fixture.scene;
    ActionVoxelGeometry *fixed=ActionVoxel_Create(),*reference=ActionVoxel_Create(),*edited=ActionVoxel_Create();
    assert(fixed&&reference&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,room);
    assert(ActionVoxel_Build(fixed,s,NULL,settings));
    const unsigned slope_caps=IceSlopeCaps(fixed,s);
    memset(&cutout,0,sizeof(cutout));unsigned cells=0,matte_pixels=0;
    for(unsigned art=0;art<sizeof(ids)/sizeof(*ids);++art) {
      int tx,ty;if(!FixtureCell(s,ids[art],&tx,&ty))continue;
      DioramaPixelEdit *mask=&cutout.pixel_layers[0].edits[cutout.pixel_layers[0].count++];
      *mask=(DioramaPixelEdit){.metatile=ids[art]};
      unsigned matte=0;
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        if(FixturePixel(s,tx*8+x,ty*8+y)==1) {mask->transparent[y]|=1u<<(15-x);++matte;}
      assert(matte==120); /* Native diagonal corner, not an enclosed shadow. */
    }
    assert(cutout.pixel_layers[0].count);
    assert(ActionVoxel_Build(reference,s,&cutout,settings));
    IceMatteContours(fixed,reference);
    const ActionRoomSceneFrameState frame={.brightness=15};
    for(unsigned art=0;art<sizeof(ids)/sizeof(*ids);++art) {
      int tx,ty;if(!FixtureCell(s,ids[art],&tx,&ty))continue;
      uint32_t native[256],cleared[256],rear[256],front[256],old_rear[256],old_front[256];
      DesertNativeLow(s,tx,ty,native);memcpy(cleared,native,sizeof(cleared));
      unsigned first=256;
      for(unsigned q=0;q<4;++q) {
        uint8_t rows[8]={0};assert(!ActionVoxel_CutoutRows(fixed,tx+(q&1),ty+(q>>1),rows));
        uint16_t word;assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&word,NULL));
        assert(!(word&0x2000)); /* This repair does not change a native actor-priority slot. */
      }
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        if(FixturePixel(s,tx*8+x,ty*8+y)==1) {
          const unsigned at=y*16+x;assert(native[at]==UINT32_C(0xff000000));
          cleared[at]=0;if(first==256)first=at;++matte_pixels;
        }
      assert(first<256);
      assert(ActionVoxel_Split(fixed,tx*8,ty*8,16,16,16,&frame,native,16,rear,front));
      assert(ActionVoxel_Split(reference,tx*8,ty*8,16,16,16,&frame,cleared,16,old_rear,old_front));
      assert(!memcmp(front,old_front,sizeof(front)));
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned at=y*16+x;
        if(FixturePixel(s,tx*8+x,ty*8+y)==1)assert(rear[at]==native[at] && !front[at]);
        else assert(rear[at]==old_rear[at]);
      }
      /* Source alpha is authoritative: a missing source cannot become a wall. */
      native[first]=0;
      assert(ActionVoxel_Split(fixed,tx*8,ty*8,16,16,16,&frame,native,16,rear,front));
      assert(!rear[first] && !front[first]);native[first]=UINT32_C(0xff000000);
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
      DioramaPixelEdit *paint=&terrain.pixel_layers[0].edits[0];
      *paint=(DioramaPixelEdit){.by_cell=true,.x=tx/2,.y=ty/2};
      paint->black[first/16]=(uint16_t)(1u<<(15-first%16));
      assert(ActionVoxel_Build(edited,s,&terrain,settings));
      assert(ActionVoxel_Split(edited,tx*8,ty*8,16,16,16,&frame,native,16,rear,front));
      assert(front[first]==native[first]);
      paint->black[first/16]=0;paint->transparent[first/16]=(uint16_t)(1u<<(15-first%16));
      assert(ActionVoxel_Build(edited,s,&terrain,settings));native[first]=0;
      assert(ActionVoxel_Split(edited,tx*8,ty*8,16,16,16,&frame,native,16,rear,front));
      assert(!rear[first] && !front[first]);
      /* Explicit backing removal still wins over automatic native retention. */
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      terrain.voxel.cells[0]=(DioramaVoxelCell){.x=tx/2,.y=ty/2,
          .fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
      assert(ActionVoxel_Build(edited,s,&terrain,settings));native[first]=UINT32_C(0xff000000);
      assert(ActionVoxel_Split(edited,tx*8,ty*8,16,16,16,&frame,native,16,rear,front));
      assert(!rear[first] && !front[first]);++cells;
    }
    assert(cells && matte_pixels==cells*120);
    /* The untouched source carries all collision, palette and priority bytes. */
    assert(!memcmp(s,&original,sizeof(original)));
    printf("ice rear matte06/%02u terrain%u: %u reviewed corners/%u rear pixels; %u complete same-palette ice caps; native front/contours, actor priority, source alpha and explicit edits preserved\n",room,region,cells,matte_pixels,slope_caps);
    ActionVoxel_Release(fixed);ActionVoxel_Release(reference);ActionVoxel_Release(edited);
  }
}
