/* The decorative motifs in 06/06 carry their own opaque black hall matte.
 * Inspect native scene pixels rather than reading the production contracts;
 * no ROM artwork is embedded in this regression. */
static void TrunkDetailBackingFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  static DioramaRoomOverride terrain;
  ActionTrunkAssets *base=malloc(sizeof(*base)),*edited=malloc(sizeof(*edited));
  assert(base&&edited);
  /* Reviewed source locations are fixture witnesses, not classification IDs. */
  const unsigned cells[][2]={{4,52},{5,52},{4,53},{5,53},
      {48,31},{49,31},{50,31},{48,32},{49,32},{50,32}};
  const unsigned specimens[][2]={{4,52},{48,31},{50,32}};
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0606.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&snapshot));
    const ActionRoomScene *s=&snapshot.scene;
    assert(ActionTrunk_Build(base,s,NULL));
    unsigned black=0,colored=0;
    for(unsigned c=0;c<sizeof(cells)/sizeof(*cells);++c)
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned wx=cells[c][0]*16+x,wy=cells[c][1]*16+y;
        uint16_t word;uint8_t id;unsigned ink;
        assert(TrunkTestSample(s,NULL,wx,wy,&word,&ink,&id));
        assert(ink&&((word>>10)&7)==5&&!(word&0x2000));
        assert(!TrunkTestCollision(s,NULL,cells[c][0],cells[c][1]));
        assert(ActionTrunk_SideSolid(base,wx,wy,kActionTrunkSideRear));
        assert(!ActionTrunk_SideSolid(base,wx,wy,kActionTrunkSideSolid));
        assert(!ActionTrunk_SideSolid(base,wx,wy,kActionTrunkSideSnow));
        assert(!ActionTrunk_SideSolid(base,wx,wy,kActionTrunkSideSnowFace));
        const uint32_t color=TrunkTestColor(s,word,ink,15);
        black+=(color&0xffffff)==0;colored+=(color&0xffffff)!=0;
        uint32_t input_pixel=color,output_pixels[3]={0};
        const uint32_t *input[3]={&input_pixel,NULL,NULL};
        const size_t pitches[3]={1,1,1};
        uint32_t *output[3]={output_pixels,output_pixels+1,output_pixels+2};
        ActionTrunk_Split(base,wx,wy,1,1,1,input,pitches,output,15);
        assert(output_pixels[0]==0&&output_pixels[1]==0&&output_pixels[2]==color);
      }
    assert(black&&colored&&black+colored==2560);
    /* Preserve the genuine opening beside the lower-left decoration, including
     * its native blue structural frame. Black recess pixels stay on the rear
     * plane, transparent pixels stay absent, and neither becomes a cap. */
    unsigned frame=0,recess=0,clear=0;
    for(unsigned y=864;y<896;++y)for(unsigned x=128;x<160;++x) {
      uint16_t word;uint8_t id;unsigned ink;
      assert(TrunkTestSample(s,NULL,x,y,&word,&ink,&id));
      assert(((word>>10)&7)==4&&!(word&0x2000));
      assert(ink==0||ink==1||(ink>=9&&ink<=13));
      assert(ActionTrunk_SideSolid(base,x,y,kActionTrunkSideSolid)==(ink>=9));
      assert(ActionTrunk_SideSolid(base,x,y,kActionTrunkSideRear)==(ink==1));
      assert(!ActionTrunk_SideSolid(base,x,y,kActionTrunkSidePriority));
      assert(!ActionTrunk_SideSolid(base,x,y,kActionTrunkSideSnow));
      assert(!ActionTrunk_SideSolid(base,x,y,kActionTrunkSideSnowFace));
      frame+=ink>=9;recess+=ink==1;clear+=ink==0;
    }
    assert(frame==410&&recess==408&&clear==206);
    for(unsigned y=896;y<912;++y)for(unsigned x=144;x<160;++x)
      for(unsigned kind=0;kind<kActionTrunkSideMaskCount;++kind)
        assert(!ActionTrunk_SideSolid(base,x,y,kind));
    for(unsigned c=0;c<sizeof(specimens)/sizeof(*specimens);++c) {
      DioramaTileStamp stamp=TrunkTestStamp(s,specimens[c][0],specimens[c][1],2,2);
      const DioramaTileStamp original=stamp;
      memset(&terrain,0,sizeof(terrain));
      terrain.stamp_layers[0].cells=&stamp;terrain.stamp_layers[0].count=1;
      for(unsigned flip=0;flip<4;++flip) {
        stamp=original;TrunkTestReflect(&stamp,flip);
        assert(ActionTrunk_Build(edited,s,&terrain));
        for(unsigned y=32;y<48;++y)for(unsigned x=32;x<48;++x) {
          assert(ActionTrunk_SideSolid(edited,x,y,kActionTrunkSideRear));
          assert(!ActionTrunk_SideSolid(edited,x,y,kActionTrunkSideSolid));
        }
      }
      /* Explicit high-priority authoring keeps its actor slot. Erased source
       * pixels stay erased; no rectangle filler may cover a genuine opening. */
      stamp=original;for(unsigned q=0;q<4;++q)stamp.words[q]|=0x2000;
      assert(ActionTrunk_Build(edited,s,&terrain));
      for(unsigned y=32;y<48;++y)for(unsigned x=32;x<48;++x) {
        assert(ActionTrunk_SideSolid(edited,x,y,kActionTrunkSidePriority));
        assert(!ActionTrunk_SideSolid(edited,x,y,kActionTrunkSideRear));
        assert(!ActionTrunk_SideSolid(edited,x,y,kActionTrunkSideSolid));
      }
      stamp=original;terrain.pixel_layers[0].count=1;
      terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=2,.y=2};
      terrain.pixel_layers[0].edits[0].transparent[8]=0xffff;
      assert(ActionTrunk_Build(edited,s,&terrain));
      for(unsigned y=32;y<48;++y)for(unsigned x=32;x<48;++x) {
        assert(ActionTrunk_SideSolid(edited,x,y,kActionTrunkSideRear)==(y!=40));
        assert(!ActionTrunk_SideSolid(edited,x,y,kActionTrunkSideSolid));
      }
    }
    printf("trunk detail backing region%u: %u native black matte and %u colored pixels moved together; gap, flips, priority and erasure retained\n",region,black,colored);
  }
  free(base);free(edited);
}
