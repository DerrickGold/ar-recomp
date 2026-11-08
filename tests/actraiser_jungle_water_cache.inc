/* Included before TestLiveVoxelPublication; that fixture stages both native
 * backgrounds, the real ROM scene and fake PPU. No game/GPU execution. */
static unsigned JungleWaterColoredWidth(const ActionVoxelGeometry *g) {
  unsigned n,width=0;const VoxelWaterSurface *p=ActionVoxel_WaterSurfaces(g,&n);
  for(unsigned i=0;i<n;++i)if(p[i].uniform_color[0]&&p[i].uniform_color[1])width+=p[i].width;
  return width;
}
static void TestJungleWaterColorCache(Ppu *ppu,const ActionRoomScene *scene) {
  const unsigned group=scene->group,room=scene->map;
  const ActionVoxelGeometry *original=ActionVoxel_Retain(ActRaiserActionBg_VoxelGeometry(group,room));
  CHECK(original);if(!original)return;
  unsigned count;const VoxelWaterSurface *p=ActionVoxel_WaterSurfaces(original,&count);CHECK(count);
  if(!count){ActionVoxel_Release(original);return;}
  const unsigned full=(room==1?80u:room==2?96u:32u)*16;
  CHECK(JungleWaterColoredWidth(original)==full);
  const unsigned mx=(unsigned)(p[0].x/16)%(scene->bg[1].pages_wide*16),cy=(unsigned)p[0].y/16;
  uint64_t revision=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);
  CHECK(PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL)==revision);
  /* The static deep-water donor is outside both BG1 and native wave inputs. */
  const unsigned ch=256+scene->bg[1].metatiles[0x1d*8+1];
  ppu->vram[ch*16]^=1;
  uint64_t next=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);CHECK(next!=revision);
  CHECK(JungleWaterColoredWidth(ActRaiserActionBg_VoxelGeometry(group,room))==0);
  CHECK(JungleWaterColoredWidth(original)==full); /* Queued old generation immutable. */
  ppu->vram[ch*16]^=1;revision=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);
  CHECK(revision!=next&&JungleWaterColoredWidth(ActRaiserActionBg_VoxelGeometry(group,room))==full);
  /* An alternate actual BG2 character base must not borrow BG1 VRAM bytes. */
  ppu->bgTileAdr^=0x10;
  next=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);CHECK(next!=revision);
  CHECK(JungleWaterColoredWidth(ActRaiserActionBg_VoxelGeometry(group,room))==0);
  ppu->bgTileAdr^=0x10;revision=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);
  CHECK(revision!=next&&JungleWaterColoredWidth(ActRaiserActionBg_VoxelGeometry(group,room))==full);
  /* Local blank/high source with a valid donor elsewhere must opt out. */
  const size_t map_at=(cy/16*scene->bg[1].pages_wide+mx/16)*256+(cy&15)*16+(mx&15);
  const uint8_t id=g_ram[kActRaiserWram_Bg2Map+map_at];
  g_ram[kActRaiserWram_Bg2Map+map_at]=0x1c;
  next=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);CHECK(next!=revision);
  CHECK(JungleWaterColoredWidth(ActRaiserActionBg_VoxelGeometry(group,room))<full);
  g_ram[kActRaiserWram_Bg2Map+map_at]=id;
  revision=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);CHECK(revision!=next);
  g_ram[kActRaiserWram_Bg2MetatileDefinitions+id*8+1]^=0x20;
  next=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);CHECK(next!=revision);
  CHECK(JungleWaterColoredWidth(ActRaiserActionBg_VoxelGeometry(group,room))<full);
  /* Modified BG2 definitions do not borrow the native phase proof. */
  size_t animation_offset,animation_bytes;
  CHECK(ActionVoxel_AnimationMatches(scene,scene->characters,&animation_offset,&animation_bytes));
  uint8_t animated[kActionRoomSceneCharacterBytes];
  CHECK(ActionRoomScene_BuildCharacters(scene,0,1,animated,sizeof(animated)));
  for(size_t i=animation_offset;i<animation_offset+animation_bytes;i+=2)
    ppu->vram[i/2]=animated[i]|(animated[i+1]<<8);
  CHECK(PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL)!=next);
  for(size_t i=animation_offset;i<animation_offset+animation_bytes;i+=2)
    ppu->vram[i/2]=scene->characters[i]|(scene->characters[i+1]<<8);
  g_ram[kActRaiserWram_Bg2MetatileDefinitions+id*8+1]^=0x20;
  revision=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);CHECK(revision!=next);
  ppu->cgram[41]^=1;next=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);CHECK(next!=revision);
  const VoxelWaterSurface *changed=ActionVoxel_WaterSurfaces(ActRaiserActionBg_VoxelGeometry(group,room),&count);
  CHECK(count&&changed[0].uniform_color[1]==0xb191);
  ppu->cgram[41]^=1;revision=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);CHECK(revision!=next);
  static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
  terrain.pixel_layers[1].count=1;DioramaPixelEdit *edit=&terrain.pixel_layers[1].edits[0];
  edit->by_cell=true;edit->x=(int16_t)mx;edit->y=(int16_t)cy;edit->black[13]=1;
  next=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,&terrain);CHECK(next!=revision);
  CHECK(JungleWaterColoredWidth(ActRaiserActionBg_VoxelGeometry(group,room))<full);
  CHECK(PrepareVoxelWithoutChangingNativeArt(ppu,group,room,&terrain)==next);
  revision=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);CHECK(revision!=next);
  CHECK(JungleWaterColoredWidth(ActRaiserActionBg_VoxelGeometry(group,room))==full);
  CHECK(PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL)==revision);
  DioramaLayerOrder_ClearRoom(&terrain);
  CHECK(DioramaLayerOrder_ParseLine(&terrain,
      "bg2-stamp = cell:0,0 metatile:00 words:0900,0900,0900,0900 bands:1,1,1,1",NULL));
  revision=PrepareVoxelWithoutChangingNativeArt(ppu,group,room,&terrain);
  for(size_t i=animation_offset;i<animation_offset+animation_bytes;i+=2)
    ppu->vram[i/2]=animated[i]|(animated[i+1]<<8);
  CHECK(PrepareVoxelWithoutChangingNativeArt(ppu,group,room,&terrain)!=revision);
  DioramaLayerOrder_ClearRoom(&terrain);
  for(size_t i=animation_offset;i<animation_offset+animation_bytes;i+=2)
    ppu->vram[i/2]=scene->characters[i]|(scene->characters[i+1]<<8);
  (void)PrepareVoxelWithoutChangingNativeArt(ppu,group,room,NULL);
  ActionVoxel_Release(original);
}
