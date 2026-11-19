/* Shipped level edits narrow the wall fixtures, without rewriting their
 * source picture, native collision, sprite priority or adjoining masonry. */
static void TempleCarvingChecks(const ActionRoomScene *s,const ActionVoxelGeometry *native,
    ActionVoxelGeometry *edited,ActionVoxelSettings settings,unsigned region) {
  if(s->map!=3)return;
  static DioramaRoomOverride manifest,terrain;
  DioramaLayerOrder_ClearRoom(&manifest);DioramaLayerOrder_ClearRoom(&terrain);
  FILE *file=fopen("diorama-layers.ini","r");assert(file);char line[4096];bool active=false,found=false;
  while(fgets(line,sizeof(line),file)) {
    line[strcspn(line,"\r\n")]=0;
    if(line[0]=='['){active=!strcmp(line,"[layers:01:03]");found|=active;continue;}
    if(!active || !line[0] || line[0]=='#' || line[0]==';')continue;
    const char *error=NULL;assert(DioramaLayerOrder_ParseLine(&manifest,line,&error));
  }
  assert(!ferror(file) && found);fclose(file);
  assert(DioramaLayerOrder_ForTerrain(&manifest,region,&terrain));assert(terrain.voxel.cell_count==24);
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  const ActionRoomSceneFrameState frame={.brightness=15};unsigned cells=0;
  for(int y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
    for(int x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
      const bool head=id==0x28 || id==0x29 || id==0x30 || id==0x31;
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,x,y);
      assert(!!a==head);
      VoxelCellGeometry before,after;
      assert(ActionVoxel_CellGeometry(native,x,y,&before) && ActionVoxel_CellGeometry(edited,x,y,&after));
      if(!head) {
        assert(!memcmp(before.front,after.front,sizeof(before.front)) &&
            !memcmp(before.back,after.back,sizeof(before.back)));continue;
      }
      ++cells;assert(a->surface && !a->native_front_art && after.authored);
      for(unsigned q=0;q<4;++q) {
        assert(fabsf(after.front[q]-.03f)<.00001f && fabsf(after.back[q]-settings.depth*.5f)<.00001f);
        assert(a->priority[q]==1 && a->back[q]>settings.play_recess);
      }
      assert((a->automatic_art & ((1u<<4)|(1u<<5))) == ((1u<<4)|(1u<<5)));
      assert(!a->surface_sample_size[4] && !a->surface_sample_size[5]);
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
        const unsigned at=py*16+px;
        assert(ActionVoxel_AuthoredColor(a->pixels[0][at],&frame)==ForestNativeColor(s,x*16+px,y*16+py,&frame));
        assert(a->pixels[4][at]==a->pixels[0][at] && a->pixels[5][at]==a->pixels[0][at]);
      }
    }
  assert(cells==24);
  printf("temple carvings terrain%u: 12 inset fixtures, native pixel extrusion, exact fronts/priority and unchanged neighbours passed\n",region);
}
