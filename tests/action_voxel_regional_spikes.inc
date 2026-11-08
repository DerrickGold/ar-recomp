/* Regional spike ownership regression. Fixture IDs locate native artwork;
 * Renumber below proves that production ownership is not tied to those IDs.
 * Cave foundations intentionally stay rear-only. Ice lower spikes retain
 * their existing foliage/spike patch contract. Neither becomes a solid deck
 * merely because the European native definition has collision15. */
#include "action/action_voxel_internal.h"

typedef struct RegionalSpikeFixture {
  unsigned group,room,id,kind,semantic,count[3];
} RegionalSpikeFixture;

static void RegionalSpikeLoad(const char *directory,unsigned region,
    const RegionalSpikeFixture *spec,ActionSceneSnapshot *snapshot) {
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  char path[1024];snprintf(path,sizeof(path),"%s/%u/%02u%02u.arscene",
      directory,region,spec->group,spec->room);
  FILE *file=fopen(path,"rb");assert(file);
  const size_t n=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
  assert(ActionSceneSnapshot_Decode(encoded,n,snapshot));
  assert(snapshot->scene.group==spec->group && snapshot->scene.map==spec->room);
  assert(snapshot->terrain_profile==region);
}

static unsigned RegionalSpikeCollision(const ActionRoomScene *s,unsigned id) {
  unsigned result=0;
  for(unsigned q=0;q<4;++q)result|=!!(s->bg[0].metatiles[id*8+q*2]&2)<<q;
  return result;
}

static bool RegionalSpikeAt(const ActionRoomScene *s,unsigned id,int x,int y) {
  uint16_t word;uint8_t native;
  return x>=0 && y>=0 && ActionRoomScene_LookupTile(s,1,x/8,y/8,&word,&native) && native==id;
}

static size_t RegionalSpikePixelAt(const ActionVoxelGeometry *g,int x,int y) {
  assert(x>=g->origin_x && x<g->origin_x+g->width &&
      y>=g->origin_y && y<g->origin_y+g->height);
  return (size_t)(y-g->origin_y)*g->width+x-g->origin_x;
}

static void RegionalSpikeCellEqual(const ActionVoxelGeometry *a,
    const ActionVoxelGeometry *b,int x,int y) {
  VoxelCellGeometry aa,bb;
  assert(ActionVoxel_CellGeometry(a,x,y,&aa) && ActionVoxel_CellGeometry(b,x,y,&bb));
  assert(!memcmp(aa.front,bb.front,sizeof(aa.front)) && !memcmp(aa.back,bb.back,sizeof(aa.back)));
  assert(aa.width==bb.width && aa.offset_x==bb.offset_x && aa.faces==bb.faces &&
      aa.shape==bb.shape && aa.authored==bb.authored);
}

static unsigned RegionalSpikePatchOwners(const ActionVoxelGeometry *g,int x,int y) {
  unsigned n,count=0;const VoxelPatch *patches=ActionVoxel_Patches(g,&n);
  for(unsigned i=0;i<n;++i) {
    const VoxelPatch *p=&patches[i];
    if(p->kind==kVoxelPatch_Spikes && x>=p->x && x<p->x+p->width &&
        y>=p->y && y<p->y+p->height) {
      assert(!p->high_priority);++count;
    }
  }
  return count;
}

