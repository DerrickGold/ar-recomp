/* Solid color for the added Jungle pool deck only. Native water/foam and
 * animated BG2 waves remain captured faces at their original scale. The
 * main-screen tone is the room's reviewed uniform deep-water material, not
 * a stone donor or an arbitrary RGB. Fingerprints survive CHR/metatile moves. */
/* Query only the source cell used by the added deck or native donor. Map
 * extensions elsewhere do not change this ownership; hidden, blank or moved
 * priority artwork at this exact coordinate keeps captured sampling. */
static bool JungleWaterWords(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,uint16_t words[4],uint8_t *id) {
  const DioramaStampLayerOverride *layer=terrain?&terrain->stamp_layers[1]:NULL;
  if(layer && layer->set_bounds &&
      (x<layer->x0 || y<layer->y0 || x>=layer->x1 || y>=layer->y1))return false;
  const DioramaTileStamp *stamp=layer?DioramaLayerOrder_StampAt(layer,x,y):NULL;
  if(stamp) {
    if(stamp->blank || stamp->bands!=0x55)return false;
    memcpy(words,stamp->words,sizeof(stamp->words));*id=stamp->metatile;return true;
  }
  for(unsigned q=0;q<4;++q)
    if(!ActionRoomScene_LookupTile(s,2,x*2+(q&1),y*2+(q>>1),&words[q],id))return false;
  return true;
}
/* Exactly the four native shoreline wave families in their four phases.
 * Identity includes all displayed ink, palette and priority. Unknown/blank/
 * high source art cannot borrow a solid tone from an unrelated intact donor. */
