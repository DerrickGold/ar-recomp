/* A complete native head shares one shallow extrusion, including the crown
 * that remains a platform. Do not put its facial features on the rear sheet. */
static void CavernHeadChecks(const ActionRoomScene *s,const ActionVoxelGeometry *native,
    ActionVoxelGeometry *edited,ActionVoxelSettings settings,unsigned region) {
  if(s->map!=6)return;
  static DioramaRoomOverride manifest,terrain;
  DioramaLayerOrder_ClearRoom(&manifest);DioramaLayerOrder_ClearRoom(&terrain);
  FILE *file=fopen("diorama-layers.ini","r");assert(file);char line[4096];bool active=false;
  while(fgets(line,sizeof(line),file)) {
    line[strcspn(line,"\r\n")]=0;
    if(line[0]=='['){active=!strcmp(line,"[layers:04:06]");continue;}
    if(!active||strncmp(line,"voxel-cell:",11))continue;
    const char *error=NULL;assert(DioramaLayerOrder_ParseLine(&manifest,line,&error));
  }
  assert(!ferror(file));fclose(file);
  assert(DioramaLayerOrder_ForTerrain(&manifest,region,&terrain)&&terrain.voxel.cell_count==35);
  assert(ActionVoxel_Build(edited,s,&terrain,settings));
  const ActionRoomSceneFrameState frame={.brightness=15};unsigned cells=0,crowns=0;
  for(int y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
    for(int x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
      const bool crown=id>=0x38&&id<=0x3b;
      const bool head=crown||(id>=0x40&&id<=0x43)||(id>=0x48&&id<=0x4b)||id==0x51||id==0x52;
      const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,x,y);assert(!!a==head);
      VoxelCellGeometry before,after;
      assert(ActionVoxel_CellGeometry(native,x,y,&before)&&ActionVoxel_CellGeometry(edited,x,y,&after));
      if(!head) {
        assert(!memcmp(before.front,after.front,sizeof(before.front))&&
            !memcmp(before.back,after.back,sizeof(before.back)));continue;
      }
      ++cells;crowns+=crown;assert(a->surface&&!a->native_front_art&&after.authored);
      for(unsigned q=0;q<4;++q) {
        assert(ActionRoomScene_LookupTile(s,1,x*2+(q&1),y*2+(q>>1),&word,NULL));
        assert(a->priority[q]==!!(word&0x2000));
        assert(fabsf(after.front[q]-.07f)<.00001f&&fabsf(after.back[q]-.16f)<.00001f);
        /* Native statue fire is in OBJ2 (.51 minus the walking recess).
         * Mount the mouth at that same world Z, keeping its original 0.09
         * thickness instead of introducing a special sprite/effect offset. */
        assert(fabsf(after.front[q]-(settings.play_recess-.01f))<.00001f);
        assert(fabsf(after.back[q]-after.front[q]-.09f)<.00001f);
        assert(after.front[q]<settings.play_recess&&after.back[q]>settings.play_recess);
      }
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
        const unsigned at=py*16+px;
        assert(ActionVoxel_AuthoredColor(a->pixels[0][at],&frame)==ForestNativeColor(s,x*16+px,y*16+py,&frame));
        assert(a->pixels[4][at]==a->pixels[0][at]&&a->pixels[5][at]==a->pixels[0][at]);
        assert(!a->pixels[3][at]); /* No duplicate face left on the rear wall. */
      }
      uint32_t source[256],rear[256],front[256];
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
        source[py*16+px]=ForestNativeColor(s,x*16+px,y*16+py,&frame);
      assert(ActionVoxel_Split(edited,x*16,y*16,16,16,16,&frame,source,16,rear,front));
      for(unsigned p=0;p<256;++p)assert(!rear[p]&&!front[p]);
    }
  assert(cells==35&&crowns==10);
  printf("cavern heads terrain%u: five complete native heads, crowns/faces/chins aligned, exact source art/priority and unchanged neighbours passed\n",region);
}