static unsigned RegionalSpikeCells(const RegionalSpikeFixture *spec,
    const ActionRoomScene *s,const ActionRoomScene *us,const ActionVoxelGeometry *g,
    const ActionVoxelGeometry *us_geometry,unsigned region) {
  int reference_tx,reference_ty;assert(FixtureCell(us,spec->id,&reference_tx,&reference_ty));
  VoxelCellGeometry reference_shape;
  assert(ActionVoxel_CellGeometry(us_geometry,reference_tx/2,reference_ty/2,&reference_shape));
  assert(!reference_shape.authored);
  uint16_t reference_words[4];uint8_t reference_pixels[256];uint32_t reference_colors[256];
  for(unsigned q=0;q<4;++q)
    assert(ActionRoomScene_LookupTile(us,1,reference_tx+(q&1),reference_ty+(q>>1),&reference_words[q],NULL));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    reference_pixels[y*16+x]=(uint8_t)FixturePixel(us,reference_tx*8+x,reference_ty*8+y);
  DesertNativeLow(us,reference_tx,reference_ty,reference_colors);
  assert(RegionalSpikeCollision(s,spec->id)==(region==2?15u:0u));
  const ActionRoomSceneFrameState frame={.brightness=15};
  unsigned cells=0,ink=0,front_ink=0,rear_ink=0;
  for(int ty=0;ty<ActionRoomScene_TileHeight(s,1);ty+=2)
    for(int tx=0;tx<ActionRoomScene_TileWidth(s,1);tx+=2) {
      uint16_t word;uint8_t id;
      assert(ActionRoomScene_LookupTile(s,1,tx,ty,&word,&id));
      if(id!=spec->id)continue;
      ++cells;
      VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(g,tx/2,ty/2,&shape));
      assert(!shape.authored && shape.width==16 && shape.offset_x==0);
      for(unsigned q=0;q<4;++q) {
        assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&word,NULL));
        /* Composed source words, palette/priority and displayed ink agree;
         * native collision lives in separate raw definition bits. */
        assert(word==reference_words[q] && !(word&0x2000));
        assert(shape.front[q]==reference_shape.front[q] && shape.back[q]==reference_shape.back[q]);
        if(spec->kind==kVoxelRearOnly)
          assert(shape.front[q]==ActionVoxel_Depth(g) && shape.back[q]==ActionVoxel_Depth(g));
      }
      const size_t semantic=(size_t)(ty*8-g->origin_y)/16*(g->width/16)+(tx*8-g->origin_x)/16;
      assert(g->semantic_cells[semantic]==spec->semantic);
      uint32_t native[256],rear[256],front[256];DesertNativeLow(s,tx,ty,native);
      assert(!memcmp(native,reference_colors,sizeof(native)));
      assert(ActionVoxel_Split(g,tx*8,ty*8,16,16,16,&frame,native,16,rear,front));
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
        const unsigned p=FixturePixel(s,tx*8+px,ty*8+py),at=py*16+px;
        assert(p==reference_pixels[at]);
        const unsigned m=g->front[RegionalSpikePixelAt(g,tx*8+px,ty*8+py)];
        const unsigned expected=p ? (spec->kind==kVoxelFoliage && p==1?kVoxelRearOnly:spec->kind) : 0;
        if((m&kVoxelKindMask)!=expected)
          fprintf(stderr,"regional spike %02u/%02u terrain%u cell%d,%d pixel%u,%u: role%u expected%u nativecollision%u\n",
              spec->group,spec->room,region,tx/2,ty/2,px,py,m&kVoxelKindMask,expected,RegionalSpikeCollision(s,id));
        /* This rejects the demonstrated solid extrusion, not just a table
         * flag. Native collision remains15 in EU even on rear/thin art. */
        assert((m&kVoxelKindMask)==expected);
        assert(!(m&kVoxelHighPriority));
        assert(!!(m&kVoxelColliding)==(p && region==2));
        assert(!(m&kVoxelTexturedTop));
        assert(front[at]==(expected && expected!=kVoxelRearOnly?native[at]:0));
        assert(rear[at]==(expected==kVoxelFoliage?0:native[at]));
        assert(!(front[at] && rear[at]));
        ink+=p!=0;front_ink+=front[at]!=0;rear_ink+=rear[at]!=0;
      }
      for(unsigned q=0;q<4;++q)
        assert(RegionalSpikePatchOwners(g,tx*8+(q&1)*8,ty*8+(q>>1)*8)==(spec->kind==kVoxelFoliage?1u:0u));
      /* The cave's native pointed upper row is independently rear-only.
       * Do not invent a foliage/physical hazard contract for that profile. */
      if(spec->group==1) {
        assert(ty>=2);
        uint8_t above;assert(ActionRoomScene_LookupTile(s,1,tx,ty-2,&word,&above) && above==0x18);
        assert(RegionalSpikeCollision(s,above)==0);
        for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
          const unsigned m=g->front[RegionalSpikePixelAt(g,tx*8+px,(ty-2)*8+py)];
          assert((m&kVoxelKindMask)==(FixturePixel(s,tx*8+px,(ty-2)*8+py)?kVoxelRearOnly:0));
        }
      }
    }
  assert(cells==spec->count[region] && ink && rear_ink);
  if(spec->kind==kVoxelFoliage)assert(front_ink);else assert(!front_ink);
  /* No solid walking cap or side sheet may be emitted from these source
   * pixels. Adjacent bank contours remain owned by their bank pixels. */
  unsigned edge_count;const VoxelEdge *edges=ActionVoxel_Edges(g,&edge_count);
  for(unsigned i=0;i<edge_count;++i)for(int j=0;j<edges[i].length;++j) {
    const int x=edges[i].x+((edges[i].side&1)?0:j),y=edges[i].y+((edges[i].side&1)?j:0);
    assert(!RegionalSpikeAt(s,spec->id,x,y));
  }
  return cells;
}

