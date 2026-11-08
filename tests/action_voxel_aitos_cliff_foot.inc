/* The shipped waterfall extension mirrors native rear rock87. Its mirrored
 * artwork also names a foreground gate in the other two mountain rooms. */
static void AitosCliffTerrain(unsigned region,DioramaRoomOverride *resolved) {
  static DioramaRoomOverride manifest;DioramaLayerOrder_ClearRoom(&manifest);
  char line[4096];bool active=false,found=false;FILE *file=fopen("diorama-layers.ini","r");assert(file);
  while(fgets(line,sizeof(line),file)) {
    line[strcspn(line,"\r\n")]=0;char *body=line;
    while(*body==' ' || *body=='\t')++body;
    size_t n=strlen(body);while(n && (body[n-1]==' ' || body[n-1]=='\t'))body[--n]=0;
    if(*body=='['){active=!strncmp(body,"[layers:04:03]",14);found|=active;continue;}
    if(!active || !*body || *body=='#' || *body==';')continue;
    const char *error=NULL;assert(DioramaLayerOrder_ParseLine(&manifest,body,&error));
  }
  assert(!ferror(file) && found);fclose(file);DioramaLayerOrder_ClearRoom(resolved);
  assert(DioramaLayerOrder_ForTerrain(&manifest,region,resolved));
}
static void AitosCliffWords(const ActionRoomScene *s,const uint16_t words[4],uint32_t pixels[256]) {
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned word=words[(y/8)*2+x/8],ink=RockFixtureWordPixel(s,word,x&7,y&7);
    const unsigned at=(((word>>10)&7)*16+ink)*2;
    const uint16_t color=s->palette[at]|s->palette[at+1]<<8;
    pixels[y*16+x]=ink?ActionVoxel_AuthoredColor(0x8000|color,&frame):0;
  }
}
static unsigned AitosCliffRear(const ActionRoomScene *s,const ActionVoxelGeometry *g,
    const DioramaTileStamp *stamp) {
  const ActionRoomSceneFrameState frame={.brightness=15};uint32_t low[256],rear[256],front[256];
  AitosCliffWords(s,stamp->words,low);
  assert(ActionVoxel_Split(g,stamp->x*16,stamp->y*16,16,16,16,&frame,low,16,rear,front));
  unsigned missing=0,ink=0;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned at=y*16+x;
    if(!low[at]){assert(!rear[at] && !front[at]);continue;}
    ++ink;const unsigned role=g->front[(stamp->y*16+y-g->origin_y)*g->width+stamp->x*16+x-g->origin_x];
    assert(!(role&(kVoxelColliding|kVoxelHighPriority)));
    missing+=(role&kVoxelKindMask)!=kVoxelRearOnly || front[at] || rear[at]!=low[at];
  }
  assert(ink==237);return missing;
}
static bool AitosCliffStamp(const ActionRoomScene *s,ActionVoxelGeometry *g,int tx,int ty,bool edits) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  DioramaTileStamp stamp={.x=-2,.y=-3};
  for(unsigned q=0;q<4;++q) {
    assert(ActionRoomScene_LookupTile(s,1,tx+((q&1)^1),ty+(q>>1),&stamp.words[q],&stamp.metatile));
    stamp.words[q]^=0x4000;
  }
  terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,.x0=-2,.y0=-3,.x1=-1,.y1=-2,.cells=&stamp,.count=1};
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,3);
  assert(ActionVoxel_Build(g,s,&terrain,settings));const bool complete=!AitosCliffRear(s,g,&stamp);
  if(!complete || !edits)return complete;
  terrain.voxel.cell_count=1;DioramaVoxelCell *cell=&terrain.voxel.cells[0];
  *cell=(DioramaVoxelCell){.x=-2,.y=-3,.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back|kDioramaVoxelCell_Width,
      .front=.03f,.back=.12f,.width=9};
  assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,-2,-3);
  assert(a && a->width==9);for(unsigned q=0;q<4;++q)assert(a->front[q]==.03f && a->back[q]==.12f);
  const ActionRoomSceneFrameState frame={.brightness=15};uint32_t low[256],rear[256],front[256];
  AitosCliffWords(s,stamp.words,low);
  *cell=(DioramaVoxelCell){.x=-2,.y=-3,.fields=kDioramaVoxelCell_Backing,.backing=kDioramaVoxelBacking_None};
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,-32,-48,16,16,16,&frame,low,16,rear,front));
  for(unsigned p=0;p<256;++p)assert(!rear[p]);
  cell->fields|=kDioramaVoxelCell_BackArt;cell->backing=kDioramaVoxelBacking_Replace;
  cell->backing_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
  for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&cell->backing_art.words[q],NULL));
  uint32_t painted[256];AitosCliffWords(s,cell->backing_art.words,painted);
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,-32,-48,16,16,16,&frame,low,16,rear,front));
  for(unsigned p=0;p<256;++p)assert(rear[p]==painted[p]);
  *cell=(DioramaVoxelCell){.x=-2,.y=-3,.fields=kDioramaVoxelCell_FrontArt,.front_art=cell->backing_art};
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,-2,-3);assert(a && a->surface);
  for(unsigned p=0;p<256;++p)assert(ActionVoxel_AuthoredColor(a->pixels[0][p],&frame)==painted[p]);
  *cell=(DioramaVoxelCell){.x=-2,.y=-3};
  assert(ActionVoxel_Build(g,s,&terrain,settings));assert(!AitosCliffRear(s,g,&stamp));
  terrain.pixel_layers[0].count=1;DioramaPixelEdit *pixel=&terrain.pixel_layers[0].edits[0];
  *pixel=(DioramaPixelEdit){.by_cell=true,.x=-2,.y=-3};pixel->transparent[3]=0x1000;low[51]=0;
  assert(ActionVoxel_Build(g,s,&terrain,settings));
  assert(ActionVoxel_Split(g,-32,-48,16,16,16,&frame,low,16,rear,front));
  for(unsigned p=0;p<256;++p)assert(rear[p]==low[p] && !front[p]);
  return true;
}
static void AitosCliffFootFixtures(const char *directory) {
  if(!directory)return;bool complete=true;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/040%u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(encoded,1,sizeof(encoded),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(encoded,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;original=*s;
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,room);assert(ActionVoxel_Build(base,s,NULL,settings));
    if(room<3) {
      int tx,ty;assert(FixtureCell(s,0x83,&tx,&ty));VoxelCellGeometry cell;assert(ActionVoxel_CellGeometry(base,tx/2,ty/2,&cell));
      for(unsigned q=0;q<4;++q)assert(cell.front[q]==0 && fabsf(cell.back[q]-.05f)<.000001f);
    } else {
      AitosCliffTerrain(region,&terrain);assert(ActionVoxel_Build(edited,s,&terrain,settings));unsigned missing=0,targets=0;
      for(int y=16;y<=20;y+=2) {
        const DioramaTileStamp *stamp=DioramaLayerOrder_StampAt(&terrain.stamp_layers[0],26,y);
        if(region==2){assert(!stamp);continue;}assert(stamp);++targets;missing+=AitosCliffRear(s,edited,stamp);
      }
      complete&=!missing;
      printf("Aitos cliff terrain%u: shipped mirrored rear feet%u, wrong-plane pixels%u; native mask%016llx edges%016llx\n",
          region,targets,missing,(unsigned long long)ForestBackingHash(base->front,(size_t)base->width*base->height),
          (unsigned long long)ForestBackingHash(base->edges,base->edge_count*sizeof(*base->edges)));
      int tx,ty;assert(FixtureCell(s,0x87,&tx,&ty));
      complete&=AitosCliffStamp(s,edited,tx,ty,!missing);
      remapped=*s;Renumber(&remapped);complete&=AitosCliffStamp(&remapped,edited,tx,ty,false);
    }
    assert(!memcmp(s,&original,sizeof(original)));ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
  DioramaLayerOrder_ClearRoom(&terrain);assert(complete);
}
