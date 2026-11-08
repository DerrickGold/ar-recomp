/* Include after TombCapTerrain/Words and RockFixtureWordPixel. The oracle
 * reads the effective original native source, not the new terminal helper. */
static const VoxelJungleTerminalRun *JungleTerminalTestAt(const VoxelJungleTerminal *s,unsigned edge,int at) {
  if(!s)return NULL;
  for(unsigned i=0;i<s->count;++i)if(s->runs[i].edge==edge&&
      at>=s->runs[i].start&&at<s->runs[i].end)return s->runs+i;
  return NULL;
}
static void JungleTerminalNativePixel(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,const VoxelJungleTerminalRun *run) {
  assert(run);const int cx=(int)floor(x/16.0),cy=(int)floor(y/16.0);
  const unsigned px=(unsigned)(x-cx*16),py=(unsigned)(y-cy*16);
  uint16_t words[4];unsigned collision;
  assert(TombCapWords(s,terrain,cx,cy,words,&collision));
  const unsigned word=words[(py/8)*2+px/8],ink=RockFixtureWordPixel(s,word,px&7,py&7);
  assert(ink);const unsigned index=((word>>10)&7)*16+ink;
  const unsigned color=s->palette[index*2]|s->palette[index*2+1]<<8;
  assert(run->palette_index==index&&run->high_priority==!!(word&0x2000));
  assert(run->color==(uint16_t)(color|0x8000));
}
static void JungleTerminalSourceCheck(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionVoxelGeometry *g,bool shipped) {
  const VoxelJungleTerminal *t=ActionVoxel_JungleTerminal(g);assert(t&&t->count);
  assert(t->left==(shipped?-16:0)&&t->top==(shipped?-96:0)&&t->right==512&&t->bottom==(shipped?304:256));
  /* The measured right boundary has no alpha holes. Every submitted color
   * must come from its exact native terminal column, with native priority. */
  for(int y=t->top;y<t->bottom;++y)
    JungleTerminalNativePixel(s,terrain,511,y,JungleTerminalTestAt(t,1,y));
  for(int x=t->left;x<t->right;++x)
    JungleTerminalNativePixel(s,terrain,x,t->bottom-1,JungleTerminalTestAt(t,2,x));
  for(unsigned i=0;i<t->count;++i) {
    const VoxelJungleTerminalRun *r=t->runs+i;
    assert(r->start<r->end&&r->edge>=1&&r->edge<=3);
    if(r->edge==3)for(int y=r->start;y<r->end;++y) {
      assert(y>=208);JungleTerminalNativePixel(s,terrain,t->left,y,r);
    }
  }
  if(shipped) {
    assert(JungleTerminalTestAt(t,2,0)->color==0xa0e5);
    assert(!JungleTerminalTestAt(t,1,215)->high_priority&&JungleTerminalTestAt(t,1,216)->high_priority);
    for(int y=208;y<304;++y)assert(JungleTerminalTestAt(t,3,y));
    for(int y=0;y<208;++y)assert(!JungleTerminalTestAt(t,3,y));
  }
}
static void JungleTerminalFixtures(const char *directory) {
  if(!directory||!*directory)return;
  static ActionSceneSnapshot snap;static ActionRoomScene source,changed;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0503.arscene",directory,region);
    FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,sizeof(bytes),f);
    assert(!ferror(f));fclose(f);assert(ActionSceneSnapshot_Decode(bytes,n,&snap));source=snap.scene;
    ActionVoxelGeometry *g=ActionVoxel_Create(),*control=ActionVoxel_Create();assert(g&&control);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(5,3);
    for(unsigned shipped=0;shipped<2;++shipped) {
      memset(&terrain,0,sizeof(terrain));if(shipped)TombCapTerrain(5,3,region,&terrain);
      assert(ActionVoxel_Build(g,&source,&terrain,settings));JungleTerminalSourceCheck(&source,&terrain,g,shipped!=0);
      /* Removing the required main source disables the optional contribution
       * and leaves every expanded native edge/front/split pixel unchanged. */
      changed=source;changed.bg[1].have_map=false;
      assert(ActionVoxel_Build(control,&changed,&terrain,settings));assert(!ActionVoxel_JungleTerminal(control));
      JungleWaterNativeUnchanged(g,control);
      for(unsigned phase=0;phase<4;++phase) {
        changed=source;assert(ActionRoomScene_BuildCharacters(&source,0,phase,changed.characters,sizeof(changed.characters)));
        assert(ActionVoxel_Build(control,&changed,&terrain,settings));JungleTerminalSourceCheck(&changed,&terrain,control,shipped!=0);
        SameGeometry(g,control);
      }
      if(!shipped) {
        changed=source;Renumber(&changed);assert(ActionVoxel_Build(control,&changed,&terrain,settings));
        JungleTerminalSourceCheck(&changed,&terrain,control,false);SameGeometry(g,control);
      } else {
        /* Opaque RGB zero is still native source; it is never treated as
         * missing alpha or silently replaced by a default water color. */
        changed=source;changed.palette[81*2]=changed.palette[81*2+1]=0;
        changed.palette[123*2]=0x34;changed.palette[123*2+1]=0x12;
        assert(ActionVoxel_Build(control,&changed,&terrain,settings));SameGeometry(g,control);
        const VoxelJungleTerminal *t=ActionVoxel_JungleTerminal(control);assert(t);
        assert(JungleTerminalTestAt(t,2,0)->color==0x8000);
        assert(JungleTerminalTestAt(t,1,0)->color==0x9234);
        ActionRoomSceneFrameState frame={.brightness=15};
        assert(ActionVoxel_AuthoredColor(JungleTerminalTestAt(t,2,0)->color,&frame)==0xff000000);
        frame.brightness=0;assert(ActionVoxel_AuthoredColor(JungleTerminalTestAt(t,1,0)->color,&frame)==0xff000000);
        /* Explicit local ownership removes only its16px source interval. */
        terrain.pixel_layers[0].count=1;
        DioramaPixelEdit *p=&terrain.pixel_layers[0].edits[0];
        *p=(DioramaPixelEdit){.by_cell=true,.x=31,.y=12};p->black[0]=1;
        for(unsigned mask=0;mask<2;++mask) {
          if(mask){p->black[0]=0;p->transparent[0]=1;}
          assert(ActionVoxel_Build(control,&source,&terrain,settings));t=ActionVoxel_JungleTerminal(control);assert(t);
          assert(!JungleTerminalTestAt(t,1,192)&&!JungleTerminalTestAt(t,1,207));
          assert(JungleTerminalTestAt(t,1,191)&&JungleTerminalTestAt(t,1,208)&&JungleTerminalTestAt(t,2,511));
        }
        terrain.pixel_layers[0].count=0;terrain.voxel.cell_count=1;
        terrain.voxel.cells[0]=(DioramaVoxelCell){.x=31,.y=18,.fields=kDioramaVoxelCell_Backing,
          .backing=kDioramaVoxelBacking_None};
        assert(ActionVoxel_Build(control,&source,&terrain,settings));t=ActionVoxel_JungleTerminal(control);assert(t);
        assert(!JungleTerminalTestAt(t,2,511)&&!JungleTerminalTestAt(t,1,303));
        assert(JungleTerminalTestAt(t,2,495)&&JungleTerminalTestAt(t,1,287));
        terrain.voxel.cells[0].fields=0;
        assert(ActionVoxel_Build(control,&source,&terrain,settings));JungleTerminalSourceCheck(&source,&terrain,control,true);
        /* Same displayed terminal art relocated into native animated CHR
         * must fail closed despite having the identical fingerprint today. */
        changed=source;memcpy(changed.characters+256*32,source.characters+0xbe*32,32);
        assert(DioramaLayerOrder_ParseLine(&terrain,
          "bg1-stamp = cell:31,0 metatile:4F words:1D00,1D00,1D00,1D00 bands:1,1,1,1",NULL));
        uint16_t animated_words[4];unsigned animated_collision;
        assert(TombCapWords(&changed,&terrain,31,0,animated_words,&animated_collision));
        assert(TombCapKey(&changed,animated_words,animated_collision,0)==UINT64_C(0x29a4069aac1574b1));
        assert(ActionVoxel_Build(control,&changed,&terrain,settings));t=ActionVoxel_JungleTerminal(control);assert(t);
        assert(!JungleTerminalTestAt(t,1,0)&&JungleTerminalTestAt(t,1,16));
        TombCapTerrain(5,3,region,&terrain);
        /* A real missing boundary stamp is no-color source, not a matte. */
        const DioramaTileStamp *old=DioramaLayerOrder_StampAt(&terrain.stamp_layers[0],31,18);assert(old);
        DioramaTileStamp *stamp=&terrain.stamp_layers[0].cells[old-terrain.stamp_layers[0].cells];stamp->blank=true;
        assert(ActionVoxel_Build(control,&source,&terrain,settings));t=ActionVoxel_JungleTerminal(control);assert(t);
        assert(!JungleTerminalTestAt(t,2,511)&&JungleTerminalTestAt(t,2,495));
      }
      DioramaLayerOrder_ClearRoom(&terrain);
    }
    changed=source;changed.map=2;assert(ActionVoxel_Build(control,&changed,NULL,settings));assert(!ActionVoxel_JungleTerminal(control));
    assert(!memcmp(&source,&snap.scene,sizeof(source)));
    ActionVoxel_Release(g);ActionVoxel_Release(control);
    printf("Jungle terminal region%u: raw/shipped exact native ink and priority, geometry/front parity,4animation phases, renumber, palette/fade, local edits/reset and missing-source controls\n",region);
  }
}