static void RegionalSpikeSameProfile(const RegionalSpikeFixture *spec,
    const ActionRoomScene *eu,const ActionVoxelGeometry *actual,
    const ActionVoxelGeometry *normalized) {
  /* Only the target raw collision bits differ in this SAME-profile oracle.
   * All observable geometry, source splitting and unrelated pixel roles
   * must agree. Global edge equality also protects adjoining bank caps. */
  assert(actual->origin_x==normalized->origin_x && actual->origin_y==normalized->origin_y &&
      actual->width==normalized->width && actual->height==normalized->height);
  SameGeometry(actual,normalized);
  const size_t area=(size_t)actual->width*actual->height;
  for(size_t at=0;at<area;++at) {
    const int x=actual->origin_x+(int)(at%actual->width),y=actual->origin_y+(int)(at/actual->width);
    const unsigned a=actual->front[at],b=normalized->front[at];
    if(RegionalSpikeAt(eu,spec->id,x,y)) {
      assert((a&~kVoxelColliding)==b);
      assert(!!(a&kVoxelColliding)==!!b);
    } else assert(a==b);
  }
  assert(!memcmp(actual->semantic_cells,normalized->semantic_cells,area/256));
  assert(!!actual->cutouts==!!normalized->cutouts);
  if(actual->cutouts)assert(!memcmp(actual->cutouts,normalized->cutouts,area/8));
  const ActionRoomSceneFrameState frame={.brightness=15};
  uint32_t input[256],ar[256],af[256],br[256],bf[256];
  for(unsigned i=0;i<256;++i)input[i]=UINT32_C(0xff010000)+i;
  for(int y=actual->origin_y;y<actual->origin_y+actual->height;y+=16)
    for(int x=actual->origin_x;x<actual->origin_x+actual->width;x+=16) {
      RegionalSpikeCellEqual(actual,normalized,x/16,y/16);
      assert(ActionVoxel_Split(actual,x,y,16,16,16,&frame,input,16,ar,af));
      assert(ActionVoxel_Split(normalized,x,y,16,16,16,&frame,input,16,br,bf));
      assert(!memcmp(ar,br,sizeof(ar)) && !memcmp(af,bf,sizeof(af)));
    }
}

static void RegionalSpikeEdits(const RegionalSpikeFixture *spec,const ActionRoomScene *s,
    const ActionVoxelGeometry *base,ActionVoxelGeometry *edited,ActionVoxelSettings settings) {
  int tx,ty;assert(FixtureCell(s,spec->id,&tx,&ty));
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  unsigned first=256;
  for(unsigned y=0;y<16 && first==256;++y)for(unsigned x=0;x<16;++x) {
    const unsigned p=FixturePixel(s,tx*8+x,ty*8+y);
    if(p && (spec->kind!=kVoxelFoliage || p!=1)){first=y*16+x;break;}
  }
  assert(first<256);
  terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *mask=&terrain.pixel_layers[0].edits[0];
  *mask=(DioramaPixelEdit){.by_cell=true,.x=tx/2,.y=ty/2};
  mask->transparent[first/16]=(uint16_t)(1u<<(15-first%16));
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  const size_t removed=RegionalSpikePixelAt(base,tx*8+first%16,ty*8+first/16);
  const size_t area=(size_t)base->width*base->height;
  assert(base->width==edited->width && base->height==edited->height &&
      base->origin_x==edited->origin_x && base->origin_y==edited->origin_y);
  for(size_t i=0;i<area;++i)assert(edited->front[i]==(i==removed?0:base->front[i]));
  const ActionRoomSceneFrameState frame={.brightness=15};
  uint32_t low[256],rear[256],front[256],before_rear[256],before_front[256];
  DesertNativeLow(s,tx,ty,low);
  assert(ActionVoxel_Split(base,tx*8,ty*8,16,16,16,&frame,low,16,before_rear,before_front));
  low[first]=0; /* The authored source mask is applied before composition. */
  assert(ActionVoxel_Split(edited,tx*8,ty*8,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i) {
    assert(rear[i]==(i==first?0:before_rear[i]));
    assert(front[i]==(i==first?0:before_front[i]));
  }
  /* Explicit authored depth and visibility win without modifying native
   * scene/collision or adjacent cell depth. Clearing the edit restores it. */
  memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
  DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=tx/2,.y=ty/2,
      .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|kDioramaVoxelCell_Faces,
      .front=.03f,.back=.13f,.faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front};
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  VoxelCellGeometry shape;assert(ActionVoxel_CellGeometry(edited,tx/2,ty/2,&shape) && shape.authored);
  for(unsigned q=0;q<4;++q)assert(shape.front[q]==.03f && shape.back[q]==.13f);
  assert(shape.faces==cell->faces);
  const int dx[]={-1,1,0,0},dy[]={0,0,-1,1};
  for(unsigned n=0;n<4;++n) {
    const int x=tx/2+dx[n],y=ty/2+dy[n];
    if(x>=base->origin_x/16 && y>=base->origin_y/16 &&
        x<(base->origin_x+base->width)/16 && y<(base->origin_y+base->height)/16)
      RegionalSpikeCellEqual(base,edited,x,y);
  }
  memset(&terrain,0,sizeof(terrain));
  assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(base,edited);
  assert(!memcmp(base->front,edited->front,area));
}

