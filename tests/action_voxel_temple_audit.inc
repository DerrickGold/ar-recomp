/* Reviewed native temple matte contract. Numeric IDs below locate fixture art
 * only; the production classification continues to use displayed-art keys. */
static void TempleAuditFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene tower_reference;
  static DioramaRoomOverride terrain;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/0103-%u.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));
    const ActionRoomScene *source=&fixture.scene;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,3);
    ActionVoxelGeometry *fixed=ActionVoxel_Create(),*reference=ActionVoxel_Create(),*edited=ActionVoxel_Create();
    assert(fixed&&reference&&edited);
    assert(ActionVoxel_Build(fixed,source,NULL,settings));
    /* Same original room assets under the unchanged Tower material scope give
     * the previous contour masks. Only indoor0103 owns this backing role. */
    tower_reference=*source;tower_reference.map=4;
    assert(ActionVoxel_Build(reference,&tower_reference,NULL,settings));
    unsigned a,b;
    const VoxelEdge *ea=ActionVoxel_Edges(fixed,&a),*eb=ActionVoxel_Edges(reference,&b);
    assert(a==b && !memcmp(ea,eb,a*sizeof(*ea)));
    const VoxelPatch *pa=ActionVoxel_Patches(fixed,&a),*pb=ActionVoxel_Patches(reference,&b);
    assert(a==b && !memcmp(pa,pb,a*sizeof(*pa)));
    unsigned checked=0;
    const unsigned artwork[]={0x33,0x5f}; /* Column shoulder; carved capital corner. */
    for(unsigned art=0;art<sizeof(artwork)/sizeof(*artwork);++art) {
      int tx,ty;assert(FixtureCell(source,artwork[art],&tx,&ty));
      uint16_t matte[16]={0};
      for(unsigned q=0;q<4;++q) {
        uint8_t old[8]={0},now[8]={0};
        ActionVoxel_CutoutRows(reference,tx+(q&1),ty+(q>>1),old);
        assert(!ActionVoxel_CutoutRows(fixed,tx+(q&1),ty+(q>>1),now));
        for(unsigned y=0;y<8;++y) {
          assert(!now[y]);
          matte[(q>>1)*8+y]|=(uint16_t)old[y]<<((q&1)?0:8);
        }
      }
      uint32_t original[256],cut[256],rear[256],front[256],old_rear[256],old_front[256];
      unsigned first=256,interior=0;
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        const unsigned at=y*16+x,pixel=FixturePixel(source,tx*8+x,ty*8+y);
        original[at]=pixel?UINT32_C(0xff030507):0;
        cut[at]=(matte[y]&(1u<<(15-x)))?0:original[at];
        if(original[at] && !cut[at]) {if(first==256)first=at;++checked;}
        if(cut[at])++interior;
      }
      assert(first<256 && interior);
      for(unsigned dim=0;dim<2;++dim) {
        const ActionRoomSceneFrameState frame={.brightness=dim?2:15,.cgadsub=0x81,.fixed_color=0x421};
        assert(ActionVoxel_Split(fixed,tx*8,ty*8,16,16,16,&frame,original,16,rear,front));
        assert(ActionVoxel_Split(reference,tx*8,ty*8,16,16,16,&frame,cut,16,old_rear,old_front));
        /* Carved faces/enclosed shadows keep their exact native foreground.
         * Exterior matte retains its source colour only at the rear plane. */
        assert(!memcmp(front,old_front,sizeof(front)));
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
          const unsigned at=y*16+x;
          if(matte[y]&(1u<<(15-x)))assert(rear[at]==original[at] && !front[at]);
          else assert(rear[at]==old_rear[at]);
        }
      }
      const ActionRoomSceneFrameState frame={.brightness=15};
      /* Missing/native-transparent input stays transparent; no invented wall. */
      original[first]=0;
      assert(ActionVoxel_Split(fixed,tx*8,ty*8,16,16,16,&frame,original,16,rear,front));
      assert(!rear[first] && !front[first]);
      original[first]=UINT32_C(0xff000000);
      memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
      DioramaPixelEdit *paint=&terrain.pixel_layers[0].edits[0];
      *paint=(DioramaPixelEdit){.by_cell=true,.x=tx/2,.y=ty/2};
      paint->black[first/16]=(uint16_t)(1u<<(15-first%16));
      assert(ActionVoxel_Build(edited,source,&terrain,settings));
      assert(ActionVoxel_Split(edited,tx*8,ty*8,16,16,16,&frame,original,16,rear,front));
      assert(front[first]==original[first]); /* Explicit opaque paint wins. */
      paint->black[first/16]=0;
      paint->transparent[first/16]=(uint16_t)(1u<<(15-first%16));
      assert(ActionVoxel_Build(edited,source,&terrain,settings));
      original[first]=0; /* Native source rasterizer applies authored transparency. */
      assert(ActionVoxel_Split(edited,tx*8,ty*8,16,16,16,&frame,original,16,rear,front));
      assert(!rear[first] && !front[first]);
    }
    /* Blue spikes use independent silhouette rules, including their black
     * exterior: never convert the hazard's empty outline into a backstop. */
    const unsigned spikes[]={0x18,0x19,0x20};
    unsigned spike_masks=0;
    for(unsigned i=0;i<sizeof(spikes)/sizeof(*spikes);++i) {
      int tx,ty;if(!FixtureCell(source,spikes[i],&tx,&ty))continue;
      for(unsigned q=0;q<4;++q) {
        uint8_t old[8]={0},now[8]={0};
        ActionVoxel_CutoutRows(reference,tx+(q&1),ty+(q>>1),old);
        spike_masks+=ActionVoxel_CutoutRows(fixed,tx+(q&1),ty+(q>>1),now);
        assert(!memcmp(old,now,8));
      }
    }
    assert(checked && spike_masks);
    printf("temple matte terrain %u: %u native rear pixels, unchanged contours/front art/spikes\n",region,checked);
    ActionVoxel_Release(fixed);ActionVoxel_Release(reference);ActionVoxel_Release(edited);
  }
}
