#include "action/action_voxel_internal.h"

/* IDs locate the reviewed native fixture only. The production contract uses
 * displayed artwork/priority/collision fingerprints, tested after remapping. */
static bool BambooFixtureTarget(unsigned id) {
  return id==0xc1 || id==0xca || id==0xcb || id==0xcc || id==0xd1 || id==0xd3;
}
static uint32_t BambooFixtureInk(const ActionRoomScene *s,const ActionRoomSceneFrameState *frame) {
  /* The uniform native grove field beside the high stalk is the matte donor. */
  return ForestNativeColor(s,22*16+8,20*16,frame);
}
static void BambooFixtureLow(const ActionRoomScene *s,int cx,int cy,uint32_t low[256]) {
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    uint16_t word;assert(ActionRoomScene_LookupTile(s,1,cx*2+x/8,cy*2+y/8,&word,NULL));
    low[y*16+x]=(word&0x2000)?0:ForestNativeColor(s,cx*16+x,cy*16+y,&frame);
  }
}
static void BambooBackingEdits(const ActionRoomScene *s,ActionVoxelGeometry *g,
    ActionVoxelSettings settings) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  const ActionRoomSceneFrameState frame={.brightness=15};
  uint32_t low[256],rear[256],front[256];BambooFixtureLow(s,22,20,low);
  terrain.voxel.cell_count=1;DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=22,.y=20,.fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,352,320,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i)assert(!rear[i]);
  cell->fields|=kDioramaVoxelCell_BackArt;cell->backing=kDioramaVoxelBacking_Replace;
  cell->backing_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
  for(unsigned q=0;q<4;++q)
    assert(ActionRoomScene_LookupTile(s,1,46+(q&1),40+(q>>1),&cell->backing_art.words[q],NULL));
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,352,320,16,16,16,&frame,low,16,rear,front));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    assert(rear[y*16+x]==ForestNativeColor(s,368+x,320+y,&frame));
  memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=22,.y=20};pixel->transparent[5]=1u<<11;
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,352,320,16,16,16,&frame,low,16,rear,front));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<8;++x)
    assert(rear[y*16+x]==((y==5 && x==4)?0:BambooFixtureInk(s,&frame)));
  /* A reflected editor stamp outside native bounds must retain only its
   * actual high-priority footprint, not create a black rectangle around it. */
  for(unsigned flip=0;flip<4;++flip) {
    memset(&terrain,0,sizeof(terrain));DioramaTileStamp stamp={.x=-1,.y=-1};
    for(unsigned q=0;q<4;++q) {
      const unsigned source=q^flip;
      assert(ActionRoomScene_LookupTile(s,1,44+(source&1),40+(source>>1),&stamp.words[q],&stamp.metatile));
      stamp.words[q]^=(flip&1?0x4000:0)|(flip&2?0x8000:0);
    }
    terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-1,.y0=-1,.x1=0,.y1=0,
        .cells=&stamp,.count=1};
    assert(ActionVoxel_Build(g,s,&terrain,settings));assert(g->rear_fill_count==1);
    assert(ActionVoxel_Split(g,-16,-16,16,16,16,&frame,NULL,0,rear,front));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
      assert(rear[y*16+x]==((x/8==(flip&1))?BambooFixtureInk(s,&frame):0));
  }
}
static void BambooBackingFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot fixture;static ActionRoomScene original,variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/040%u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);const size_t n=fread(encoded,1,sizeof(encoded),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    original=fixture.scene;const ActionRoomScene *s=&fixture.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,room);
    assert(ActionVoxel_Build(g,s,NULL,settings));unsigned cells=0,pixels=0;
    const ActionRoomSceneFrameState frame={.brightness=15};
    const uint16_t native_ink=s->palette[73*2]|(s->palette[73*2+1]<<8);
    for(unsigned cy=0;cy<ActionRoomScene_TileHeight(s,1)/2;++cy)
      for(unsigned cx=0;cx<ActionRoomScene_TileWidth(s,1)/2;++cx) {
        uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,cx*2,cy*2,&word,&id));
        const VoxelRearFill *fill=JungleBackingAt(g,cx,cy);
        if(!BambooFixtureTarget(id)) {assert(!fill);continue;}
        ++cells;assert(fill && fill->preserve_existing && !fill->replace_front);
        uint32_t low[256],rear[256],front[256];BambooFixtureLow(s,cx,cy,low);
        assert(ActionVoxel_Split(g,cx*16,cy*16,16,16,16,&frame,low,16,rear,front));
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
          const unsigned at=y*16+x,q=(y/8)*2+x/8;
          assert(ActionRoomScene_LookupTile(s,1,cx*2+x/8,cy*2+y/8,&word,NULL));
          const bool high=(word&0x2000)!=0,opaque=FixturePixel(s,cx*16+x,cy*16+y)!=0;
          const bool fill_pixel=high&&opaque;pixels+=fill_pixel;
          assert(fill->colors[at]==(fill_pixel?(0x8000|native_ink):0));
          assert(rear[at]==(fill_pixel?BambooFixtureInk(s,&frame):low[at]));
          const unsigned role=g->front[(cy*16+y-g->origin_y)*g->width+cx*16+x-g->origin_x];
          if(opaque) {
            assert(!!(role&kVoxelHighPriority)==high);
            assert(!!(role&kVoxelColliding)==!!(s->bg[0].metatiles[id*8+q*2]&2));
          }
          /* Original native high art stays owned by the high layer; no copy
           * is inserted into the foreground low image or into a sky void. */
          if(high)assert(!front[at]);
        }
      }
    assert(cells==(room==1?248u:0u) && g->rear_fill_count==cells && cells<kVoxelMaxRearFills);
    if(room==1) {
      /* Actual sky just beyond the grove and all existing low foliage remain
       * native. In particular, no full-map opaque black wall is synthesized. */
      for(unsigned cy=20;cy<40;++cy) {
        uint32_t low[256],rear[256],front[256];BambooFixtureLow(s,40,cy,low);
        assert(ActionVoxel_Split(g,640,cy*16,16,16,16,&frame,low,16,rear,front));
        assert(!memcmp(low,rear,sizeof(low)));
      }
      variant=*s;Renumber(&variant);assert(ActionVoxel_Build(other,&variant,NULL,settings));SameGeometry(g,other);
      assert(other->rear_fill_count==cells);
      for(unsigned i=0;i<cells;++i) {
        const VoxelRearFill *a=&g->rear_fills[i],*b=JungleBackingAt(other,a->x/16,a->y/16);
        assert(b && !memcmp(a->colors,b->colors,sizeof(a->colors)));
      }
      /* Cached native ink follows palette edits and the ordinary frame fade
       * and subtractive-color math, instead of baking a near-black RGB. */
      for(unsigned phase=0;phase<2;++phase) {
        const ActionRoomSceneFrameState fade={.brightness=phase?7:15,
            .cgadsub=0x81,.fixed_color=phase?0x421:0};
        uint32_t low[256],rear[256],front[256];BambooFixtureLow(s,22,20,low);
        assert(ActionVoxel_Split(g,352,320,16,16,16,&fade,low,16,rear,front));
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<8;++x)
          assert(rear[y*16+x]==BambooFixtureInk(s,&fade));
      }
      variant=*s;variant.palette[73*2]=0xff;variant.palette[73*2+1]=0x7f;
      assert(ActionVoxel_Build(other,&variant,NULL,settings));SameGeometry(g,other);
      assert(JungleBackingAt(other,22,20)->colors[0]==0xffff);
      BambooBackingEdits(s,other,settings);
    }
    assert(!memcmp(s,&original,sizeof(original)));
    printf("Aitos bamboo backing 04/%02u terrain%u: %u cells/%u native high opaque pixels, low artwork and open grove boundary preserved; no priority/collision/source mutation\n",room,region,cells,pixels);
    ActionVoxel_Release(g);ActionVoxel_Release(other);
  }
}
