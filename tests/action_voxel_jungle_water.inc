/* Added-water color is independent of native front/geometry ownership. */
static const VoxelWaterSurface *JungleWaterAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned count;const VoxelWaterSurface *p=ActionVoxel_WaterSurfaces(g,&count);
  for(unsigned i=0;i<count;++i)if(p[i].y==y && x>=p[i].x && x<p[i].x+p[i].width)return p+i;
  return NULL;
}
static void JungleWaterNativeUnchanged(const ActionVoxelGeometry *a,const ActionVoxelGeometry *b) {
  SameGeometry(a,b);assert(a->width==b->width&&a->height==b->height);
  assert(!memcmp(a->front,b->front,(size_t)a->width*a->height));
  uint32_t native[256],ar[256],af[256],br[256],bf[256];
  for(unsigned i=0;i<256;++i)native[i]=0xff000000u|(i*65993u&0xffffff);
  const ActionRoomSceneFrameState frame={.brightness=15};
  unsigned count;const VoxelWaterSurface *p=ActionVoxel_WaterSurfaces(a,&count);
  for(unsigned i=0;i<count;++i)for(int x=p[i].x;x<p[i].x+p[i].width;x+=16) {
    assert(ActionVoxel_Split(a,x,p[i].y,16,16,16,&frame,native,16,ar,af));
    assert(ActionVoxel_Split(b,x,p[i].y,16,16,16,&frame,native,16,br,bf));
    assert(!memcmp(ar,br,sizeof(ar))&&!memcmp(af,bf,sizeof(af)));
  }
}
static void JungleWaterColorFixtures(const char *directory) {
  if(!directory)return;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];static ActionSceneSnapshot snapshot;
  static ActionRoomScene scene,changed;static DioramaRoomOverride terrain;
  for(unsigned region=0;region<3;++region)for(unsigned room=1;room<=3;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/050%u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);size_t size=fread(bytes,1,sizeof(bytes),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    for(unsigned rename=0;rename<2;++rename) {
      scene=snapshot.scene;if(rename)Renumber(&scene);
      ActionVoxelGeometry *g=ActionVoxel_Create(),*fallback=ActionVoxel_Create();assert(g&&fallback);
      const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(5,room);
      assert(ActionVoxel_Build(g,&scene,NULL,settings));
      changed=scene;changed.bg[1].have_map=false;
      assert(ActionVoxel_Build(fallback,&changed,NULL,settings));JungleWaterNativeUnchanged(g,fallback);
      unsigned count,total=0;const VoxelWaterSurface *p=ActionVoxel_WaterSurfaces(g,&count);assert(count);
      const int wx=p[0].x,wy=p[0].y,cx=wx/16,cy=wy/16;
      for(unsigned i=0;i<count;++i) {
        assert(p[i].uniform_color[0]==0x9d28&&p[i].uniform_color[1]==0xb190);
        assert(p[i].sample_y==13&&p[i].sample_height==3&&p[i].opaque&&p[i].high_priority);
        total+=p[i].width;const VoxelWaterSurface *old=JungleWaterAt(fallback,p[i].x,p[i].y);
        assert(old&&!old->uniform_color[0]&&!old->uniform_color[1]);
      }
      assert(total==(room==1?80u:room==2?96u:32u)*16);
      ActionRoomSceneFrameState frame={.brightness=15};
      assert(ActionVoxel_WaterSurfaceColor(p,0,&frame)==0xff424a39);
      assert(ActionVoxel_WaterSurfaceColor(p,1,&frame)==0xff846363);
      frame.brightness=0;assert(ActionVoxel_WaterSurfaceColor(p,0,&frame)==0xff000000);
      assert(ActionVoxel_WaterSurfaceColor(p,1,&frame)==0xff000000);
      frame.brightness=15;frame.fixed_color=0x7fff;frame.cgadsub=0x81;
      assert(ActionVoxel_WaterSurfaceColor(p,0,&frame)==0xff000000);
      assert(ActionVoxel_WaterSurfaceColor(p,1,&frame)==0xff846363);
      frame.cgadsub=0x82;assert(ActionVoxel_WaterSurfaceColor(p,0,&frame)==0xff424a39);
      assert(ActionVoxel_WaterSurfaceColor(p,1,&frame)==0xff000000);
      /* A native palette change affects the added tone, not the walking or
       * ornament surfaces. SameGeometry deliberately excludes color metadata. */
      changed=scene;changed.palette[41*2]^=1;
      assert(ActionVoxel_Build(fallback,&changed,NULL,settings));JungleWaterNativeUnchanged(g,fallback);
      assert(JungleWaterAt(fallback,wx,wy)->uniform_color[1]==0xb191);
      /* Retain an intact donor elsewhere while the local main-screen source
       * is replaced by native blank art: no opaque added water over that edit. */
      changed=scene;const unsigned mx=(unsigned)cx%(scene.bg[1].pages_wide*16);
      const size_t at=((unsigned)cy/16*scene.bg[1].pages_wide+mx/16)*256+((unsigned)cy&15)*16+(mx&15);
      changed.bg[1].map[at]=(uint8_t)(rename?(0x1c+73)&255:0x1c);
      assert(ActionVoxel_Build(fallback,&changed,NULL,settings));
      assert(JungleWaterAt(fallback,wx,wy)&&!JungleWaterAt(fallback,wx,wy)->uniform_color[0]);
      /* Source-specific pixel ownership: upper native shore masks are safe;
       * a submerged edit must retain captured sampling for that water section. */
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[1].count=1;
      DioramaPixelEdit *e=&terrain.pixel_layers[1].edits[0];e->by_cell=true;e->x=(int16_t)mx;e->y=(int16_t)cy;e->black[0]=0xffff;
      assert(ActionVoxel_Build(fallback,&scene,&terrain,settings));
      assert(JungleWaterAt(fallback,wx,wy)->uniform_color[0]);
      e->black[13]=1;assert(ActionVoxel_Build(fallback,&scene,&terrain,settings));
      assert(!JungleWaterAt(fallback,wx,wy)->uniform_color[0]);
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;e=&terrain.pixel_layers[0].edits[0];
      e->by_cell=true;e->x=(int16_t)cx;e->y=(int16_t)cy;e->transparent[14]=1;
      assert(ActionVoxel_Build(fallback,&scene,&terrain,settings));
      assert(!JungleWaterAt(fallback,wx,wy)->uniform_color[0]);
      /* Donor identity is content, including renumbered CHR. */
      changed=scene;const unsigned donor_id=rename?(0x1d+73)&255:0x1d;
      const unsigned donor_chr=256+changed.bg[1].metatiles[donor_id*8+1];
      changed.characters[donor_chr*32]^=1;
      assert(ActionVoxel_Build(fallback,&changed,NULL,settings));
      assert(!JungleWaterAt(fallback,wx,wy)->uniform_color[0]);
      if(!rename&&region==0)for(unsigned phase=0;phase<4;++phase) {
        changed=scene;assert(ActionRoomScene_BuildCharacters(&scene,0,phase,changed.characters,sizeof(changed.characters)));
        assert(ActionVoxel_Build(fallback,&changed,NULL,settings));JungleWaterNativeUnchanged(g,fallback);
        unsigned n;const VoxelWaterSurface *phase_water=ActionVoxel_WaterSurfaces(fallback,&n);
        assert(n==count);for(unsigned i=0;i<n;++i)assert(phase_water[i].uniform_color[0]&&phase_water[i].uniform_color[1]);
      }
      if(!rename) {
        TombCapTerrain(5,room,region,&terrain);
        assert(ActionVoxel_Build(fallback,&scene,&terrain,settings));
        unsigned shipped_count,shipped_width=0,shipped_colored=0;
        const VoxelWaterSurface *shipped=ActionVoxel_WaterSurfaces(fallback,&shipped_count);
        for(unsigned i=0;i<shipped_count;++i) {
          shipped_width+=shipped[i].width;
          if(shipped[i].uniform_color[0]&&shipped[i].uniform_color[1])shipped_colored+=shipped[i].width;
        }
        fprintf(stderr,"water shipped05/%02u region%u color%u/total%u\n",room,region,shipped_colored,shipped_width);
        assert(shipped_width>=total && shipped_colored==shipped_width);
        changed=scene;changed.bg[1].have_map=false;
        assert(ActionVoxel_Build(g,&changed,&terrain,settings));JungleWaterNativeUnchanged(g,fallback);
        if(room==3) {
          /* The shipped left extension is shorelineAD at(-1,13), paired
           * with BG2wave31. Row12 is the stone course above it. */
          const VoxelWaterSurface *negative=JungleWaterAt(fallback,-16,208);
          assert(negative&&negative->uniform_color[0]);
          /* Edit the effective, terrain-resolved record. Parsing a second
           * unqualified manifest record here would not replace its region. */
          const DioramaTileStamp *old_stamp=DioramaLayerOrder_StampAt(&terrain.stamp_layers[1],-1,13);
          assert(old_stamp);
          DioramaTileStamp *blank=&terrain.stamp_layers[1].cells[old_stamp-terrain.stamp_layers[1].cells];
          blank->blank=true;blank->bands=0;memset(blank->words,0,sizeof(blank->words));
          assert(DioramaLayerOrder_StampAt(&terrain.stamp_layers[1],-1,13)->blank);
          assert(ActionVoxel_Build(fallback,&scene,&terrain,settings));
          assert(!JungleWaterAt(fallback,-16,208)->uniform_color[0]);
          assert(JungleWaterAt(fallback,0,208)->uniform_color[0]);
        }
        /* A local stamped blank/priority change must still opt out, even
         * when the shipped unrelated map extension is accepted. */
        char line[256];snprintf(line,sizeof(line),"bg2-stamp = cell:%u,%d words:blank bands:0,0,0,0 metatile:00",mx,cy);
        DioramaLayerOrder_ClearRoom(&terrain);
        assert(DioramaLayerOrder_ParseLine(&terrain,line,NULL));
        assert(ActionVoxel_Build(fallback,&scene,&terrain,settings));
        assert(!JungleWaterAt(fallback,wx,wy)->uniform_color[0]);
        DioramaLayerOrder_ClearRoom(&terrain);
      }
      ActionVoxel_Release(g);ActionVoxel_Release(fallback);
    }
    printf("Jungle water05/%02u terrain%u: native/renumbered colors, geometry/front parity, fade, edited/unknown-source fallback passed\n",room,region);
  }
}
