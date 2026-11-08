#include "action/action_voxel_internal.h"

/* Native91 andCA are identical bark with different collision bytes. The four
 * noncolliding91 cells still belong to the continuous entrance trunk body;
 * collision must not turn every other row into a recessed decorative panel. */
static void SnowTrunkFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot fixture;static ActionRoomScene original;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0605.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));original=fixture.scene;
    const ActionRoomScene *s=&fixture.scene;assert(s->group==6 && s->map==5);
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,5);
    assert(ActionVoxel_Build(base,s,NULL,settings));
    int body_tx,body_ty;assert(FixtureCell(s,0xca,&body_tx,&body_ty));
    VoxelCellGeometry body;assert(ActionVoxel_CellGeometry(base,body_tx/2,body_ty/2,&body));
    unsigned cells=0,side_pixels=0;
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
        uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
        if(id!=0x91)continue;
        assert(x==43 && (y==11 || y==13 || y==15 || y==17));++cells;
        VoxelCellGeometry cell;assert(ActionVoxel_CellGeometry(base,x,y,&cell));
        for(unsigned q=0;q<4;++q) {
          uint16_t body_word;
          assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&word,NULL));
          assert(ActionRoomScene_LookupTile(s,1,body_tx+(q&1),body_ty+(q>>1),&body_word,NULL));
          assert(word==body_word && !(word&0x2000));
          assert(!(s->bg[0].metatiles[0x91*8+q*2]&2));
          assert(s->bg[0].metatiles[0xca*8+q*2]&2);
          assert(cell.front[q]==0 && fabsf(cell.back[q]-.2f)<.000001f);
          assert(cell.front[q]==body.front[q] && cell.back[q]==body.back[q]);
        }
        uint32_t native[256],rear[256],front[256];
        DesertNativeLow(s,x*2,y*2,native);
        const ActionRoomSceneFrameState frame={.brightness=15};
        assert(ActionVoxel_Split(base,x*16,y*16,16,16,16,&frame,native,16,rear,front));
        assert(!memcmp(front,native,sizeof(front)));
        for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
          assert(FixturePixel(s,x*16+px,y*16+py)==FixturePixel(s,body_tx*8+px,body_ty*8+py));
          const unsigned material=base->front[(y*16+py-base->origin_y)*base->width+x*16+px-base->origin_x];
          assert((material&kVoxelKindMask)==kVoxelSolid);
          assert(!(material&(kVoxelColliding|kVoxelHighPriority)));
        }
        unsigned edge_count;const VoxelEdge *edges=ActionVoxel_Edges(base,&edge_count);
        for(unsigned i=0;i<edge_count;++i) {
          const VoxelEdge *e=&edges[i];
          /* Both row joins are internal to the same solid bark body. No top
           * or underside can remain to produce a tooth at either boundary. */
          if(!(e->side&1) && e->x<(int)(x+1)*16 && e->x+e->length>(int)x*16) {
            const int edge_y=e->y+(e->side==2);
            assert(edge_y!=(int)y*16 && edge_y!=(int)(y+1)*16);
          }
          if((e->side&1) && e->x==(int)x*16 && e->y<(int)(y+1)*16 && e->y+e->length>(int)y*16) {
            assert(e->far_rank==2 && !e->colliding && !e->high_priority);
            ++side_pixels;
          }
        }
      }
    assert(cells==4 && side_pixels);
    /* The adjacent outline and separate lower bark retain their shallow
     * scenery contract; this is not a room-wide tree depth change. */
    VoxelCellGeometry outline;assert(ActionVoxel_CellGeometry(base,42,11,&outline));
    for(unsigned q=0;q<4;++q)assert(fabsf(outline.front[q]-.15f)<.000001f &&
        fabsf(outline.back[q]-.2f)<.000001f);
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    DioramaVoxelCell *edit=&terrain.voxel.cells[0];
    *edit=(DioramaVoxelCell){.x=43,.y=11,.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,
        .front=.07f,.back=.18f};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,43,11);assert(a&&a->surface);
    for(unsigned q=0;q<4;++q)assert(a->front[q]==edit->front && a->back[q]==edit->back && !a->priority[q]);
    VoxelCellGeometry neighbor;assert(ActionVoxel_CellGeometry(edited,43,12,&neighbor));
    for(unsigned q=0;q<4;++q)assert(neighbor.front[q]==0 && fabsf(neighbor.back[q]-.2f)<.000001f);
    *edit=(DioramaVoxelCell){.x=43,.y=11,.fields=kDioramaVoxelCell_Faces,
        .faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Front};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    a=ActionVoxel_AuthoredCellAt(edited,43,11);
    assert(a && a->native_front_hidden && a->faces==edit->faces);SameGeometry(base,edited);
    edit->faces=kDioramaVoxelFaceMask_All;
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    assert(!ActionVoxel_AuthoredCellAt(edited,43,11));SameGeometry(base,edited);
    assert(!memcmp(s,&original,sizeof(original)));
    printf("Northwall entrance trunk terrain%u: four noncolliding bark cells share native body depth, no internal ledges; artwork, priority, outline and authored edits preserved\n",region);
    ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
}
