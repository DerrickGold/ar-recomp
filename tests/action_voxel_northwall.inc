/* ROM-derived fixture checks are optional and remain local. IDs below locate
 * reviewed source art only; the remapping check proves runtime independence. */
#include "action/action_bg_metatile.h"

static void NorthwallAnimation(const ActionRoomScene *source,ActionVoxelGeometry *a,
    ActionVoxelGeometry *b,ActionVoxelSettings settings) {
  static ActionRoomScene live;
  size_t offset,bytes;
  assert(ActionVoxel_AnimationCacheCandidate(kActionVoxelProfile_NorthwallSnow));
  assert(!ActionVoxel_AnimationCacheCandidate(kActionVoxelProfile_NorthwallIce));
  assert(ActionVoxel_AnimationMatches(source,source->characters,&offset,&bytes));
  assert(!offset && bytes==128 && ActionRoomScene_CharacterAnimationPhaseCount(source)==4);
  assert(ActionRoomScene_CharacterAnimationCadence(source)==8);
  /* The native animation is absent from this room's placed BG1 cells, but
   * unused definitions still reference it. They must be proved, not dropped
   * from the compiler inputs: an editor or live publication can place them. */
  unsigned unused=0,animated_id=256;
  for(unsigned id=0;id<256;++id) {
    bool animated=false;
    for(unsigned q=0;q<4;++q) {
      const uint8_t *definition=source->bg[0].metatiles+id*8+q*2;
      const size_t at=(size_t)definition[1]*32;
      animated|=at>=offset && at<offset+bytes;
    }
    if(!animated)continue;
    if(animated_id==256)animated_id=id;
    for(size_t i=0;i<source->bg[0].map_size;++i)assert(source->bg[0].map[i]!=id);
    ++unused;
  }
  assert(unused && animated_id<256);
  const unsigned w=ActionRoomScene_TileWidth(source,1),h=ActionRoomScene_TileHeight(source,1);
  uint16_t *tiles=malloc((size_t)w*h*sizeof(*tiles));assert(tiles);
  int solid_x=-1,solid_y=-1;unsigned solid_id=256;
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
    uint8_t id;
    assert(ActionRoomScene_LookupTile(source,1,x,y,&tiles[(size_t)y*w+x],&id));
    const unsigned q=((y&1)<<1)|(x&1);
    if(solid_id==256 && (source->bg[0].metatiles[id*8+q*2]&2)) {
      solid_x=x;solid_y=y;solid_id=id;
    }
  }
  assert(solid_id<256);
  assert(ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,tiles,w));
  assert(!ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,tiles,w-1));
  for(unsigned phase=0;phase<4;++phase) {
    live=*source;
    assert(ActionRoomScene_BuildCharacters(source,0,phase,live.characters,sizeof(live.characters)));
    assert(ActionVoxel_AnimationMatches(source,live.characters,&offset,&bytes));
    assert(ActionVoxel_AnimationIsPresentationOnly(&live,source,NULL,tiles,w));
    assert(ActionVoxel_BuildWithTiles(b,&live,NULL,settings,tiles,w));SameGeometry(a,b);
    /* Check the observable split over every cell, including empty/rear art,
     * rather than accepting equal edge lists with different source masks. */
    const ActionRoomSceneFrameState frame={.brightness=15};
    uint32_t input[256],rear_a[256],front_a[256],rear_b[256],front_b[256];
    for(unsigned i=0;i<256;++i)input[i]=0xff010000u+i;
    for(unsigned y=0;y<h;y+=2)for(unsigned x=0;x<w;x+=2) {
      assert(ActionVoxel_Split(a,x*8,y*8,16,16,16,&frame,input,16,rear_a,front_a));
      assert(ActionVoxel_Split(b,x*8,y*8,16,16,16,&frame,input,16,rear_b,front_b));
      assert(!memcmp(rear_a,rear_b,sizeof(rear_a)) && !memcmp(front_a,front_b,sizeof(front_a)));
      assert(ActionVoxel_FrontRecess(a,x,y)==ActionVoxel_FrontRecess(b,x,y));
    }
  }
  /* Unknown edits and partially published native phases must never reuse a
   * canonical mesh. Restoring a complete known phase permits proof again. */
  live=*source;live.characters[offset]^=0xff;
  assert(!ActionVoxel_AnimationMatches(source,live.characters,&offset,&bytes));
  assert(!ActionVoxel_AnimationIsPresentationOnly(&live,source,NULL,tiles,w));
  bool partial=false;
  for(size_t split=32;split<bytes && !partial;split+=32) {
    live=*source;
    memcpy(live.characters+offset+split,source->characters+offset+bytes+split,bytes-split);
    partial=!ActionVoxel_AnimationMatches(source,live.characters,&offset,&bytes);
  }
  assert(partial && !ActionVoxel_AnimationIsPresentationOnly(&live,source,NULL,tiles,w));
  live=*source;assert(ActionVoxel_AnimationIsPresentationOnly(&live,source,NULL,tiles,w));
  /* An unused definition becoming colliding is just as unsafe as a placed
   * tile. The guard must reject it, then recover when that edit is undone. */
  live.bg[0].metatiles[animated_id*8]|=2;
  assert(!ActionVoxel_AnimationIsPresentationOnly(&live,source,NULL,tiles,w));
  live=*source;assert(ActionVoxel_AnimationIsPresentationOnly(&live,source,NULL,tiles,w));
  const size_t at=(size_t)solid_y*w+solid_x;
  const uint16_t original=tiles[at];
  tiles[at]=(original&~1023u)|(unsigned)(offset/32);
  assert(!ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,tiles,w));
  tiles[at]=original;
  assert(ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,tiles,w));

  DioramaRoomOverride *terrain=calloc(1,sizeof(*terrain));assert(terrain);
  DioramaTileStamp stamp={.x=-1,.y=-1,.metatile=animated_id};
  for(unsigned q=0;q<4;++q)
    stamp.words[q]=ActionBg_ComposeTilemapWord(
        (uint16_t)source->bg[0].metatiles[animated_id*8+q*2]<<8 |
            source->bg[0].metatiles[animated_id*8+q*2+1],
        kActionRoomSceneTileWordMask,ActionRoomScene_BgAttributes(source,1));
  terrain->stamp_layers[0].cells=&stamp;terrain->stamp_layers[0].count=1;
  terrain->stamp_layers[0].set_bounds=true;
  terrain->stamp_layers[0].x0=-1;terrain->stamp_layers[0].y0=-1;
  terrain->stamp_layers[0].x1=w/2;terrain->stamp_layers[0].y1=h/2;
  terrain->pixel_layers[0].count=1;
  terrain->pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=-1,.y=-1};
  terrain->pixel_layers[0].edits[0].transparent[3]=0x5555;
  terrain->pixel_layers[0].edits[0].black[5]=0xaaaa;
  assert(ActionVoxel_AnimationIsPresentationOnly(source,source,terrain,tiles,w));
  assert(ActionVoxel_BuildWithTiles(a,source,terrain,settings,tiles,w));
  for(unsigned phase=0;phase<4;++phase) {
    live=*source;
    assert(ActionRoomScene_BuildCharacters(source,0,phase,live.characters,sizeof(live.characters)));
    assert(ActionVoxel_AnimationIsPresentationOnly(&live,source,terrain,tiles,w));
    assert(ActionVoxel_BuildWithTiles(b,&live,terrain,settings,tiles,w));SameGeometry(a,b);
  }
  /* A stamp can combine the same animated words with a solid native
   * collision contract, even outside the original room's bounds. */
  stamp.metatile=solid_id;
  for(unsigned q=0;q<4;++q)stamp.words[q]=(unsigned)(offset/32)+q;
  assert(!ActionVoxel_AnimationIsPresentationOnly(source,source,terrain,tiles,w));
  terrain->stamp_layers[0].count=0;
  assert(ActionVoxel_AnimationIsPresentationOnly(source,source,terrain,tiles,w));
  free(terrain);free(tiles);
  assert(ActionVoxel_Build(a,source,NULL,settings));
  printf("Northwall animation: %u unused definitions, 4 native + 4 edited phases, split parity and unsafe-input recovery\n",unused);
}

