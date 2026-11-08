#include "action/action_voxel_internal.h"

static const VoxelRearFill *JungleBackingAt(const ActionVoxelGeometry *g,int x,int y) {
  for(unsigned i=0;i<g->rear_fill_count;++i)
    if(g->rear_fills[i].x==x*16 && g->rear_fills[i].y==y*16)return &g->rear_fills[i];
  return NULL;
}
static void JungleBackingCell(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    int x,int y,bool colliding) {
  uint32_t low[256],rear[256],front[256];DesertNativeLow(s,x*2,y*2,low);
  for(unsigned i=0;i<256;++i)assert(!low[i]); /* Native high artwork owns the whole post. */
  VoxelCellGeometry cell;assert(ActionVoxel_CellGeometry(g,x,y,&cell));
  for(unsigned q=0;q<4;++q) {
    uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&word,&id));
    assert(word&0x2000);
    assert(!!(s->bg[0].metatiles[id*8+q*2]&2)==colliding);
    assert(cell.front[q]==0 && fabsf(cell.back[q]-(colliding?.2f:.05f))<.000001f);
  }
  const VoxelRearFill *fill=JungleBackingAt(g,x,y);assert(fill && fill->preserve_existing);
  for(unsigned phase=0;phase<2;++phase) {
    const ActionRoomSceneFrameState frame={.brightness=phase?7:15};
    assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,&frame,low,16,rear,front));
    bool left=true,right=true;
    for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
      const unsigned at=py*16+px;assert((rear[at]>>24)==255 && !front[at]);
      left&=rear[at]==ForestNativeColor(s,(x-1)*16+px,y*16+py,&frame);
      right&=rear[at]==ForestNativeColor(s,(x+1)*16+px,y*16+py,&frame);
      const unsigned material=g->front[(y*16+py-g->origin_y)*g->width+x*16+px-g->origin_x];
      assert(material&kVoxelHighPriority);
      assert(!!(material&kVoxelColliding)==colliding);
    }
    if(x==19 && y>=20 && y<=25) {
      const int donor_x=(y==23 || y==24)?22:20;
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
        assert(rear[py*16+px]==ForestNativeColor(s,donor_x*16+px,y*16+py,&frame));
    } else assert(left || right); /* Complete native art, never a flat colour or a strip. */
  }
}
static void JungleBackingEdits(const ActionRoomScene *s,const ActionVoxelGeometry *base,
    ActionVoxelGeometry *edited,ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  const ActionRoomSceneFrameState frame={.brightness=15};uint32_t low[256]={0},rear[256],front[256];
  terrain.voxel.cell_count=1;DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=37,.y=23,.fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_Split(edited,592,368,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i)assert(!rear[i] && !front[i]);
  assert(!memcmp(JungleBackingAt(base,40,23)->colors,JungleBackingAt(edited,40,23)->colors,512));
  cell->fields|=kDioramaVoxelCell_BackArt;cell->backing=kDioramaVoxelBacking_Replace;
  cell->backing_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
  for(unsigned q=0;q<4;++q)
    assert(ActionRoomScene_LookupTile(s,1,76+(q&1),46+(q>>1),&cell->backing_art.words[q],NULL));
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_Split(edited,592,368,16,16,16,&frame,low,16,rear,front));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    assert(rear[y*16+x]==ForestNativeColor(s,608+x,368+y,&frame));
  /* A deliberately opened donor side is not evidence of continuous rear art.
   * A later zero-field record restores the native donor rather than inheriting None. */
  *cell=(DioramaVoxelCell){.x=36,.y=23,.fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(!JungleBackingAt(edited,37,23));
  terrain.voxel.cell_count=2;terrain.voxel.cells[1]=(DioramaVoxelCell){.x=36,.y=23};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(JungleBackingAt(edited,37,23));
  terrain.voxel.cell_count=1;*cell=(DioramaVoxelCell){.x=36,.y=23,.fields=kDioramaVoxelCell_Faces,
      .faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Back};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(!JungleBackingAt(edited,37,23));
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=37,.y=23};pixel->transparent[5]=1u<<11;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(ActionVoxel_Split(edited,592,368,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i)assert((rear[i]!=0)==(i!=5*16+4));
  pixel->x=36;assert(ActionVoxel_Build(edited,s,&terrain,settings));
  assert(!JungleBackingAt(edited,37,23)); /* An open donor must not close a deliberate gap. */
  /* The same three source cells remain meaningful beyond negative world bounds,
   * including a reflected stamp. No donor needs to be inside the capture. */
  DioramaTileStamp stamps[3];
  for(unsigned flip=0;flip<2;++flip) {
    memset(&terrain,0,sizeof(terrain));memset(stamps,0,sizeof(stamps));
    for(unsigned i=0;i<3;++i) {
      stamps[i].x=(int)i-2;stamps[i].y=-1;
      for(unsigned q=0;q<4;++q) {
        const unsigned source=(36+i)*2+((q&1)^flip);
        assert(ActionRoomScene_LookupTile(s,1,source,46+(q>>1),&stamps[i].words[q],&stamps[i].metatile));
        if(flip)stamps[i].words[q]^=0x4000;
      }
    }
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-1,.x1=1,.y1=0,
        .cells=stamps,.count=3};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    assert(JungleBackingAt(edited,-1,-1) && edited->rear_fill_count==1);
    assert(ActionVoxel_Split(edited,-16,-16,16,16,16,&frame,low,16,rear,front));
    bool left=true,right=true;
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      assert(!front[y*16+x]);const unsigned sx=flip?15-x:x;
      left&=rear[y*16+x]==ForestNativeColor(s,576+sx,368+y,&frame);
      right&=rear[y*16+x]==ForestNativeColor(s,608+sx,368+y,&frame);
    }
    assert(left || right);
    stamps[0].blank=true;
    assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(!edited->rear_fill_count);
  }
}
static void JungleShadowLow(const ActionRoomScene *s,int x,int y,uint32_t low[256]) {
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned q=0;q<4;++q) {
    uint16_t word;assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&word,NULL));
    assert(!(word&0x2000));
  }
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
    low[py*16+px]=ForestNativeColor(s,x*16+px,y*16+py,&frame);
}
static void JungleShadowRepairs(const ActionRoomScene *s,const ActionVoxelGeometry *base,
    ActionVoxelGeometry *edited,ActionVoxelSettings settings) {
  const ActionRoomSceneFrameState frame={.brightness=15};
  uint32_t low[256],rear[256],front[256];
  for(int y=20;y<=25;++y) {
    const int donor_x=(y==23 || y==24)?22:20;
    JungleShadowLow(s,18,y,low);
    const VoxelRearFill *repair=JungleBackingAt(base,18,y);assert(repair && repair->replace_front);
    assert(ActionVoxel_Split(base,288,y*16,16,16,16,&frame,low,16,rear,front));
    unsigned original_black=0,new_black=0;
    for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
      const unsigned at=py*16+px;const uint32_t expected=ForestNativeColor(s,donor_x*16+px,y*16+py,&frame);
      assert(front[at]==expected && rear[at]==expected);
      assert(!!(repair->colors[at]&0x8000)==(px>=8));
      if(px<8)assert(front[at]==low[at]);
      else {original_black+=low[at]==0xff000000;new_black+=front[at]==0xff000000;}
    }
    assert(original_black==128 && new_black<16); /* Proves both visible images lose the black slit. */
    VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(base,18,y,&shape));
    for(unsigned q=0;q<4;++q)assert(fabsf(shape.front[q]-.15f)<.000001f && fabsf(shape.back[q]-.2f)<.000001f);
  }
  /* Real window pieces and the two arched doorways retain native dark art;
   * they are neither donors for the repaired shadow nor automatic targets. */
  const int preserved[][2]={{16,23},{17,23},{20,23},{16,24},{17,24},{20,24},{43,24},{44,25},{46,24},{47,25}};
  for(unsigned i=0;i<sizeof(preserved)/sizeof(*preserved);++i) {
    const int x=preserved[i][0],y=preserved[i][1];assert(!JungleBackingAt(base,x,y));
    DesertNativeLow(s,x*2,y*2,low);
    assert(ActionVoxel_Split(base,x*16,y*16,16,16,16,&frame,low,16,rear,front));
    assert(!memcmp(low,rear,sizeof(low)));
  }
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=18,.y=23,.fields=kDioramaVoxelCell_Front,.front=.1f};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,18,23);assert(a && a->native_front_art);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    assert(ActionVoxel_AuthoredColor(a->pixels[0][y*16+x],&frame)==ForestNativeColor(s,352+x,368+y,&frame));
  cell->fields|=kDioramaVoxelCell_FrontArt;
  cell->front_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
  for(unsigned q=0;q<4;++q)
    assert(ActionRoomScene_LookupTile(s,1,40+(q&1),46+(q>>1),&cell->front_art.words[q],NULL));
  assert(ActionVoxel_Build(edited,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(edited,18,23);
  assert(a && !a->native_front_art);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    assert(ActionVoxel_AuthoredColor(a->pixels[0][y*16+x],&frame)==ForestNativeColor(s,320+x,368+y,&frame));
  *cell=(DioramaVoxelCell){.x=18,.y=23,.fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));JungleShadowLow(s,18,23,low);
  assert(ActionVoxel_Split(edited,288,368,16,16,16,&frame,low,16,rear,front));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    assert(!rear[y*16+x]);assert(front[y*16+x]==ForestNativeColor(s,352+x,368+y,&frame));
  }
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=18,.y=23};pixel->transparent[7]=1u<<5;pixel->black[7]=1u<<4;
  assert(ActionVoxel_Build(edited,s,&terrain,settings));low[7*16+10]=0;
  assert(ActionVoxel_Split(edited,288,368,16,16,16,&frame,low,16,rear,front));
  assert(!rear[7*16+10] && !front[7*16+10]);
  assert(rear[7*16+11]==0xff000000 && front[7*16+11]==0xff000000);
  DioramaTileStamp stamps[3];
  for(unsigned flip=0;flip<2;++flip) {
    memset(&terrain,0,sizeof(terrain));memset(stamps,0,sizeof(stamps));
    for(unsigned i=0;i<3;++i) {
      const unsigned native_x=i==1?18:((i==0)^flip)?20:19;
      stamps[i].x=(int)i-2;stamps[i].y=-1;
      for(unsigned q=0;q<4;++q) {
        assert(ActionRoomScene_LookupTile(s,1,native_x*2+((q&1)^flip),40+(q>>1),&stamps[i].words[q],&stamps[i].metatile));
        if(flip)stamps[i].words[q]^=0x4000;
      }
    }
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-1,.x1=1,.y1=0,.cells=stamps,.count=3};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    const VoxelRearFill *repair=JungleBackingAt(edited,-1,-1);assert(repair && repair->replace_front);
    JungleShadowLow(s,18,20,low);
    if(flip)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<8;++x){uint32_t t=low[y*16+x];low[y*16+x]=low[y*16+15-x];low[y*16+15-x]=t;}
    assert(ActionVoxel_Split(edited,-16,-16,16,16,16,&frame,low,16,rear,front));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const uint32_t expected=ForestNativeColor(s,320+(flip?15-x:x),320+y,&frame);
      assert(rear[y*16+x]==expected && front[y*16+x]==expected);
    }
    stamps[flip?0:2].blank=true;
    assert(ActionVoxel_Build(edited,s,&terrain,settings));assert(!JungleBackingAt(edited,-1,-1));
  }
}
static void JungleBackingFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot fixture;static ActionRoomScene source,variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0501.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t n=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));source=fixture.scene;
    const ActionRoomScene *s=&fixture.scene;
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(5,1);
    assert(ActionVoxel_Build(base,s,NULL,settings));
    for(int y=19;y<=26;++y)JungleBackingCell(s,base,19,y,y==26);
    for(int y=21;y<=26;++y)for(int x=37;x<=40;x+=3)JungleBackingCell(s,base,x,y,y==26);
    assert(base->rear_fill_count>=20 && base->rear_fill_count<kVoxelMaxRearFills);
    /* The now-aligned stone course does not create a new water backing fill. */
    for(int x=37;x<=40;x+=3)for(int y=27;y<=28;++y) {
      assert(!JungleBackingAt(base,x,y));VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(base,x,y,&shape));
      for(unsigned q=0;q<4;++q)assert(fabsf(shape.front[q]-(y==28 && q>=2?.2f:0))<.000001f);
    }
    variant=*s;Renumber(&variant);assert(ActionVoxel_Build(edited,&variant,NULL,settings));SameGeometry(base,edited);
    assert(base->rear_fill_count==edited->rear_fill_count);
    for(unsigned i=0;i<base->rear_fill_count;++i) {
      const VoxelRearFill *a=&base->rear_fills[i],*b=JungleBackingAt(edited,a->x/16,a->y/16);
      assert(b && !memcmp(a->colors,b->colors,sizeof(a->colors)));
    }
    JungleBackingEdits(s,base,edited,settings);
    JungleShadowRepairs(s,base,edited,settings);
    assert(!memcmp(s,&source,sizeof(source)));
    printf("Marahna pillar backing terrain%u: twenty complete native rear cells plus six repaired half-shadows in both split images; native front priority/collision and water preserved; remapping, reflected negative stamps, cropped capture, real windows, painted faces and explicit None/Replace/reset/masks passed\n",region);
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
}