static bool JungleWaterMainSurface(const ActionRoomScene *s,const uint16_t words[4]) {
  static const uint64_t keys[]={
    UINT64_C(0x03c3d2ebda2c0937),
    UINT64_C(0x1134bc82f1f2131d),
    UINT64_C(0x15514b9f0a44a3b7),
    UINT64_C(0x1f2a70a5433bf485),
    UINT64_C(0x3aba7b78270a01e6),
    UINT64_C(0x42fd641f48f74f58),
    UINT64_C(0x45134da639264ec1),
    UINT64_C(0x51d69a53a7ace0d2),
    UINT64_C(0x8995ee6e8d2ffb7a),
    UINT64_C(0x8fd782b6ecfae28d),
    UINT64_C(0x90a456b03beee1f9),
    UINT64_C(0xa0fd6278cb6bbd42),
    UINT64_C(0xc9464b582e0b2248),
    UINT64_C(0xf21960c66e9ba6cf),
    UINT64_C(0xf36d12a3ff6eb5c6),
    UINT64_C(0xfade4c91d2456075),
  };
  CastleMaterial material; Material(s,1,words,0,&material);
  const uint64_t key=MaterialKey(&material,0);
  for (unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i) if (keys[i]==key) return true;
  return false;
}
static uint16_t JungleWaterMainColor(const ActionRoomScene *s,
    const DioramaRoomOverride *terrain, const uint16_t palette[128]) {
  if (ActionVoxel_ProfileForRoom(s->group,s->map)!=kActionVoxelProfile_MarahnaJungle ||
      !ActionRoomScene_HasBackground(s,2) || !s->have_character_bank[1] ||
      !s->have_video_profile || s->video_profile[9]!=0x22 || s->video_profile[10]!=0x22 ||
      s->raster_effect!=kActionRoomRaster_None) return 0;
  const unsigned width=ActionRoomScene_TileWidth(s,2), height=ActionRoomScene_TileHeight(s,2);
  bool checked[256]={false}; uint16_t colors[256]={0},checked_words[256][4];
  for (unsigned y=0;y<height;y+=2) for (unsigned x=0;x<width;x+=2) {
    uint16_t words[4]; uint8_t id;
    if (!JungleWaterWords(s,terrain,x/2,y/2,words,&id)) continue;
    if (!checked[id] || memcmp(words,checked_words[id],sizeof(words))) {
      checked[id]=true;colors[id]=0;memcpy(checked_words[id],words,sizeof(words));
      bool valid=true;
      CastleMaterial material; Material(s,1,words,0,&material);
      if (MaterialKey(&material,0)!=UINT64_C(0x0646168c3c1771e5)) continue;
      /* The fingerprint includes palette2, low priority and all256 index9
       * pixels. Reject a remapped donor inside an animated character window. */
      for (unsigned q=0;q<4;++q) valid &= SurfaceStaticWord(s,words[q]);
      if (valid) colors[id]=(uint16_t)(0x8000 | palette[((words[0]>>10)&7)*16+material.pixels[0][0]]);
    }
    if (!colors[id]) continue;
    const DioramaPixelEdit *edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,1,x/2,y/2,id):NULL;
    bool intact=true;
    for (unsigned row=0;edit && row<16;++row)
      intact &= !(edit->black[row] | edit->transparent[row]);
    if (intact) return colors[id];
  }
  return 0;
}
static uint16_t JungleWaterSubColor(const ActionRoomScene *s,
    const DioramaRoomOverride *terrain, const CastleMaterial *m, const DioramaPixelEdit *edit,
    const uint16_t palette[128], int cx, int cy, uint16_t main_color) {
  if (!main_color || (m->flags&(kVoxelWaterSurfaceMaterial|kVoxelOpaqueSurfaceMaterial))!=
      (kVoxelWaterSurfaceMaterial|kVoxelOpaqueSurfaceMaterial)) return 0;
  const DioramaVoxelCell *cell=terrain?DioramaVoxelEdits_Cell(&terrain->voxel,cx,cy):NULL;
  unsigned fields=DioramaVoxelCell_EffectiveFields(cell);
  if (cell && cell->faces==kDioramaVoxelFaceMask_All) fields &= ~kDioramaVoxelCell_Faces;
  if (fields) return 0;
  /* Jungle's main/sub cameras are1:1. BG2 repeats its finite native map inX,
   * while authored pixel masks follow those same map-cell coordinates. Keep
   * a painted/erased submerged term textured; unrelated shore edits, such as
   * shipped05/03's upper rows0..6, do not disable the added deck. */
  const int columns=(int)ActionRoomScene_TileWidth(s,2)/2;
  const int rows=(int)ActionRoomScene_TileHeight(s,2)/2;
  if (columns<=0 || cy<0 || cy>=rows) return 0;
  const DioramaStampLayerOverride *stamps=terrain?&terrain->stamp_layers[1]:NULL;
  if(stamps && stamps->set_bounds &&
      (cx<stamps->x0 || cy<stamps->y0 || cx>=stamps->x1 || cy>=stamps->y1))return 0;
  /* Explicit world-cell stamps precede the native repeated-page lookup. */
  const int mx=stamps && DioramaLayerOrder_StampAt(stamps,cx,cy)
      ? cx : ((cx%columns)+columns)%columns;
  uint16_t words[4]; uint8_t id;
  if (!JungleWaterWords(s,terrain,mx,cy,words,&id) ||
      !JungleWaterMainSurface(s,words)) return 0;
  const DioramaPixelEdit *main_edit=terrain?DioramaLayerOrder_PixelEditAt(terrain,1,mx,cy,id):NULL;
  for (unsigned y=13;main_edit && y<16;++y)
    if (main_edit->black[y] | main_edit->transparent[y]) return 0;
  for (unsigned q=2;q<4;++q)
    if (!SurfaceStaticWord(s,m->words[q]) || (m->words[q]&0x3c00)!=0x3400) return 0;
  for (unsigned y=13;y<16;++y) for (unsigned x=0;x<16;++x) {
    const unsigned q=2+x/8, bit=1u<<(15-x);
    if (m->pixels[q][(y&7)*8+(x&7)]!=12 || MaterialTransparent(m,edit,x,y) ||
        (edit && (edit->black[y]&bit))) return 0;
  }
  return (uint16_t)(0x8000 | palette[((m->words[2]>>10)&7)*16+12]);
}