static void NorthwallForegroundPillars(const ActionRoomScene *s, const ActionVoxelGeometry *g) {
  const unsigned char ids[] = {0x57, 0x5f, 0x67, 0x6f, 0x77};
  unsigned cells = 0, count, sides = 0;
  const VoxelEdge *edges = ActionVoxel_Edges(g, &count);
  const float depth = ActionVoxel_Depth(g);
  const float actor = ActionVoxel_PlayRecess(g) - .01f;
  for (int y = 0; y < (int)ActionRoomScene_TileHeight(s, 1) / 2; ++y)
    for (int x = 0; x < (int)ActionRoomScene_TileWidth(s, 1) / 2; ++x) {
      uint8_t id;
      uint16_t word;
      assert(ActionRoomScene_LookupTile(s, 1, x * 2, y * 2, &word, &id));
      if (!memchr(ids, id, sizeof(ids))) continue;
      VoxelCellGeometry geometry;
      assert(ActionVoxel_CellGeometry(g, x, y, &geometry));
      for (unsigned q = 0; q < 4; ++q) {
        assert(ActionRoomScene_LookupTile(s, 1, x * 2 + (q & 1), y * 2 + (q >> 1), &word, NULL));
        assert((word & 0x2000) && !(s->bg[0].metatiles[id * 8 + q * 2] & 2));
        assert(geometry.front[q] == 0 && geometry.back[q] < actor);
      }
      const ActionRoomSceneFrameState frame = {.brightness = 15};
      uint32_t rear[256], front[256];
      assert(ActionVoxel_Split(g, x * 16, y * 16, 16, 16, 16, &frame, NULL, 0, rear, front));
      for (unsigned p = 0; p < 256; ++p)
        assert(rear[p] == 0xff000000u && front[p] == 0);
      if (id == 0x57 || id == 0x77)
        for (int dx = -1; dx <= 1; dx += 2) {
          /* The old cap/base silhouette extends into these rear tiles. */
          uint32_t source[256];
          for (unsigned py = 0; py < 16; ++py)
            for (unsigned px = 0; px < 16; ++px)
              source[py * 16 + px] =
                  ForestNativeColor(s, (x + dx) * 16 + px, y * 16 + py, &frame);
          assert(ActionVoxel_Split(g, (x + dx) * 16, y * 16, 16, 16, 16,
                                   &frame, source, 16, rear, front));
          for (unsigned p = 0; p < 256; ++p)
            assert(rear[p] == (source[p] ? source[p] : 0xff000000u) && front[p] == 0);
        }
      ++cells;
      for (unsigned i = 0; i < count; ++i) {
        const VoxelEdge *e = &edges[i];
        if (!(e->side & 1) || e->far_rank != 1 || e->x / 16 != x || e->y / 16 != y)
          continue;
        VoxelEdgeSpan span;
        assert(e->high_priority && !e->colliding && e->thin_trim);
        assert(ActionVoxel_ResolveEdgeSpan(g, e, kVoxelEdgeNative, 0, depth, &span));
        assert(span.near_depth == 0 && span.far_depth < actor);
        ++sides;
      }
    }
  assert(cells && sides);
  printf("Northwall 06/%02u foreground ice columns: %u cells, %u thin sides ahead of OBJ2\n",
      s->map, cells, sides);
}