static void RegionalSpikeFixtures(const char *directory) {
  if(!directory)return;
  static const RegionalSpikeFixture specs[]={
    {1,2,0x20,kVoxelRearOnly,kVoxelSemantic_Rear,{20,20,20}},
    {6,2,0x3f,kVoxelFoliage,kVoxelSemantic_Hazard,{17,44,17}},
    {6,3,0x3f,kVoxelFoliage,kVoxelSemantic_Hazard,{14,14,14}}
  };
  static ActionSceneSnapshot fixture,us_fixture;
  static ActionRoomScene original,variant;
  unsigned european_cells=0;
  for(unsigned n=0;n<sizeof(specs)/sizeof(*specs);++n) {
    const RegionalSpikeFixture *spec=&specs[n];
    RegionalSpikeLoad(directory,0,spec,&us_fixture);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(spec->group,spec->room);
    ActionVoxelGeometry *us=ActionVoxel_Create(),*base=ActionVoxel_Create(),*other=ActionVoxel_Create();
    assert(us&&base&&other);
    assert(ActionVoxel_Build(us,&us_fixture.scene,NULL,settings));
    for(unsigned region=0;region<3;++region) {
      RegionalSpikeLoad(directory,region,spec,&fixture);original=fixture.scene;
      const ActionRoomScene *s=&fixture.scene;
      assert(ActionVoxel_Build(base,s,NULL,settings));
      const unsigned checked=RegionalSpikeCells(spec,s,&us_fixture.scene,base,us,region);
      if(region==2) {
        european_cells+=checked;variant=*s;
        for(unsigned q=0;q<4;++q) {
          assert((variant.bg[0].metatiles[spec->id*8+q*2]&2) &&
              !(us_fixture.scene.bg[0].metatiles[spec->id*8+q*2]&2));
          variant.bg[0].metatiles[spec->id*8+q*2]&=(uint8_t)~2u;
        }
        assert(RegionalSpikeCollision(s,spec->id)==15 && RegionalSpikeCollision(&variant,spec->id)==0);
        assert(ActionVoxel_Build(other,&variant,NULL,settings));
        RegionalSpikeSameProfile(spec,s,base,other);
      }
      variant=*s;Renumber(&variant);
      assert(ActionVoxel_Build(other,&variant,NULL,settings));SameGeometry(base,other);
      assert(base->width==other->width && base->height==other->height &&
          !memcmp(base->front,other->front,(size_t)base->width*base->height));
      RegionalSpikeEdits(spec,s,base,other,settings);
      assert(!memcmp(s,&original,sizeof(original)));
      assert(RegionalSpikeCollision(s,spec->id)==(region==2?15u:0u));
      printf("regional spikes %02u/%02u terrain%u: %u native cells, exact source/priority/split roles, no solid extrusion, immutable collision, remapping and authored precedence\n",
          spec->group,spec->room,region,checked);
    }
    ActionVoxel_Release(us);ActionVoxel_Release(base);ActionVoxel_Release(other);
  }
  assert(european_cells==20+17+14);
}
