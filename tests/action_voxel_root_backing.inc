#include "action/action_voxel_internal.h"

static const VoxelRearFill *ForestRootFill(const ActionVoxelGeometry *g,int x,int y) {
  for(unsigned i=0;i<g->rear_fill_count;++i) {
    const VoxelRearFill *fill=&g->rear_fills[i];
    if(fill->x!=x*16 || fill->y!=y*16 || !fill->preserve_existing || fill->replace_front)continue;
    bool wood_only=true,any=false;
    for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)if(fill->colors[py*16+px]&0x8000) {
      any=true;const unsigned role=g->front[(size_t)(y*16+py-g->origin_y)*g->width+x*16+px-g->origin_x];
      wood_only&=(role&kVoxelKindMask)==kVoxelForeground;
    }
    if(any && wood_only)return fill;
  }
  return NULL;
}
static bool ForestRootCurved(const ActionVoxelGeometry *g,int x,int y) {
  const int mx=x-g->origin_x,my=y-g->origin_y;
  return g->trunk_cells && g->trunk_cells[(size_t)(my/8)*(g->width/8)+mx/8];
}
static void ForestRootNative(const ActionRoomScene *s,int x,int y,
    const ActionRoomSceneFrameState *frame,uint32_t low[256]) {
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
    low[py*16+px]=ForestNativeColor(s,x*16+px,y*16+py,frame);
}
/* Source pixels, not a copy of the production material table, prove which
 * native quadrant contains a complete leaf patch and which bark was moved. */