static void NorthwallForegroundEntrance(const ActionRoomScene *s, const ActionVoxelGeometry *g) {
  const unsigned char ids[] = {0xd1, 0xd9, 0xe1, 0xe2, 0xe3};
  unsigned cells = 0;
  const float actor = ActionVoxel_PlayRecess(g) - .01f;
  const ActionRoomSceneFrameState frame = {.brightness = 15};
  for (int y = 0; y < (int)ActionRoomScene_TileHeight(s, 1) / 2; ++y)
    for (int x = 0; x < (int)ActionRoomScene_TileWidth(s, 1) / 2; ++x) {
      uint8_t id;
      uint16_t word;
      assert(ActionRoomScene_LookupTile(s, 1, x * 2, y * 2, &word, &id));
      if (!memchr(ids, id, sizeof(ids))) continue;
      VoxelCellGeometry geometry;
      assert(ActionVoxel_CellGeometry(g, x, y, &geometry));
      uint32_t source[256] = {0}, rear[256], front[256];
      for (unsigned q = 0; q < 4; ++q) {
        assert(ActionRoomScene_LookupTile(s, 1, x * 2 + (q & 1), y * 2 + (q >> 1), &word, NULL));
        if (word & 0x2000) {
          assert(geometry.front[q] == 0 && geometry.back[q] < actor);
          continue;
        }
        for (unsigned py = (q >> 1) * 8; py < (q >> 1) * 8 + 8; ++py)
          for (unsigned px = (q & 1) * 8; px < (q & 1) * 8 + 8; ++px)
            source[py * 16 + px] = ForestNativeColor(s, x * 16 + px, y * 16 + py, &frame);
      }
      assert(ActionVoxel_Split(g, x * 16, y * 16, 16, 16, 16, &frame, source, 16, rear, front));
      for (unsigned p = 0; p < 256; ++p)
        assert(rear[p] == (source[p] ? source[p] : 0xff000000u));
      ++cells;
    }
  assert(cells == 17);
}

