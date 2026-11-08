/* Reviewed native tomb artwork, including the Death Heim copy. Numeric IDs
 * are never policy: fixture matching uses displayed pixels/palette/collision. */
static uint64_t TombCapKey(const ActionRoomScene *s,const uint16_t w[4],unsigned collision,unsigned flip) {
  uint64_t h=UINT64_C(14695981039346656037);
  for(unsigned q=0;q<4;++q) {
    const unsigned source=q^flip,header[]={w[source]>>10&7,collision>>source&1,!!(w[source]&0x2000)};
    for(unsigned i=0;i<3;++i)h=(h^header[i])*UINT64_C(1099511628211);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
      h=(h^RockFixtureWordPixel(s,w[source],flip&1?7-x:x,flip&2?7-y:y))*UINT64_C(1099511628211);
  }
  return h;
}
static bool TombCapWords(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int cx,int cy,uint16_t words[4],unsigned *collision) {
  uint8_t id=0;const DioramaTileStamp *stamp=terrain?
      DioramaLayerOrder_StampAt(&terrain->stamp_layers[0],cx,cy):NULL;
  if(stamp) {if(stamp->blank)return false;id=stamp->metatile;memcpy(words,stamp->words,8);}
  else for(unsigned q=0;q<4;++q)
    if(cx<0||cy<0||!ActionRoomScene_LookupTile(s,1,cx*2+(q&1),cy*2+(q>>1),&words[q],&id))return false;
  *collision=0;for(unsigned q=0;q<4;++q)*collision|=!!(s->bg[0].metatiles[id*8+q*2]&2)<<q;
  return true;
}
/* Only these three reviewed carved ornament families revert to their own native
 *8px top crops. Plain donors and diagonal contour skins keep their contracts. */
static const uint64_t kTombOrnamentKeys[]={UINT64_C(0x16eb4a73a6a264ee),
    UINT64_C(0xf96e8065dd7d129a),UINT64_C(0x1a99b94b4b4a192d)};
static bool TombOrnamentKey(uint64_t key) {
  for(unsigned i=0;i<3;++i)if(key==kTombOrnamentKeys[i])return true;
  return false;
}
static unsigned TombCapRole(const ActionRoomScene *s,const uint16_t words[4],unsigned collision) {
  static const uint64_t targets[]={UINT64_C(0xa5d6fa6ee48d8a14),UINT64_C(0xf0bf245766861e4e),
      UINT64_C(0xa2784924f1ee3977),UINT64_C(0x90ea2b42340c6007),
      UINT64_C(0x529b74c0925378be),UINT64_C(0x380c2c4558ffce16)};
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=TombCapKey(s,words,collision,flip);
    if(TombOrnamentKey(key))return 4;
    if(key==UINT64_C(0x812b2717bca9a4be)||key==UINT64_C(0xad3e241d2d498981))return 2;
    if(key==UINT64_C(0x27c4423e46f41aed)||key==UINT64_C(0x61bd101ef124a375))return 3;
    for(unsigned i=0;i<sizeof(targets)/sizeof(*targets);++i)if(key==targets[i])return 1;
  }
  return 0;
}
static void TombCapTerrain(unsigned group,unsigned room,unsigned region,DioramaRoomOverride *out) {
  static DioramaRoomOverride manifest;DioramaLayerOrder_ClearRoom(&manifest);
  char heading[64],line[4096];snprintf(heading,sizeof(heading),"[layers:%02u:%02u]",group,room);
  FILE *file=fopen("diorama-layers.ini","r");assert(file);bool active=false;
  while(fgets(line,sizeof(line),file)) {
    line[strcspn(line,"\r\n")]=0;char *body=line;while(*body==' '||*body=='\t')++body;
    size_t n=strlen(body);while(n&&(body[n-1]==' '||body[n-1]=='\t'))body[--n]=0;
    if(*body=='['){active=!strcmp(body,heading);continue;}
    if(!active||!*body||*body=='#'||*body==';')continue;
    const char *error=NULL;assert(DioramaLayerOrder_ParseLine(&manifest,body,&error));
  }
  assert(!ferror(file));fclose(file);DioramaLayerOrder_ClearRoom(out);
  assert(DioramaLayerOrder_ForTerrain(&manifest,region,out));
}
/* Image partitioning may change from8 to16px. Hash expanded structural
 * boundaries and their actual depth spans, never atlas handles or UVs. */