static unsigned ForestRootVerify(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    int cx,int cy,const ActionRoomSceneFrameState *frame) {
  unsigned donor=4;
  for(unsigned q=0;q<4 && donor==4;++q) {
    bool foliage=true;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
      const unsigned ink=FixturePixel(s,cx*16+(q&1)*8+x,cy*16+(q>>1)*8+y);
      foliage&=ink>=1 && ink<=7;
    }
    if(foliage)donor=q;
  }
  assert(donor<4);
  uint32_t low[256],rear[256],front[256];ForestRootNative(s,cx,cy,frame,low);
  assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,frame,low,16,rear,front));
  const VoxelRearFill *own=ForestRootFill(g,cx,cy);
  unsigned repaired=0;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned at=y*16+x,ink=FixturePixel(s,cx*16+x,cy*16+y);
    assert(ink);const bool wood=ink>=8,curved=wood && ForestRootCurved(g,cx*16+x,cy*16+y);
    /* Existing thorn/border continuations can already cross a root. Retain
     * their exact colours; the new sparse repair owns only remaining alpha. */
    uint32_t previous=curved?0:low[at];
    for(unsigned i=0;i<g->rear_fill_count;++i) {
      const VoxelRearFill *fill=&g->rear_fills[i];
      if(fill==own || fill->x!=cx*16 || fill->y!=cy*16 || !(fill->colors[at]&0x8000))continue;
      if(!fill->preserve_existing || !(previous>>24))previous=ActionVoxel_AuthoredColor(fill->colors[at],frame);
    }
    const uint32_t expected=curved && !previous?ForestNativeColor(s,cx*16+(donor&1)*8+(x&7),
        cy*16+(donor>>1)*8+(y&7),frame):previous;
    if(rear[at]!=expected)fprintf(stderr,"root backing missing cell%d,%d pixel%u,%u curved%d rear%08x expected%08x\n",
        cx,cy,x,y,curved,rear[at],expected);
    assert(rear[at]==expected && front[at]==(wood?low[at]:0));repaired+=curved;
    const unsigned role=g->front[(size_t)(cy*16+y-g->origin_y)*g->width+cx*16+x-g->origin_x];
    assert(!(role&(kVoxelColliding|kVoxelHighPriority)));
    assert((role&kVoxelKindMask)==(wood?kVoxelForeground:kVoxelRearOnly));
  }
  /* An uncurved root can already have an independent border continuation;
   * the split assertions above require every one of its native pixels intact. */
  if(repaired) {
    const VoxelRearFill *fill=ForestRootFill(g,cx,cy);assert(fill->preserve_existing && !fill->replace_front);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
      assert(!!(fill->colors[y*16+x]&0x8000)==(FixturePixel(s,cx*16+x,cy*16+y)>=8 &&
          ForestRootCurved(g,cx*16+x,cy*16+y)));
    /* The compiled donor need not be visible in a one-pixel capture. */
    const unsigned at=0;uint32_t crop_rear,crop_front;
    assert(ActionVoxel_Split(g,cx*16,cy*16,1,1,1,frame,low+at,16,&crop_rear,&crop_front));
    assert(crop_rear==rear[at] && crop_front==front[at]);
  }
  return repaired;
}
static void ForestRootEdits(const ActionRoomScene *s,const ActionVoxelGeometry *base,
    ActionVoxelGeometry *edited,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  const ActionRoomSceneFrameState frame={.brightness=15};uint32_t low[256],rear[256],front[256];
  ForestRootNative(s,128,23,&frame,low);terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=128,.y=23,.fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(!ForestRootFill(edited,128,23));
  assert(ActionVoxel_Split(edited,2048,368,16,16,16,&frame,low,16,rear,front));
  for(unsigned p=0;p<256;++p)assert(!rear[p]);
  assert(!memcmp(ForestRootFill(base,129,23),ForestRootFill(edited,129,23),sizeof(VoxelRearFill)));
  cell->fields|=kDioramaVoxelCell_BackArt;cell->backing=kDioramaVoxelBacking_Replace;
  cell->backing_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
  for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,260+(q&1),46+(q>>1),&cell->backing_art.words[q],NULL));
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_Split(edited,2048,368,16,16,16,&frame,low,16,rear,front));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    assert(rear[y*16+x]==ForestNativeColor(s,2080+x,368+y,&frame));
  *cell=(DioramaVoxelCell){.x=128,.y=23,.fields=kDioramaVoxelCell_Faces,
      .faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Back};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_Split(edited,2048,368,16,16,16,&frame,low,16,rear,front));
  for(unsigned p=0;p<256;++p)assert(!rear[p]);
  cell->faces=kDioramaVoxelFaceMask_All;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(base,edited);
  assert(ForestRootVerify(s,edited,128,23,&frame));
  *cell=(DioramaVoxelCell){.x=128,.y=23,.fields=kDioramaVoxelCell_Width|kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,
      .width=8,.front=.03f,.back=.12f};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(!ForestRootFill(edited,128,23));
  const VoxelAuthoredCell *owned=ActionVoxel_AuthoredCellAt(edited,128,23);assert(owned && owned->width==8);
  for(unsigned q=0;q<4;++q)assert(owned->front[q]==.03f && owned->back[q]==.12f);
  assert(ForestRootVerify(s,edited,129,23,&frame));
  /* Undo supplies a complete zero-field replacement, not stale edit flags. */
  terrain.voxel.cell_count=2;terrain.voxel.cells[1]=(DioramaVoxelCell){.x=128,.y=23};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(base,edited);
  assert(ForestRootVerify(s,edited,128,23,&frame));
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=128,.y=23};
  unsigned wood=0;while(wood<256 && FixturePixel(s,2048+wood%16,368+wood/16)<8)++wood;assert(wood<256);
  pixel->transparent[wood/16]=1u<<(15-wood%16);
  assert(ActionVoxel_Build(edited,s,&terrain,settings));low[wood]=0;
  assert(ActionVoxel_Split(edited,2048,368,16,16,16,&frame,low,16,rear,front));
  assert(!rear[wood] && !front[wood]);
  assert(!ForestRootFill(edited,128,23) || !ForestRootFill(edited,128,23)->colors[wood]);
  memset(pixel->transparent,0,sizeof(pixel->transparent));pixel->black[wood/16]=1u<<(15-wood%16);
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(!ForestRootFill(edited,128,23) || !ForestRootFill(edited,128,23)->colors[wood]);
  /* The only complete leaf quadrant is unavailable after an explicit hole. */
  memset(pixel->black,0,sizeof(pixel->black));pixel->transparent[8]=0x8000;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(!ForestRootFill(edited,128,23));
  assert(ForestRootVerify(s,edited,129,23,&frame));
}
static void ForestRootStamp(const ActionRoomScene *s,ActionVoxelGeometry *edited,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;DioramaTileStamp stamps[24];
  for(unsigned flip=0;flip<2;++flip) {
    memset(&terrain,0,sizeof(terrain));memset(stamps,0,sizeof(stamps));
    for(unsigned y=0;y<6;++y)for(unsigned x=0;x<4;++x) {
      DioramaTileStamp *stamp=&stamps[y*4+x];stamp->x=(int)x-3;stamp->y=(int)y-5;
      const unsigned sx=127+(flip?3-x:x);
      for(unsigned q=0;q<4;++q) {
        assert(ActionRoomScene_LookupTile(s,1,sx*2+((q&1)^flip),(20+y)*2+(q>>1),&stamp->words[q],&stamp->metatile));
        if(flip)stamp->words[q]^=0x4000;
      }
    }
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-3,.y0=-5,.x1=1,.y1=1,
        .cells=stamps,.count=24};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    const int cx=flip?-1:-2,cy=-2;const VoxelRearFill *fill=ForestRootFill(edited,cx,cy);
    assert(fill && fill->preserve_existing);
    const ActionRoomSceneFrameState frame={.brightness=15};uint32_t low[256],rear[256],front[256];
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
      low[y*16+x]=ForestNativeColor(s,2048+(flip?15-x:x),368+y,&frame);
    assert(ActionVoxel_Split(edited,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned sx=flip?15-x:x,ink=FixturePixel(s,2048+sx,368+y);
      assert(front[y*16+x]==(ink>=8?low[y*16+x]:0));
      const bool curved=ink>=8 && ForestRootCurved(edited,cx*16+x,cy*16+y);
      const uint32_t expected=curved?ForestNativeColor(s,2048+(sx&7),376+(y&7),&frame):low[y*16+x];
      assert(rear[y*16+x]==expected);
    }
    /* The same native bank/crown relationship survives reflected negative
     * stamps; the inherited rear copy keeps every original alpha hole. */
    const int crown_x=flip?0:-3;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
      low[y*16+x]=ForestNativeColor(s,2032+(flip?15-x:x),368+y,&frame);
    assert(ActionVoxel_Split(edited,crown_x*16,-32,16,16,16,&frame,low,16,rear,front));
    for(unsigned p=0;p<256;++p)assert(rear[p]==low[p] && front[p]==low[p]);
  }
}
static uint64_t ForestBackingHash(const void *bytes,size_t count) {
  const unsigned char *p=bytes;uint64_t hash=UINT64_C(14695981039346656037);
  for(size_t i=0;i<count;++i)hash=(hash^p[i])*UINT64_C(1099511628211);
  return hash;
}
/* These mixed bank-edge cards are independent of the bark above them. The
 * prior curved-root-only repair left their original, opaque rear footprint
 * completely empty. Locate reviewed specimens by source IDs in this fixture;
 * the production contract uses exact native artwork fingerprints. */