static unsigned NorthwallFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  const unsigned rooms[]={1,2,3,4,5,8};
  for(unsigned r=0;r<sizeof(rooms)/sizeof(*rooms);++r)for(unsigned region=0;region<3;++region) {
    const unsigned room=rooms[r];const bool ice=room>=2 && room<=4;
    char path[1024];snprintf(path,sizeof(path),"%s/060%u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    const ActionRoomScene *s=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,room);
    assert(ActionVoxel_Build(a,s,NULL,settings) && ActionVoxel_Active(a,6,room));
    assert(ActionVoxel_Profile(a)==(ice?kActionVoxelProfile_NorthwallIce:kActionVoxelProfile_NorthwallSnow));
    assert(ActionVoxel_Depth(a)==(ice?.22f:.20f));
    assert(ActionVoxel_PlayRecess(a)==(ice?.075f:.065f));
    if(room==5) {
      NorthwallAnimation(s,a,b,settings);
      NorthwallForegroundEntrance(s,a);
    }
    variant=*s;Renumber(&variant);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    variant=*s;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    for(unsigned phase=0;phase<ActionRoomScene_CharacterAnimationPhaseCount(s);++phase) {
      variant=*s;
      assert(ActionRoomScene_BuildCharacters(s,0,phase,variant.characters,sizeof(variant.characters)));
      assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    }
    unsigned count,solid=0,rear=0,decks=0;
    const VoxelEdge *edges=ActionVoxel_Edges(a,&count);assert(count && count<kVoxelMaxEdges);
    const float depth=ActionVoxel_Depth(a),actor=ActionVoxel_PlayRecess(a)-.01f;
    for(unsigned i=0;i<count;++i) {
      const VoxelEdge *e=&edges[i];solid+=e->colliding!=0;rear+=e->rear_mounted!=0;
      if(e->high_priority && e->colliding && !e->side && e->far_rank==2) {
        VoxelEdgeSpan front,back;
        if(ActionVoxel_ResolveEdgeSpan(a,e,kVoxelEdgeNative,0,depth,&front) &&
            ActionVoxel_ResolveEdgeSpan(a,e,kVoxelEdgeHighDeckBehind,0,depth,&back)) {
          assert(front.far_depth<=actor && back.near_depth>=actor);++decks;
        }
      }
    }
    assert(solid);if(room==1 || room==8)assert(decks);
    if(room==5)assert(rear);
    if(room==2 || room==3 || room==5)TombIntervals(a);
    if(ice) {
      if (room <= 3) NorthwallForegroundPillars(s, a);
      assert(ActionVoxel_HasRearRelief(a));
      unsigned water_count,width=0,expected=0;
      const VoxelWaterSurface *water=ActionVoxel_WaterSurfaces(a,&water_count);assert(water_count);
      for(size_t i=0;i<s->bg[1].map_size;++i) {
        const unsigned id=s->bg[1].map[i];
        if(id==1 || id==0xb0 || id==0xb1)expected+=16;
      }
      for(unsigned i=0;i<water_count;++i) {
        assert(water[i].background==1 && !water[i].opaque);
        assert(water[i].sample_y==3 && water[i].sample_height==8);width+=water[i].width;
      }
      assert(width==expected);
      DesertMaterial(a,s,0x60,kVoxelSolid);
      DesertMaterial(a,s,0x8a,kVoxelRearOnly); /* Preserve opaque cave interior. */
    } else if(room==1)DesertMaterial(a,s,1,kVoxelSolid);
    else if(room==5)DesertMaterial(a,s,0x5f,kVoxelSolid);
    else DesertMaterial(a,s,0xf3,kVoxelSolid);
    /* The exit masks the player in native OBJ2, so its front jamb is thin
     * and stays entirely ahead of that walking plane. */
    int gate_x,gate_y;
    if(room<=3 && FixtureCell(s,0x7c,&gate_x,&gate_y)) {
      unsigned leaf_faces=0;
      for(unsigned i=0;i<count;++i) {
        const VoxelEdge *e=&edges[i];
        if(e->x<gate_x*8 || e->x>gate_x*8+16 || e->y<gate_y*8 || e->y>gate_y*8+16)continue;
        if(e->high_priority && !e->colliding && e->far_rank==1) {
          VoxelEdgeSpan span;
          assert(ActionVoxel_ResolveEdgeSpan(a,e,kVoxelEdgeNative,0,depth,&span));
          assert(e->thin_trim && !e->rear_mounted);
          assert(span.near_depth==0 && span.far_depth<=depth*.10001f);
          assert(span.far_depth<settings.play_recess-.01f);++leaf_faces;
        }
      }
      assert(leaf_faces);
      const ActionRoomSceneFrameState gate_frame={.brightness=15};
      uint32_t gate_rear[256],gate_front[256];
      assert(ActionVoxel_Split(a,gate_x*8,gate_y*8,16,16,16,
                               &gate_frame,NULL,0,gate_rear,gate_front));
      unsigned backed=0;
      for(unsigned p=0;p<256;++p) {
        assert(!gate_rear[p] || gate_rear[p]==0xff000000u);
        backed+=gate_rear[p]!=0;
      }
      assert(backed);
      variant=*s;
      for(unsigned q=0;q<4;++q) {
        uint16_t word;
        assert(ActionRoomScene_LookupTile(s,1,gate_x+(q&1),gate_y+(q>>1),&word,NULL));
        memset(variant.characters+(word&1023)*32,0,32);
      }
      assert(ActionVoxel_Build(b,&variant,NULL,settings) && ActionVoxel_Active(b,6,room));
      assert(ActionVoxel_Split(b,gate_x*8,gate_y*8,16,16,16,
                               &gate_frame,NULL,0,gate_rear,gate_front));
      for(unsigned p=0;p<256;++p)assert(!gate_rear[p] && !gate_front[p]);
      unsigned cleared_count;const VoxelEdge *cleared=ActionVoxel_Edges(b,&cleared_count);
      for(unsigned i=0;i<cleared_count;++i) {
        const VoxelEdge *e=&cleared[i];
        if(e->x<gate_x*8 || e->x>gate_x*8+16 || e->y<gate_y*8 || e->y>gate_y*8+16)continue;
        assert(!(e->high_priority && !e->colliding && e->far_rank==2));
      }
    }
    /* Snow, ice and summit profiles must not invent a backdrop behind empty
     * artwork. This also checks the canopy's freestanding border. */
    uint32_t low[64]={0},back[64],front[64];const ActionRoomSceneFrameState frame={.brightness=15};
    assert(ActionVoxel_Split(a,-32,-32,8,8,8,&frame,low,8,back,front));
    for(unsigned i=0;i<64;++i)assert(!back[i] && !front[i]);
    variant=*s;memset(variant.characters,0,sizeof(variant.characters));
    assert(ActionVoxel_Build(b,&variant,NULL,settings) && !ActionVoxel_Active(b,6,room));
    printf("Northwall %u terrain %u: %u contours, %u rear, %u split walking tops\n",room,region,count,rear,decks);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  assert(!ActionVoxel_Room(6,6) && !ActionVoxel_Room(6,7)); /* Curved trunk owns them. */
  return 18;
}
