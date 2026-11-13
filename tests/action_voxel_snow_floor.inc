#include "action/action_voxel_internal.h"

static bool SnowFloorDetail(unsigned id) {
  static const uint8_t ids[]={2,3,4,5,6,7,8,9,10,11,16,17,36,37,38,39,44,45,46,
      48,49,50,51,54,57,58,59,60,61,68,82,134};
  for(unsigned i=0;i<sizeof(ids);++i)if(id==ids[i])return true;
  return false;
}
static bool SnowFloorSlope(unsigned id) {
  static const uint8_t ids[]={0x0c,0x0f,0x1c,0x1d,0x1e,0x1f,0x2a,0x2b,0x34,0x35};
  for(unsigned i=0;i<sizeof(ids);++i)if(id==ids[i])return true;
  return false;
}
static unsigned SnowFloorCollision(const ActionRoomScene *s,int x,int y) {
  uint16_t word;uint8_t id;
  if(!ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id))return 0;
  unsigned result=0;
  for(unsigned q=0;q<4;++q)result|=!!(s->bg[0].metatiles[id*8+q*2]&2)<<q;
  return result;
}
/* Native $00:9084/$90A9: neighbouring collision codes select half grades;
 * an isolated source tile uses the full grade. Artwork is not the contact. */
static unsigned SnowFloorContact(unsigned collision,unsigned left,unsigned right,unsigned x) {
  assert(collision==6 || collision==9);
  unsigned height=(collision==6?x:~x)&14;
  if((collision==6?left:right)==collision)height=height/2+8;
  else if((collision==6?right:left)==collision)height/=2;
  return 16-height;
}
static uint16_t SnowFloorFoot(const ActionRoomScene *s,int x,int y,unsigned id) {
  if(id!=0x33 && id!=0x3d)return 0;
  uint16_t word;uint8_t above;
  if(!y || !ActionRoomScene_LookupTile(s,1,x*2,(y-1)*2,&word,&above) ||
      !SnowFloorSlope(above))return 0;
  const unsigned collision=SnowFloorCollision(s,x,y-1),left=SnowFloorCollision(s,x-1,y-1),
      right=SnowFloorCollision(s,x+1,y-1);
  uint16_t result=0;
  for(unsigned px=0;px<16;++px) {
    const unsigned contact=SnowFloorContact(collision,left,right,px);
    const bool native_gap=contact==15 && !FixturePixel(s,x*16+px,(y-1)*16+15);
    if(contact==16 || native_gap)result|=1u<<(15-px);
  }
  return result;
}
static void SnowFloorSlopes(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    const uint8_t *tops,unsigned region) {
  unsigned cells=0,ascending=0,descending=0,contacts=0,rear_pixels=0,outline_offsets=0;
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
      if(!SnowFloorSlope(id))continue;
      const unsigned collision=SnowFloorCollision(s,x,y),left=SnowFloorCollision(s,(int)x-1,y),
          right=SnowFloorCollision(s,x+1,y);
      ++cells;ascending+=collision==6;descending+=collision==9;
      uint32_t native[256],rear[256],front[256];
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
        native[py*16+px]=ForestNativeColor(s,x*16+px,y*16+py,&frame);
      assert(ActionVoxel_Split(g,x*16,y*16,16,16,16,&frame,native,16,rear,front));
      for(unsigned px=0;px<16;++px) {
        const unsigned contact=SnowFloorContact(collision,left,right,px);
        for(unsigned py=0;py<16;++py) {
          const size_t at=(y*16+py-g->origin_y)*g->width+x*16+px-g->origin_x;
          const unsigned m=g->front[at];
          if(m) {
            assert((m&kVoxelKindMask)==(py<contact?kVoxelRearOnly:kVoxelSolid));
            assert(!!(m&kVoxelColliding)==!!(collision&(1u<<((py/8)*2+px/8))));
            assert(!(m&kVoxelHighPriority));
          }
          /* The former half-cell shelves must contribute no walking cap. */
          if(py<contact)assert(!tops[at]);
          assert(rear[py*16+px]==native[py*16+px]);
          assert(front[py*16+px]==(py<contact?0:native[py*16+px]));
          rear_pixels+=py<contact;
        }
        unsigned surface_y=contact;
        if(contact<16 && !(native[contact*16+px]>>24)) {
          /* The two native35 tips have one transparent contour pixel at
           * the physics contact. Preserve it; the cap begins on the first
           * actual opaque snow pixel one row below, not synthetic ink. */
          assert(id==0x35 && px==13 && contact==15);
          const size_t hole=(y*16+contact-g->origin_y)*g->width+x*16+px-g->origin_x;
          assert(!g->front[hole] && !tops[hole]);
          assert(FixturePixel(s,x*16+px,y*16+contact+1));
          ++surface_y;++outline_offsets;
        }
        const size_t at=(y*16+surface_y-g->origin_y)*g->width+x*16+px-g->origin_x;
        const VoxelTrunkCurve *rock=ActionVoxel_ColumnCurveForCell(g,(x*16+px)/8,(y*16+surface_y)/8);
        const unsigned cap=rock?1:2;
        if((g->front[at]&kVoxelKindMask)!=kVoxelSolid || !(tops[at]&cap))
          fprintf(stderr,"snow contact region%u cell%u,%u id%02x collision%u left%u right%u pixel%u,%u role%u top%u\n",
              region,x,y,id,collision,left,right,px,surface_y,g->front[at]&kVoxelKindMask,tops[at]);
        assert((g->front[at]&kVoxelKindMask)==kVoxelSolid && (tops[at]&cap));++contacts;
      }
    }
  assert(cells==58 && ascending==22 && descending==36 && contacts==928 && outline_offsets==2);
  printf("Northwall native ramps terrain%u: %u cells (%u ascending/%u descending), %u exact contact caps + %u preserved one-pixel native outline offsets, %u rear source pixels; no raised artwork shelves\n",
      region,cells,ascending,descending,contacts-outline_offsets,outline_offsets,rear_pixels);
}
/* Snow decoration above the bank belongs against its rear. Native solid
 * contacts expose a floor in front of it; decoration above spikes does not
 * become an invented walkable bridge. */