static uint64_t TombCapGeometryHash(const ActionVoxelGeometry *g) {
  uint64_t sum=0;unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i)for(unsigned p=0;p<(unsigned)edges[i].length;++p) {
    VoxelEdge e=edges[i];e.x+=(e.side&1)?0:p;e.y+=(e.side&1)?p:0;e.length=1;
    VoxelEdgeSpan span={0};const bool visible=ActionVoxel_ResolveEdgeSpan(g,&e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span);
    unsigned f,b;memcpy(&f,&span.near_depth,4);memcpy(&b,&span.far_depth,4);
    const uint32_t values[]={(uint32_t)e.x,(uint32_t)e.y,e.side,e.near_rank,e.far_rank,
        e.high_priority,e.colliding,e.rear_trim,e.trunk_curve,e.column_curve,visible,f,b};
    sum+=ForestBackingHash(values,sizeof(values));
  }
  return sum;
}
/* Compare the complete published owned tile to its actual source crop,
 * including repeats. A palette-compatible foreign stone donor cannot pass. */
static void TombOrnamentCrop(const ActionRoomScene *s,const uint16_t words[4],
    int cx,int cy,int sample_x,int sample_y,const uint16_t *art) {
  assert(art&&sample_x>=cx*16&&sample_x+8<=cx*16+16);
  assert(sample_y>=cy*16&&sample_y+8<=cy*16+16);
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned nx=sample_x-cx*16+x%8,ny=sample_y-cy*16+y%8;
    const unsigned w=words[(ny/8)*2+nx/8],ink=RockFixtureWordPixel(s,w,nx&7,ny&7);
    const unsigned at=(((w>>10)&7)*16+ink)*2;
    assert(ink&&art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
  }
}
static unsigned TombCapCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,unsigned *covered,int selected[2]) {
  unsigned count,missing=0;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);*covered=0;
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];if(e->side||e->far_rank!=2)continue;
    const int cx=(int)floorf(e->x/16.f),cy=(int)floorf(e->y/16.f);
    uint16_t words[4];unsigned collision;
    if(!TombCapWords(s,terrain,cx,cy,words,&collision))continue;
    const unsigned role=TombCapRole(s,words,collision),q=((e->y-cy*16)/8)*2+(e->x-cx*16)/8;
    if(role==3){assert(((words[q]>>10)&7)==4);assert(e->top_sample_size!=16);continue;}
    if(!role||((words[q]>>10)&7)!=5)continue;
    *covered+=e->length;
    if(role==4) {
      assert(e->surface_texture&&e->top_owned_art&&e->top_sample_size==8);
      assert(!e->contour_skin&&!e->surface_top_material);
      /* Opaque source quadrants of this exact native family are complete;
       * the first-choice crop must remain that quadrant, not its neighbour. */
      assert(e->top_sample_x==cx*16+(q&1)*8&&e->top_sample_y==cy*16+(q>>1)*8);
      assert(!!e->high_priority==!!(words[q]&0x2000));
      assert(!!e->colliding==!!(collision&(1u<<q)));
      TombOrnamentCrop(s,words,cx,cy,e->top_sample_x,e->top_sample_y,
          ActionVoxel_AuthoredArt(g,e->top_owned_art-1));
      continue;
    }
    if(!e->surface_texture||!e->top_owned_art||e->top_sample_size!=16){missing+=e->length;continue;}
    assert(!e->high_priority && !(words[q]&0x2000));
    const int dx=e->top_sample_x/16,dy=e->top_sample_y/16;
    uint16_t donor[4];unsigned donor_collision;
    assert(TombCapWords(s,terrain,dx,dy,donor,&donor_collision));
    assert(TombCapRole(s,donor,donor_collision)==2);
    assert(abs(dx-cx)+abs(dy-cy)<=8);
    const uint16_t *art=ActionVoxel_AuthoredArt(g,e->top_owned_art-1);assert(art);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned w=donor[(y/8)*2+x/8],ink=RockFixtureWordPixel(s,w,x&7,y&7);
      assert(ink && !((words[q]^w)&0x3c00));
      const unsigned at=(((w>>10)&7)*16+ink)*2;
      assert(art[y*16+x]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
    }
    bool whole_stone=true;for(unsigned quadrant=0;quadrant<4;++quadrant)
      whole_stone&=((words[quadrant]>>10)&7)==5;
    if(role==1&&whole_stone&&selected[0]==INT_MIN){selected[0]=cx;selected[1]=cy;}
  }
  return missing;
}
static void TombCapEdits(const ActionRoomScene *s,int x,int y) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  terrain.voxel.cell_count=1;DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Width,.width=12};
  assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a && a->surface_sample_size[1]==16);
  const int donor_x=a->surface_sample_x[1],donor_y=a->surface_sample_y[1];
  uint16_t expected[256];memcpy(expected,a->pixels[1],sizeof(expected));
  edit->fields|=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back;edit->front=.03f;edit->back=.17f;
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a && a->surface_sample_size[1]==16 && !memcmp(a->pixels[1],expected,sizeof(expected)));
  edit->fields|=kDioramaVoxelCell_TopArt;edit->top_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
  unsigned collision;assert(TombCapWords(s,NULL,x,y,edit->top_art.words,&collision));
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a && !a->surface_sample_size[1] && memcmp(a->pixels[1],expected,sizeof(expected)));
  edit->fields=kDioramaVoxelCell_Faces;edit->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Top;
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);assert(a&&!(a->faces&kDioramaVoxelFaceMask_Top));
  *edit=(DioramaVoxelCell){.x=x,.y=y};
  assert(ActionVoxel_Build(g,s,&terrain,settings));unsigned covered;int unused[2]={INT_MIN,0};
  assert(!TombCapCheck(s,&terrain,g,&covered,unused));
  /* An explicitly erased donor cannot be advertised as an opaque native
   * square; another actual donor may win, or the original contour remains. */
  terrain.pixel_layers[0].count=1;
  terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=donor_x/16,.y=donor_y/16};
  terrain.pixel_layers[0].edits[0].transparent[0]=0x8000;
  assert(ActionVoxel_Build(g,s,&terrain,settings));unsigned count;const VoxelEdge *e=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i)assert(!(e[i].surface_texture&&e[i].top_sample_size==16&&
      e[i].top_sample_x==donor_x&&e[i].top_sample_y==donor_y));
  ActionVoxel_Release(g);
}
static bool TombOrnamentFind(const ActionRoomScene *s,uint64_t key,
    int *x,int *y,uint8_t *id,uint16_t words[4]) {
  for(int cy=0;cy<(int)ActionRoomScene_TileHeight(s,1)/2;++cy)
    for(int cx=0;cx<(int)ActionRoomScene_TileWidth(s,1)/2;++cx) {
      unsigned collision;
      if(TombCapWords(s,NULL,cx,cy,words,&collision)&&TombCapKey(s,words,collision,0)==key) {
        assert(ActionRoomScene_LookupTile(s,1,cx*2,cy*2,&words[0],id));*x=cx;*y=cy;return true;
      }
    }
  return false;
}
static void TombOrnamentEdits(const ActionRoomScene *s,int x,int y) {
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(s->group,s->map);
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,x,y,words,&collision));
  assert(TombCapRole(s,words,collision)==4);
  terrain.voxel.cell_count=1;DioramaVoxelCell *edit=&terrain.voxel.cells[0];
  *edit=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Width,.width=12};
  assert(ActionVoxel_Build(g,s,&terrain,settings));const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a&&a->surface_sample_size[1]==8&&a->surface_sample_x[1]==x*16&&a->surface_sample_y[1]==y*16);
  TombOrnamentCrop(s,words,x,y,a->surface_sample_x[1],a->surface_sample_y[1],a->pixels[1]);
  uint16_t expected[256],front[256];memcpy(expected,a->pixels[1],sizeof(expected));memcpy(front,a->pixels[0],sizeof(front));
  edit->fields|=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back;edit->front=.03f;edit->back=.17f;
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a&&a->surface_sample_size[1]==8&&!memcmp(a->pixels[1],expected,sizeof(expected)));
  /* Painting is still explicit: choosing a real plain donor is honoured,
   * while the automatic front remains the source ornament. */
  int tx,ty;assert(FixtureCell(s,0x0a,&tx,&ty));
  edit->fields|=kDioramaVoxelCell_TopArt;edit->top_art=(DioramaVoxelArt){.set=true,.bg=1,.size=16};
  assert(TombCapWords(s,NULL,tx/2,ty/2,edit->top_art.words,&collision));
  assert(TombCapRole(s,edit->top_art.words,collision)==2);
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a&&!a->surface_sample_size[1]&&memcmp(a->pixels[1],expected,sizeof(expected)));
  assert(!memcmp(a->pixels[0],front,sizeof(front)));
  for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
    const unsigned w=edit->top_art.words[(py/8)*2+px/8],ink=RockFixtureWordPixel(s,w,px&7,py&7);
    const unsigned at=(((w>>10)&7)*16+ink)*2;
    assert(ink&&a->pixels[1][py*16+px]==(uint16_t)(0x8000|s->palette[at]|s->palette[at+1]<<8));
  }
  edit->fields=kDioramaVoxelCell_Faces;edit->faces=kDioramaVoxelFaceMask_All&~kDioramaVoxelFaceMask_Top;
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a&&!(a->faces&kDioramaVoxelFaceMask_Top));
  *edit=(DioramaVoxelCell){.x=x,.y=y,.fields=kDioramaVoxelCell_Width,.width=12};
  assert(ActionVoxel_Build(g,s,&terrain,settings));a=ActionVoxel_AuthoredCellAt(g,x,y);
  assert(a&&a->surface_sample_size[1]==8&&!memcmp(a->pixels[1],expected,sizeof(expected)));
  *edit=(DioramaVoxelCell){.x=x,.y=y};
  assert(ActionVoxel_Build(g,s,&terrain,settings));unsigned covered;int unused[2]={INT_MIN,0};
  assert(!TombCapCheck(s,&terrain,g,&covered,unused));ActionVoxel_Release(g);
}
static void TombCapReflectedStamps(const ActionRoomScene *s) {
  static DioramaRoomOverride terrain;static DioramaTileStamp cells[2];
  static ActionRoomScene reflected;
  int tx,ty;assert(FixtureCell(s,0x0a,&tx,&ty));uint8_t donor_id;
  uint16_t donor[4];unsigned collision;
  assert(TombCapWords(s,NULL,tx/2,ty/2,donor,&collision));
  assert(ActionRoomScene_LookupTile(s,1,tx,ty,&donor[0],&donor_id));
  ActionVoxelGeometry *g=ActionVoxel_Create();assert(g);
  unsigned families=0;
  for(unsigned family=0;family<3;++family) {
    uint16_t words[4];uint8_t id;int x,y;
    if(!TombOrnamentFind(s,kTombOrnamentKeys[family],&x,&y,&id,words))continue;
    ++families;assert(TombCapWords(s,NULL,x,y,words,&collision));
    for(unsigned flip=0;flip<4;++flip) {
      reflected=*s;
      for(unsigned q=0;q<4;++q)reflected.bg[0].metatiles[id*8+q*2]=
          (s->bg[0].metatiles[id*8+q*2]&~2u)|((collision>>(q^flip)&1u)<<1);
      memset(&terrain,0,sizeof(terrain));
      terrain.stamp_layers[0]=(DioramaStampLayerOverride){.set_bounds=true,
          .x0=-3,.y0=-3,.x1=-1,.y1=-2,.count=2,.capacity=2,.cells=cells};
      cells[0]=(DioramaTileStamp){.x=-3,.y=-3,.metatile=id};
      cells[1]=(DioramaTileStamp){.x=-2,.y=-3,.metatile=donor_id};
      for(unsigned q=0;q<4;++q){cells[0].words[q]=words[q^flip]^((flip&1)?0x4000:0)^((flip&2)?0x8000:0);cells[1].words[q]=donor[q];}
      assert(ActionVoxel_Build(g,&reflected,&terrain,ActionVoxel_SettingsForRoom(s->group,s->map)));
      unsigned covered;int selected[2]={INT_MIN,0};assert(!TombCapCheck(&reflected,&terrain,g,&covered,selected));
      assert(covered==32); /*16 own8 ornament pixels plus16 retained plain donor pixels. */
    }
  }
  assert(families==3);ActionVoxel_Release(g);
}
static void TombCapFixtures(const char *directory) {
  if(!directory)return;bool complete=true;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,remapped;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  const unsigned rooms[][2]={{3,3},{3,4},{3,5},{3,6},{7,4}};
  for(unsigned region=0;region<3;++region)for(unsigned r=0;r<5;++r) {
    const unsigned group=rooms[r][0],room=rooms[r][1];char path[1024];
    snprintf(path,sizeof(path),"%s/%u/%02u%02u.arscene",directory,region,group,room);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    original=snapshot.scene;const ActionRoomScene *s=&snapshot.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create(),*renamed=ActionVoxel_Create();assert(g&&renamed);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(group,room);
    assert(ActionVoxel_Build(g,s,NULL,settings));unsigned covered;int selected[2]={INT_MIN,0};
    const unsigned missing=TombCapCheck(s,NULL,g,&covered,selected);assert(covered);complete&=!missing;
    if(group==3&&room==5) {
      /* Requested material transition: carved blocks4/5 and8/9 keep own8,
       * while gem-side blocks6/7 and10/11 keep plain stone16 on top. */
      unsigned n;const VoxelEdge *edge=ActionVoxel_Edges(g,&n);
      for(int x=4;x<=11;++x) {
        const bool gem=(x%4)>=2;unsigned pixels=0;
        uint16_t words[4];unsigned collision;assert(TombCapWords(s,NULL,x,92,words,&collision));
        assert(TombCapRole(s,words,collision)==(gem?1u:4u));
        for(unsigned i=0;i<n;++i)if(!edge[i].side&&edge[i].far_rank==2&&edge[i].y==92*16&&edge[i].x/16==x) {
          assert(edge[i].top_sample_size==(gem?16u:8u));
          if(!gem)assert(edge[i].top_sample_x/16==x&&edge[i].top_sample_y/16==92);
          else {uint16_t donor[4];unsigned dc;assert(TombCapWords(s,NULL,edge[i].top_sample_x/16,edge[i].top_sample_y/16,donor,&dc));assert(TombCapRole(s,donor,dc)==2);}
          pixels+=edge[i].length;
        }
        assert(pixels==16);
      }
    }
    printf("tomb caps%02u/%02u terrain%u: native top pixels%u missing16%u mask%016llx geometry%016llx profiles%016llx\n",
        group,room,region,covered,missing,(unsigned long long)ForestBackingHash(g->front,(size_t)g->width*g->height),
        (unsigned long long)TombCapGeometryHash(g),(unsigned long long)ForestBackingHash(g->patches,g->patch_count*sizeof(*g->patches)));
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(renamed,&remapped,NULL,settings));
    SameGeometry(g,renamed);assert(!memcmp(g->front,renamed->front,(size_t)g->width*g->height));
    unsigned renamed_covered;int none[2]={INT_MIN,0};
    assert(TombCapCheck(&remapped,NULL,renamed,&renamed_covered,none)==missing&&renamed_covered==covered);
    TombCapTerrain(group,room,region,&terrain);assert(ActionVoxel_Build(g,s,&terrain,settings));
    none[0]=INT_MIN;const unsigned shipped_missing=TombCapCheck(s,&terrain,g,&covered,none);
    printf("tomb caps%02u/%02u terrain%u: shipped top pixels%u missing16%u\n",group,room,region,covered,shipped_missing);
    complete&=!shipped_missing;
    if(!missing&&!shipped_missing) {
      /* The boss chamber/copy contain no remaining role1 cap. Their donor
       * and reflected coverage stays above; exercise their own8 edit below. */
      if(group==3&&room<=5){assert(selected[0]!=INT_MIN);TombCapEdits(s,selected[0],selected[1]);}
      else assert(selected[0]==INT_MIN);
      uint16_t words[4];uint8_t id;int x,y;
      assert(TombOrnamentFind(s,kTombOrnamentKeys[0],&x,&y,&id,words));
      TombOrnamentEdits(s,x,y);TombCapReflectedStamps(s);
    }
    assert(!memcmp(s,&original,sizeof(original)));ActionVoxel_Release(g);ActionVoxel_Release(renamed);
  }
  DioramaLayerOrder_ClearRoom(&terrain);assert(complete);
}