static bool ForestBorderBackingVerify(const ActionRoomScene *s,const ActionVoxelGeometry *g,unsigned region) {
  const ActionRoomSceneFrameState frame={.brightness=15};unsigned cells=0,opaque=0,missing=0;
  unsigned crowns=0,crown_opaque=0,rear_plants=0,growth_plants=0;
  for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
      bool crown=false,rear_plant=false,growth_plant=false;
      if(id==2 || id==0xc1 || id==0xd1) {
        uint8_t below;assert(ActionRoomScene_LookupTile(s,1,x*2,(y+1)*2,&word,&below));
        /* Native crown02 and C1 have identical artwork. Its bank support is
         * the independent witness: copying this fingerprint everywhere would
         * incorrectly add a backstop to foreground thorn-platform plants. */
        crown=below==0xc9;
        growth_plant=below==0xba || below==0xbb;
        rear_plant=!crown && !growth_plant;
        assert(crown || growth_plant || below==0x1b || below==0x1d || below==0x0a || below==0x0b || below==0xb0);
        crowns+=crown;rear_plants+=rear_plant;growth_plants+=growth_plant;
      } else if(id!=0xb9 && id!=0xc0 && id!=0xc9)continue;
      else ++cells;
      uint32_t low[256],rear[256],front[256];ForestRootNative(s,x,y,&frame,low);
      assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,&frame,low,16,rear,front));
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
        const unsigned at=py*16+px;
        assert(front[at]==(rear_plant?0:low[at])); /* Preserve the existing native front plane. */
        if(low[at]) {
          const unsigned role=g->front[(size_t)(y*16+py-g->origin_y)*g->width+x*16+px-g->origin_x];
          assert((role&kVoxelKindMask)==(rear_plant?kVoxelRearOnly:kVoxelFoliage) && !(role&(kVoxelColliding|kVoxelHighPriority)));
          if(growth_plant)assert(!(role&kVoxelRearCopy));
          else {missing+=rear[at]!=low[at];if(crown)++crown_opaque;else if(!rear_plant)++opaque;}
        } else if(!growth_plant)assert(!rear[at]); /* Preserve the irregular outer silhouette. */
      }
    }
  assert(cells==(region==1?5u:3u));
  assert(crowns==(region==1?2u:1u) && growth_plants==(region==1?13u:7u));
  /* Recorded before/after hashes independently expose any accidental geometry
   * change in this backing-only contract. Ignore only its intended source tag. */
  uint64_t mask=UINT64_C(14695981039346656037);
  for(size_t i=0;i<(size_t)g->width*g->height;++i)mask=(mask^(g->front[i]&~kVoxelRearCopy))*UINT64_C(1099511628211);
  printf("forest border terrain%u: cells%u opaque%u missing%u mask%016llx edges%016llx recess%016llx curves%016llx\n",
      region,cells,opaque,missing,(unsigned long long)mask,
      (unsigned long long)ForestBackingHash(g->edges,g->edge_count*sizeof(*g->edges)),
      (unsigned long long)ForestBackingHash(g->front_recess,(size_t)(g->width/8)*(g->height/8)),
      (unsigned long long)ForestBackingHash(g->trunk_cells,(size_t)(g->width/8)*(g->height/8)));
  printf("forest crown census terrain%u: %u backed crowns/%u native ink pixels, %u existing rear plants, %u independent growth crowns unchanged\n",
      region,crowns,crown_opaque,rear_plants,growth_plants);
  return missing==0;
}
static void ForestBorderBackingEdits(const ActionRoomScene *s,const ActionVoxelGeometry *base,
    ActionVoxelGeometry *edited,ActionVoxelSettings settings,int cx,int cy) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  const ActionRoomSceneFrameState frame={.brightness=15};uint32_t low[256],rear[256],front[256];
  ForestRootNative(s,cx,cy,&frame,low);terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=cx,.y=cy,.fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_Split(edited,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned at=0;at<256;++at)assert(!rear[at] && front[at]==low[at]);
  cell->fields|=kDioramaVoxelCell_BackArt;cell->backing=kDioramaVoxelBacking_Replace;
  cell->backing_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
  for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,258+(q&1),48+(q>>1),&cell->backing_art.words[q],NULL));
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_Split(edited,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    assert(front[y*16+x]==low[y*16+x]);
    assert(rear[y*16+x]==ForestNativeColor(s,2064+x,384+y,&frame));
  }
  *cell=(DioramaVoxelCell){.x=cx,.y=cy,.fields=kDioramaVoxelCell_FrontArt,.front_art=cell->backing_art};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,cx,cy);assert(a && a->surface);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    assert(ActionVoxel_AuthoredColor(a->pixels[0][y*16+x],&frame)==ForestNativeColor(s,2064+x,384+y,&frame));
  *cell=(DioramaVoxelCell){.x=cx,.y=cy}; /* Clear artwork/undo uses native front and backing. */
  assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(base,edited);
  assert(ActionVoxel_Split(edited,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned at=0;at<256;++at)assert(rear[at]==low[at] && front[at]==low[at]);
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=cx,.y=cy};pixel->transparent[3]=0x1000;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));low[3*16+3]=0;
  assert(ActionVoxel_Split(edited,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
  for(unsigned at=0;at<256;++at)assert(rear[at]==low[at] && front[at]==low[at]);
  uint32_t neighbor[256];ForestRootNative(s,127,24,&frame,neighbor);
  assert(ActionVoxel_Split(edited,2032,384,16,16,16,&frame,neighbor,16,rear,front));
  for(unsigned at=0;at<256;++at)assert(rear[at]==neighbor[at] && front[at]==neighbor[at]);
}
static void ForestRootBackingFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];bool complete=true;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0101.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(encoded,1,sizeof(encoded),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(encoded,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;original=*s;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,1);
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    assert(ActionVoxel_Build(base,s,NULL,settings));unsigned roots=0,repaired_cells=0,repaired_pixels=0;
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
        uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
        if(id!=0x30 && id!=0x31)continue;++roots;
        for(unsigned phase=0;phase<2;++phase) {
          const ActionRoomSceneFrameState frame={.brightness=phase?7:15,.fixed_color=phase?0x0421:0,.cgadsub=phase?0x81:0};
          const unsigned repaired=ForestRootVerify(s,base,x,y,&frame);
          if(!phase){repaired_cells+=repaired!=0;repaired_pixels+=repaired;}
        }
      }
    assert(roots==36 && repaired_cells>20 && repaired_cells<roots && base->rear_fill_count<kVoxelMaxRearFills);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(edited,&remapped,NULL,settings));SameGeometry(base,edited);
    assert(!memcmp(base->front,edited->front,(size_t)base->width*base->height));
    assert(base->trunk_curve_count==edited->trunk_curve_count && !memcmp(base->trunk_curves,edited->trunk_curves,sizeof(base->trunk_curves)));
    assert(!memcmp(base->trunk_cells,edited->trunk_cells,(size_t)(base->width/8)*(base->height/8)));
    assert(base->rear_fill_count==edited->rear_fill_count && !memcmp(base->rear_fills,edited->rear_fills,base->rear_fill_count*sizeof(VoxelRearFill)));
    ForestRootEdits(s,base,edited,settings);ForestRootStamp(s,edited,settings);
    const bool borders=ForestBorderBackingVerify(s,base,region);complete&=borders;
    if(borders) {
      ForestBorderBackingEdits(s,base,edited,settings,128,24);
      ForestBorderBackingEdits(s,base,edited,settings,127,23);
    }
    assert(!memcmp(s,&original,sizeof(original)));
    printf("forest root backing terrain%u: %u native roots, %u curved cells/%u repaired pixels, %u rear records; colors/fade/crop/front/curve/remap/overrides/pixel edits/reflected negative stamps PASS\n",
        region,roots,repaired_cells,repaired_pixels,base->rear_fill_count);
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
  assert(complete);
}