static void SnowFloorFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot fixture;static ActionRoomScene original,variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  static DioramaRoomOverride terrain;
  uint16_t hazard_words[3][4];uint8_t hazard_pixels[3][256];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0601.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));original=fixture.scene;
    const ActionRoomScene *s=&fixture.scene;
    if(!region)for(unsigned part=0;part<3;++part) {
      int tx,ty;assert(FixtureCell(s,0x2f+part*8,&tx,&ty));
      for(unsigned q=0;q<4;++q)
        assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&hazard_words[part][q],NULL));
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
        hazard_pixels[part][py*16+px]=FixturePixel(s,tx*8+px,ty*8+py);
    }
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,1);
    assert(ActionVoxel_Build(base,s,NULL,settings));assert(base->mixed_scenery);
    const size_t area=(size_t)base->width*base->height;
    uint8_t *top=calloc(area,1);assert(top);
    unsigned edge_count;const VoxelEdge *edges=ActionVoxel_Edges(base,&edge_count);
    unsigned shared_caps=0;
    for(unsigned i=0;i<edge_count;++i) {
      const VoxelEdge *e=&edges[i];
      assert(!e->flat_rear_join && !e->rear_fold_top && !e->rear_fold_bottom);
      if(!e->side && e->far_rank==2) {
        VoxelEdgeSpan native_span;
        const bool full_depth=ActionVoxel_ResolveEdgeSpanAtY(base,e,kVoxelEdgeNative,
            0,ActionVoxel_Depth(base),e->y,&native_span) && fabsf(native_span.near_depth)<.000001f &&
            fabsf(native_span.far_depth-ActionVoxel_Depth(base))<.000001f;
        for(int x=e->x;x<e->x+e->length;++x)
          top[(size_t)(e->y-base->origin_y)*base->width+x-base->origin_x]|=1u|(full_depth?2u:0u);
        if(e->near_rank==1 && e->neighbor_rear_mounted) {
          VoxelEdgeSpan span;
          const VoxelEdgePass pass=e->high_priority&&e->colliding?kVoxelEdgeHighDeckBehind:kVoxelEdgeNative;
          assert(ActionVoxel_ResolveEdgeSpanAtY(base,e,pass,0,ActionVoxel_Depth(base),e->y,&span));
          assert(fabsf(span.far_depth-.15f)<.000001f && span.near_depth<span.far_depth);
          shared_caps+=e->length;
        }
      }
      assert(!(e->far_rank==1 && e->x>=156*16 && e->x<157*16 && e->y>=36*16 && e->y<40*16));
    }
    SnowFloorSlopes(s,base,top,region);
    unsigned expected_caps=0,sloped_caps=0;
    for(int y=1;y<base->height;++y)for(int x=0;x<base->width;++x) {
      const size_t at=(size_t)y*base->width+x;
      const unsigned above=base->front[at-base->width]&kVoxelKindMask;
      if((base->front[at]&kVoxelKindMask)!=kVoxelSolid || above==kVoxelSolid)continue;
      if(above==kVoxelForeground &&
          ActionVoxel_FrontRecess(base,(x+base->origin_x)/8,(y-1+base->origin_y)/8)<.149f)continue;
      /* Every individual source boundary must exist, including diagonal
       * one-pixel steps and native noncolliding slope quadrants. */
      assert(top[at]);
      if(above==kVoxelForeground)++expected_caps;
      else if(y&15)++sloped_caps;
    }
    assert(expected_caps && sloped_caps && shared_caps==expected_caps);
    unsigned details=0,hazards=0,pits=0,pit_floor_columns=0,foot_pixels=0,rock_bases=0;
    int8_t hazard_art[256];memset(hazard_art,-1,sizeof(hazard_art));
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
        uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
        /* Match the verified native spike artwork, not the metatile number
         * or collision byte: EU has the same lower spike art with collision15. */
        if(hazard_art[id]<0) {
          hazard_art[id]=0;
          for(unsigned part=0;part<3;++part) {
            bool same=true;
            for(unsigned q=0;q<4;++q) {
              uint16_t candidate;assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&candidate,NULL));
              same&=candidate==hazard_words[part][q];
            }
            if(same)for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
              same&=FixturePixel(s,x*16+px,y*16+py)==hazard_pixels[part][py*16+px];
            if(same)hazard_art[id]=part+1;
          }
        }
        const bool detail=SnowFloorDetail(id),hazard=hazard_art[id]>0;
        if(!detail && !hazard && id!=0x7b)continue;
        details+=detail;hazards+=hazard;
        const uint16_t foot=SnowFloorFoot(s,x,y,id);
        VoxelCellGeometry cell;assert(ActionVoxel_CellGeometry(base,x,y,&cell));
        for(unsigned q=0;q<4;++q) {
          if(!hazard)assert(!(s->bg[0].metatiles[id*8+q*2]&2));
          assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&word,NULL));
          assert(!(word&0x2000));
          if(detail) {
            const VoxelTrunkCurve *rock=ActionVoxel_ColumnCurveForCell(base,x*2+(q&1),y*2+(q>>1));
            bool modeled=false,foot_quadrant=false;
            const uint8_t *recess=ActionVoxel_PixelRecessRows(base,x*2+(q&1),y*2+(q>>1));
            for(unsigned py=0;py<8;++py)for(unsigned px=0;px<8;++px) {
              const unsigned m=base->front[(y*16+(q>>1)*8+py-base->origin_y)*base->width+x*16+(q&1)*8+px-base->origin_x];
              const bool contact=q<2 && !py && (foot&(1u<<(15-(q&1)*8-px)));
              assert(!(m&(kVoxelHighPriority|kVoxelColliding)));
              if(!rock)assert(((m&kVoxelKindMask)==kVoxelSolid)==contact);
              if(recess)assert(!!(recess[py]&(1u<<(7-px)))==!contact);
              foot_quadrant|=contact;foot_pixels+=contact;
              modeled|=(m&kVoxelKindMask)==kVoxelForeground;
            }
            if(rock) {
              ++rock_bases;assert(!recess);
              const float front=ActionVoxel_ColumnFrontDepth(base,rock,y*16+(q>>1)*8+4);
              assert(fabsf(cell.front[q]-front)<.000001f && front>=settings.play_recess-.010001f);
              assert(fabsf(ActionVoxel_ColumnFrontDepth(base,rock,(y+1)*16)-.15f)<.000001f);
            } else {
              if(foot_quadrant)assert(recess);
              assert(fabsf(cell.front[q]-(modeled?.15f:.2f))<.000001f);
            }
            assert(fabsf(cell.back[q]-.2f)<.000001f);
          }
        }
        if(id==0x7b) {
          assert(ActionVoxel_FrontRecess(base,x*2,y*2)==ActionVoxel_SceneryRecess(base));
          assert(ActionVoxel_FrontRecess(base,x*2,y*2+1)==ActionVoxel_SceneryRecess(base));
        }
        if(hazard)for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
          const size_t at=(y*16+py-base->origin_y)*base->width+x*16+px-base->origin_x;
          const unsigned kind=base->front[at]&kVoxelKindMask;
          assert(!kind || kind==kVoxelRearOnly || kind==kVoxelFoliage);
          if(hazard_art[id]==1 && kind)assert(kind==kVoxelRearOnly);
          if(base->front[at]) {
            const unsigned q=(py/8)*2+px/8;
            assert(!!(base->front[at]&kVoxelColliding)==!!(s->bg[0].metatiles[id*8+q*2]&2));
          }
          assert(!top[at]); /* No floor cap crosses any native hazard cell. */
        }
        if(hazard_art[id]==1) {
          ++pits;
          for(unsigned px=0;px<16;++px) {
            bool floor=false;
            for(unsigned py=48;py<64;++py)
              floor|=top[(y*16+py-base->origin_y)*base->width+x*16+px-base->origin_x]!=0;
            pit_floor_columns+=floor;
          }
        }
      }
    assert(details && pits && rock_bases && hazards==pits*3 && pit_floor_columns==pits*16 && foot_pixels==10);
    if(!region)assert(details==205 && hazards==90 && pits==30);
    /* The native collision bytes, masks and artwork are immutable, including
     * all slopes. A material remap must not change this semantic policy. */
    variant=*s;Renumber(&variant);
    assert(ActionVoxel_Build(edited,&variant,NULL,settings));SameGeometry(base,edited);
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    terrain.voxel.cells[0]=(DioramaVoxelCell){.x=0,.y=26,
        .fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,.front=.03f,.back=.1f};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    VoxelCellGeometry cell;assert(ActionVoxel_CellGeometry(edited,0,26,&cell) && cell.authored);
    for(unsigned q=0;q<4;++q)assert(cell.front[q]==.03f && cell.back[q]==.1f);
    assert(ActionVoxel_FrontRecess(edited,0,28*2)==0); /* Adjacent bank stays on contact. */
    assert(!memcmp(s,&original,sizeof(original)));
    printf("Northwall snow floor terrain%u: %u walking cap pixels (%u slope pixels), %u rear details, %u open hazard cells/%u pit floor columns and a rear jamb\n",
        region,expected_caps,sloped_caps,details,hazards,pit_floor_columns);
    free(top);ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
}
