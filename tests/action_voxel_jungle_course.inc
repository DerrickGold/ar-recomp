#include "action/action_voxel_internal.h"

/* The same stone artwork has colliding and noncolliding native variants.
 * Both form one platform course; only the lower water quadrants stay rear. */
static void JungleCourseFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot fixture;static ActionRoomScene original;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/050%u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));original=fixture.scene;
    const ActionRoomScene *s=&fixture.scene;
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(5,room);
    assert(ActionVoxel_Build(base,s,NULL,settings));
    unsigned courses=0,tails=0;
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
        uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
        if(id!=0x2d && id!=0x34)continue;
        const bool tail=id==0x34;courses+=!tail;tails+=tail;
        int body_tx,body_ty;assert(FixtureCell(s,tail?0xb0:0xb1,&body_tx,&body_ty));
        VoxelCellGeometry cell,body;
        assert(ActionVoxel_CellGeometry(base,x,y,&cell));
        assert(ActionVoxel_CellGeometry(base,body_tx/2,body_ty/2,&body));
        for(unsigned q=0;q<4;++q) {
          uint16_t body_word;
          assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&word,NULL));
          assert(ActionRoomScene_LookupTile(s,1,body_tx+(q&1),body_ty+(q>>1),&body_word,NULL));
          assert(word==body_word);
          assert(!(s->bg[0].metatiles[id*8+q*2]&2));
          assert(cell.front[q]==body.front[q] && cell.back[q]==body.back[q]);
          assert(fabsf(cell.back[q]-.2f)<.000001f);
          assert(fabsf(cell.front[q]-(tail && q>=2?.2f:0))<.000001f);
          if(!tail || q<2) {
            /* This is the renderer's actual per-quadrant front coordinate,
             * not just the editor's semantic cell summary. */
            assert(ActionVoxel_FrontRecess(base,x*2+(q&1),y*2+(q>>1))==0);
            assert(!ActionVoxel_PixelRecessRows(base,x*2+(q&1),y*2+(q>>1)));
            assert(!ActionVoxel_ColumnCurveForCell(base,x*2+(q&1),y*2+(q>>1)));
            assert(!ActionVoxel_TrunkCurveForCell(base,x*2+(q&1),y*2+(q>>1)));
          }
        }
        const ActionRoomSceneFrameState frame={.brightness=15};
        uint32_t native[256],rear[256],front[256];DesertNativeLow(s,x*2,y*2,native);
        assert(ActionVoxel_Split(base,x*16,y*16,16,16,16,&frame,native,16,rear,front));
        for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
          const bool water=tail && py>=8;
          const unsigned at=py*16+px,q=(py/8)*2+px/8;
          assert(FixturePixel(s,x*16+px,y*16+py)==FixturePixel(s,body_tx*8+px,body_ty*8+py));
          const unsigned material=base->front[(y*16+py-base->origin_y)*base->width+x*16+px-base->origin_x];
          assert((material&kVoxelKindMask)==(water?kVoxelRearOnly:kVoxelSolid));
          assert(!(material&kVoxelColliding));
          assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&word,NULL));
          assert(!!(material&kVoxelHighPriority)==!!(word&0x2000));
          assert(front[at]==(water?0:native[at]));
          assert(rear[at]==native[at]);
        }
      }
    assert(courses==(room==1?4u:room==2?5u:1u));
    assert(tails==(room==3?0u:4u));
    unsigned count,width=0,expected=0;
    const VoxelWaterSurface *water=ActionVoxel_WaterSurfaces(base,&count);
    for(size_t i=0;i<s->bg[0].map_size;++i) {
      const unsigned id=s->bg[0].map[i];
      if(id==1 || id==0x34 || id==0x6e || id==0x6f || id==0xac || id==0xad || id==0xb0 || id==0xb8 || id==0xbf)expected+=16;
    }
    for(unsigned i=0;i<count;++i) {
      assert(water[i].opaque && water[i].high_priority && !water[i].background);
      assert(water[i].sample_y==13 && water[i].sample_height==3);width+=water[i].width;
    }
    assert(width==expected);
    if(room==1) {
      /* Neighboring native26 and the cap have the same actual front plane. */
      for(unsigned y=26;y<=27;++y)for(unsigned q=0;q<4;++q)
        assert(ActionVoxel_FrontRecess(base,38*2+(q&1),y*2+(q>>1))==0);
      memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
      DioramaVoxelCell *edit=&terrain.voxel.cells[0];
      *edit=(DioramaVoxelCell){.x=37,.y=27,.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,
          .front=.04f,.back=.18f};
      assert(ActionVoxel_Build(edited,s,&terrain,settings));
      VoxelCellGeometry cell;assert(ActionVoxel_CellGeometry(edited,37,27,&cell) && cell.authored);
      for(unsigned q=0;q<4;++q)assert(cell.front[q]==edit->front && cell.back[q]==edit->back);
      assert(ActionVoxel_FrontRecess(edited,38*2,27*2)==0);
      *edit=(DioramaVoxelCell){.x=37,.y=27,.fields=kDioramaVoxelCell_Faces,
          .faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front};
      assert(ActionVoxel_Build(edited,s,&terrain,settings));
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,37,27);assert(a&&a->native_front_hidden);
      edit->faces=kDioramaVoxelFaceMask_All;
      assert(ActionVoxel_Build(edited,s,&terrain,settings));
      assert(!ActionVoxel_AuthoredCellAt(edited,37,27));SameGeometry(base,edited);
    }
    assert(!memcmp(s,&original,sizeof(original)));
    printf("Marahna stone course 05/%02u terrain%u: %u bodies/%u stone-water tails aligned; native art, collision, priority, water and authored overrides preserved\n",
        room,region,courses,tails);
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
}
