/* Native blue-grey ridge artwork only. Include after TombCapWords/Key and
 * ForestBackingHash. The baseline records all geometry/front/backing, so a
 * thicker rock or a moved ground fold cannot satisfy the surface oracle. */
static unsigned MarshRockRole(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,uint16_t *word,unsigned *ink,unsigned *faces) {
  static const uint64_t keys[]={UINT64_C(0x799f202b35bdadb1),UINT64_C(0x82b18a1b52907c2d),
      UINT64_C(0xfff6fe6c7e296585),UINT64_C(0x84cb69b2fefc8a06)};
  const int cx=(int)floorf(x/16.f),cy=(int)floorf(y/16.f);
  uint16_t words[4];unsigned collision;*faces=0;
  if(!TombCapWords(s,terrain,cx,cy,words,&collision))return 0;
  const unsigned px=x-cx*16,py=y-cy*16;
  *word=words[(py/8)*2+px/8];*ink=RockFixtureWordPixel(s,*word,px&7,py&7);
  for(unsigned flip=0;flip<4;++flip)for(unsigned i=0;i<4;++i) {
    if(TombCapKey(s,words,collision,flip)!=keys[i])continue;
    assert(!collision);
    /* Both lateral directions are reviewed. Vertical reflection swaps only
     * the upper receiver; continuation records never acquire a receiver. */
    if(i<2)*faces=(1u<<((flip&2)?2:0))|2u|8u;
    return i+1;
  }
  return 0;
}
static bool MarshRockPixelEdited(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y) {
  if(!terrain)return false;
  const int cx=(int)floorf(x/16.f),cy=(int)floorf(y/16.f);
  const DioramaTileStamp *stamp=DioramaLayerOrder_StampAt(&terrain->stamp_layers[0],cx,cy);
  uint8_t id;uint16_t word;
  if(stamp)id=stamp->metatile;
  else if(!ActionRoomScene_LookupTile(s,1,cx*2,cy*2,&word,&id))return false;
  const DioramaPixelEdit *edit=DioramaLayerOrder_PixelEditAt(terrain,0,cx,cy,id);
  return edit&&((edit->black[y-cy*16]|edit->transparent[y-cy*16])&(1u<<(15-(x-cx*16))));
}
static bool MarshRockExpected(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,const VoxelEdge *e,unsigned along,uint16_t colors[16],
    unsigned *count) {
  static const int dx[]={0,-1,0,1},dy[]={1,0,-1,0};
  const int x=e->x+((e->side&1)?0:(int)along),y=e->y+((e->side&1)?(int)along:0);
  uint16_t reference=0;*count=0;
  for(unsigned d=0;d<16;++d) {
    const int sx=x+dx[e->side]*(int)d,sy=y+dy[e->side]*(int)d;
    uint16_t word=0;unsigned ink=0,faces=0;
    if(!MarshRockRole(s,terrain,sx,sy,&word,&ink,&faces))break;
    if(MarshRockPixelEdited(s,terrain,sx,sy)||ActionVoxel_AuthoredSurfaceAt(g,sx,sy))return false;
    if(!SurfaceFixtureStaticWord(s,word))return false;
    if(!ink)break;
    if(!d)reference=word;
    if((word^reference)&0x3c00)break;
    const unsigned at=(((word>>10)&7)*16+ink)*2;
    colors[(*count)++]=0x8000|s->palette[at]|s->palette[at+1]<<8;
  }
  return *count!=0;
}
static unsigned MarshRockCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g) {
  unsigned owned=0,varied=0;
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=g->edges+i;uint16_t word=0;unsigned ink=0,faces=0;
    MarshRockRole(s,terrain,e->x,e->y,&word,&ink,&faces);
    if(!(faces&(1u<<e->side))||e->far_rank!=1) {
      assert(e->surface_texture!=kVoxelSurfaceTextureNativeInward);continue;
    }
    /* Existing explicit authored donors are a separate authoritative path. */
    if(e->top_sample_size&&!e->surface_derived)continue;
    bool safe=true;uint16_t native[16][16];unsigned lengths[16]={0};
    assert(e->length<=16);
    for(unsigned p=0;p<(unsigned)e->length;++p)
      safe&=MarshRockExpected(s,terrain,g,e,p,native[p],lengths+p);
    if(!safe){assert(e->surface_texture!=kVoxelSurfaceTextureNativeInward);continue;}
    assert(!e->colliding&&!e->high_priority&&((word>>10)&7)==5);
    assert(e->surface_texture==kVoxelSurfaceTextureNativeInward&&e->surface_derived);
    assert(e->top_sample_size==16&&e->top_owned_art&&!e->surface_top_material&&!e->contour_skin);
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    for(unsigned p=0;p<(unsigned)e->length;++p) {
      const unsigned phase=((e->side&1)?e->y+p:e->x+p)&15;
      bool differs=false;
      for(unsigned d=0;d<16;++d) {
        const unsigned across=(e->side==1||e->side==2)?15-d:d;
        const unsigned at=(e->side&1)?phase*16+across:across*16+phase;
        const uint16_t expected=native[p][d<lengths[p]?d:lengths[p]-1];
        assert(art[at]==expected);differs|=expected!=native[p][0];
      }
      /* The original dark outline is the first texel, while genuine inward
       * highlights/fissures prevent a repeated-outline-only implementation. */
      varied+=differs;++owned;
    }
  }
  assert(owned&&varied);return owned;
}
static uint64_t MarshRockBackingHash(const ActionVoxelGeometry *g) {
  uint64_t sum=0;
  for(unsigned i=0;i<g->rear_fill_count;++i) {
    const VoxelRearFill *f=g->rear_fills+i;
    const int fields[]={f->x,f->y,f->preserve_existing,f->replace_front};
    sum+=ForestBackingHash(fields,sizeof(fields))+ForestBackingHash(f->colors,sizeof(f->colors));
  }
  for(unsigned i=0;i<g->edge_count;++i) {
    const VoxelEdge *e=g->edges+i;
    for(int p=0;p<e->length;++p) {
      const int values[]={e->x+((e->side&1)?0:p),e->y+((e->side&1)?p:0),e->side,
          e->rear_fold_top,e->rear_fold_bottom,e->flat_rear_join};
      sum+=ForestBackingHash(values,sizeof(values));
    }
  }
  return sum;
}
static void MarshRockSurfaceFixtures(const char *directory) {
  if(!directory||!*directory)return;
  const char *write_path=getenv("AR_MARSH_ROCK_WRITE_BASELINE"),
      *read_path=getenv("AR_MARSH_ROCK_BASELINE");assert(write_path||read_path);
  FILE *baseline=fopen(write_path?write_path:read_path,write_path?"w":"r");assert(baseline);
  static ActionSceneSnapshot fixture;static ActionRoomScene original,remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0201.arscene",directory,region);
    FILE *f=fopen(path,"rb");assert(f);const size_t n=fread(bytes,1,sizeof(bytes),f);
    assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,n,&fixture));
    ActionRoomScene *s=&fixture.scene;original=*s;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,1);
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    for(unsigned authored=0;authored<2;++authored) {
      TombCapTerrain(2,1,region,&terrain);
      const DioramaRoomOverride *t=authored?&terrain:NULL;
      assert(ActionVoxel_Build(g,s,t,settings));
      const uint64_t front=ForestBackingHash(g->front,(size_t)g->width*g->height),
          geometry=TombCapGeometryHash(g),backing=MarshRockBackingHash(g),
          patches=ForestBackingHash(g->patches,g->patch_count*sizeof(*g->patches));
      if(write_path)fprintf(baseline,"%u %u %016llx %016llx %016llx %016llx\n",region,authored,
          (unsigned long long)front,(unsigned long long)geometry,
          (unsigned long long)backing,(unsigned long long)patches);
      else {
        unsigned rr,aa;unsigned long long ff,gg,bb,pp;
        assert(fscanf(baseline,"%u %u %llx %llx %llx %llx",&rr,&aa,&ff,&gg,&bb,&pp)==6);
        assert(rr==region&&aa==authored&&ff==front&&gg==geometry&&bb==backing&&pp==patches);
        const unsigned count=MarshRockCheck(s,t,g);if(!authored)assert(count==556);
      }
      DioramaLayerOrder_ClearRoom(&terrain);
    }
    if(!write_path) {
      assert(ActionVoxel_Build(g,s,NULL,settings));
      remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(other,&remapped,NULL,settings));
      assert(MarshRockCheck(&remapped,NULL,other)==556);
      assert(TombCapGeometryHash(g)==TombCapGeometryHash(other));
      assert(!memcmp(g->front,other->front,(size_t)g->width*g->height));
      for(unsigned erase=0;erase<2;++erase) {
        terrain.pixel_layers[0].count=1;
        DioramaPixelEdit *edit=terrain.pixel_layers[0].edits;
        *edit=(DioramaPixelEdit){.by_cell=true,.x=3,.y=22};
        if(erase)edit->transparent[0]=0x8000;else edit->black[0]=0x8000;
        assert(ActionVoxel_Build(other,s,&terrain,settings));MarshRockCheck(s,&terrain,other);
        DioramaLayerOrder_ClearRoom(&terrain);
      }
      for(unsigned side=0;side<2;++side) {
        terrain.voxel.cell_count=1;DioramaVoxelCell *edit=terrain.voxel.cells;
        *edit=(DioramaVoxelCell){.x=3,.y=22,
          .fields=side?kDioramaVoxelCell_SideArt:kDioramaVoxelCell_TopArt};
        DioramaVoxelArt *art=side?&edit->side_art:&edit->top_art;
        art->set=true;art->bg=1;art->size=16;unsigned collision;
        assert(TombCapWords(s,NULL,3,22,art->words,&collision));
        assert(ActionVoxel_Build(other,s,&terrain,settings));
        const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(other,3,22);
        assert(a&&a->surface&&!a->surface_derived[side?2:1]);
        DioramaLayerOrder_ClearRoom(&terrain);
      }
      terrain.voxel.cell_count=1;terrain.voxel.cells[0]=(DioramaVoxelCell){.x=3,.y=22};
      assert(ActionVoxel_Build(other,s,&terrain,settings));assert(MarshRockCheck(s,&terrain,other)==556);
      assert(TombCapGeometryHash(g)==TombCapGeometryHash(other));DioramaLayerOrder_ClearRoom(&terrain);
    }
    assert(!memcmp(s,&original,sizeof(original)));
    ActionVoxel_Release(g);ActionVoxel_Release(other);
    printf("Marsh rock region%u: %s\n",region,write_path?"baseline recorded":"native contour art/front/depth/props/backing/editor/remap PASS");
  }
  assert(!ferror(baseline));fclose(baseline);
}
