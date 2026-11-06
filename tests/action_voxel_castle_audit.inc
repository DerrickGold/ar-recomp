/* User-audited castle contracts. Fixture IDs only locate the reviewed native
 * artwork; each compiled scene is also checked after metatile/character
 * renumbering. Include after the shared fixture helpers in action_voxel_test.c. */
static bool CastleAuditRearTrimAt(const ActionVoxelGeometry *g,int x,int y) {
  unsigned count;const VoxelRearTrim *trim=ActionVoxel_RearTrims(g,&count);
  for(unsigned i=0;i<count;++i)
    if(trim[i].y==y && trim[i].x<=x && trim[i].x+trim[i].width>x)return true;
  return false;
}
static void CastleAuditJambBacking(const ActionVoxelGeometry *g,const ActionRoomScene *s,
                                  int tx,int ty,const DioramaPixelEdit *edit) {
  /* This native high-priority face has no pixels in BG1Low. Its opaque ink
   * must still occlude the sky after moving forward, without copying the
   * colored jamb to the rear. Native color zero and authored holes stay open. */
  const uint32_t low[256]={0};uint32_t rear[256],front[256];
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(g,tx*8,ty*8,16,16,16,&frame,low,16,rear,front));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const bool opaque=FixturePixel(s,tx*8+x,ty*8+y) &&
        !(edit && (edit->transparent[y]&(1u<<(15-x))));
    assert(rear[y*16+x]==(opaque?0xff000000u:0));
    assert(front[y*16+x]==0); /* The native high-priority source is separate. */
  }
}
static void CastleAuditFrontArtwork(const ActionVoxelGeometry *g,const ActionRoomScene *s,
                                   int tx,int ty,bool thin) {
  uint32_t low[256],rear[256],front[256];
  DesertNativeLow(s,tx,ty,low);
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(g,tx*8,ty*8,16,16,16,&frame,low,16,rear,front));
  unsigned colored=0;
  for(unsigned i=0;i<256;++i)if(low[i]&0xffffffu) {
    assert(front[i]==low[i]);
    /* A thin support/chain replaces its rear colored copy with the native
     * castle interior shadow. Never show two colored versions of the art. */
    if(thin)assert(rear[i]==0xff000000u);
    ++colored;
  }
  assert(colored);
}
static void CastleAuditCapTop(const ActionVoxelGeometry *g,int x,int y) {
  unsigned count;const VoxelEdge *edge=ActionVoxel_Edges(g,&count);
  unsigned covered=0;
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edge[i];
    if(e->side || e->y!=y || !e->colliding || e->far_rank!=2)continue;
    assert(!e->high_priority);
    const int left=e->x>x?e->x:x,right=e->x+e->length<x+16?e->x+e->length:x+16;
    if(right<=left)continue;
    VoxelEdgeSpan span;
    assert(ActionVoxel_ResolveEdgeSpan(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),&span));
    const float contact=ActionVoxel_PlayRecess(g)-.01f;
    assert(span.near_depth<=contact && span.far_depth>=contact);
    covered+=(unsigned)(right-left);
  }
  assert(covered==16);
}
static unsigned CastleAuditFixtures(const char *directory) {
  static ActionSceneSnapshot snapshot;
  static ActionRoomScene phase,renumbered;
  static uint8_t bytes[kActionSceneSnapshotMaxBytes];
  unsigned checked=0,chain_phases=0,doorways=0,capitals=0,reliefs=0,crenellations=0,portal_edges=0;
  for(unsigned room=2;room<=8;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/02%02u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    size_t size=fread(bytes,1,sizeof(bytes),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(bytes,size,&snapshot));
    ActionVoxelGeometry *g=ActionVoxel_Create(),*other=ActionVoxel_Create();assert(g&&other);
    unsigned phases=room==3||room==5?ActionRoomScene_CharacterAnimationPhaseCount(&snapshot.scene):1;
    assert(phases);
    for(unsigned p=0;p<phases;++p) {
      phase=snapshot.scene;
      assert(ActionRoomScene_BuildCharacters(&snapshot.scene,0,p,phase.characters,sizeof(phase.characters)));
      const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,room);
      assert(ActionVoxel_Build(g,&phase,NULL,settings));
      renumbered=phase;Renumber(&renumbered);
      assert(ActionVoxel_Build(other,&renumbered,NULL,settings));SameGeometry(g,other);
      unsigned chains=0;bool tested_jamb_edit=false;
      for(unsigned ty=0;ty<ActionRoomScene_TileHeight(&phase,1);ty+=2)
        for(unsigned tx=0;tx<ActionRoomScene_TileWidth(&phase,1);tx+=2) {
          uint16_t word;uint8_t id;
          assert(ActionRoomScene_LookupTile(&phase,1,tx,ty,&word,&id));
          if((room==3||room==5) && (id==1||id==2)) {
            /* Every native animation pose must stay on the same front plane. */
            assert(!(word&0x2000));CastleAuditFrontArtwork(g,&phase,tx,ty,true);++chains;
          }
          if(p)continue;
          if(id==0x43||id==0x4b) {
            assert(word&0x2000);
            assert(!CastleAuditRearTrimAt(g,tx*8,ty*8));
            CastleAuditJambBacking(g,&phase,tx,ty,NULL);
            if(!tested_jamb_edit) {
              DioramaRoomOverride *terrain=calloc(1,sizeof(*terrain));assert(terrain);
              terrain->pixel_layers[0].count=1;
              DioramaPixelEdit *edit=&terrain->pixel_layers[0].edits[0];
              *edit=(DioramaPixelEdit){.by_cell=true,.x=tx/2,.y=ty/2};
              edit->transparent[7]=0x0ff0;
              /* Explicit transparency wins even if the same edit marks black. */
              edit->black[7]=0x0100;
              assert(ActionVoxel_Build(other,&phase,terrain,settings));
              CastleAuditJambBacking(other,&phase,tx,ty,edit);
              free(terrain);tested_jamb_edit=true;
            }
            ++doorways;
          }
          if(id==0x11||id==0x12) {
            assert(word&0x2000);
            assert(CastleAuditRearTrimAt(g,tx*8,ty*8));++crenellations;
          }
          if(id==0xfb||id==0xfd) {
            /* Adjacent opaque, noncolliding wall edge: the front jamb's
             * footprint alone ends one native cell before its projected face.
             * Retain the existing wall's black rear, not another gold jamb. */
            assert(word&0x2000);
            CastleAuditJambBacking(g,&phase,tx,ty,NULL);
            assert(!CastleAuditRearTrimAt(g,tx*8,ty*8));
            if(room==7 && tx*8==880 && ty*8>=128 && ty*8<=192) {
              ++portal_edges;
              renumbered=phase;Renumber(&renumbered);
              assert(ActionVoxel_Build(other,&renumbered,NULL,settings));
              CastleAuditJambBacking(other,&renumbered,tx,ty,NULL);
              /* New source color zero is a real hole. Changing artwork also
               * changes its semantic signature; never extend the reviewed
               * opaque-wall promise to an unreviewed transparent variant. */
              renumbered=phase;
              uint8_t *character=renumbered.characters+(word&1023)*32;
              unsigned sx=word&0x4000?7:0,sy=word&0x8000?7:0;
              const uint8_t keep=(uint8_t)~(1u<<(7-sx));
              character[sy*2]&=keep;character[sy*2+1]&=keep;
              character[sy*2+16]&=keep;character[sy*2+17]&=keep;
              assert(!FixturePixel(&renumbered,tx*8,ty*8));
              assert(ActionVoxel_Build(other,&renumbered,NULL,settings));
              const uint32_t empty[256]={0};uint32_t rear[256],front[256];
              const ActionRoomSceneFrameState frame={.brightness=15};
              assert(ActionVoxel_Split(other,tx*8,ty*8,16,16,16,&frame,empty,16,rear,front));
              assert(!rear[0] && !front[0]);
            }
          }
          if(room==8 && (id==0x36||id==0x37)) {
            const uint8_t *definition=phase.bg[0].metatiles+id*8;
            unsigned collision=0;
            for(unsigned q=0;q<4;++q)collision|=!!(((definition[q*2]<<8)|definition[q*2+1])&0x200)<<q;
            assert(collision==3);
            CastleAuditFrontArtwork(g,&phase,tx,ty,false);CastleAuditCapTop(g,tx*8,ty*8);++capitals;
          }
          if(room==8 && id>=0xce && id<=0xe3 && id!=0xdb) {
            CastleAuditFrontArtwork(g,&phase,tx,ty,true);++reliefs;
          }
        }
      if(room==3||room==5){assert(chains);++chain_phases;}
    }
    /* The portal material correction must not remove the closed gate. */
    if(room==8) {
      unsigned count;const VoxelGate *gates=ActionVoxel_Gates(g,&count);
      assert(count==1 && gates[0].x==0 && gates[0].y==152 && gates[0].height==64);
    }
    ActionVoxel_Release(other);ActionVoxel_Release(g);++checked;
  }
  assert(checked==21 && chain_phases==24 && doorways && capitals==36 && reliefs && crenellations && portal_edges==15);
  return checked;
}
