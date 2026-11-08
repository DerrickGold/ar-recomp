/* A finite Marahna03 atmospheric source ends inside the oblique viewport.
 * Publish only the original terminal ink, never a repeated architectural
 * tile. These optional runs do not modify native masks, depth or collision. */
static unsigned JungleTerminalFamily(uint64_t key) {
  static const uint64_t backdrop[]={
    UINT64_C(0x29a4069aac1574b1),UINT64_C(0xff2c412099fa36b1),
    UINT64_C(0xb43ebb2dcd599d8b),UINT64_C(0xaa089ce7b3e078ee),
    UINT64_C(0xb65fcd236e8aa1fc),UINT64_C(0xce84c0a2a19a73eb),
    UINT64_C(0x54adcc4939d00578)};
  for(unsigned i=0;i<sizeof(backdrop)/sizeof(*backdrop);++i)
    if(key==backdrop[i])return 1;
  if(key==UINT64_C(0xd4be8ba220cb80e6)||key==UINT64_C(0x9020d8381bf24e82))return 2;
  if(key==UINT64_C(0x2e352c34d3a283a5)||key==UINT64_C(0x05d1c8937938aec1)||
      key==UINT64_C(0xd873379c19c6368b))return 3;
  return 0;
}
static bool JungleTerminalCell(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const DioramaStampLayerOverride *stamps,const CastleMaterial native[256],
    int cx,int cy,unsigned edge,CastleMaterial *m,
    const uint16_t *tiles,size_t tile_pitch) {
  uint8_t id;
  if(!LookupMaterial(s,stamps,native,cx,cy,m,&id,tiles,tile_pitch))return false;
  unsigned family=JungleTerminalFamily(MaterialKey(m,0));
  if(!family || (edge==2&&family!=3) || (edge==3&&family<2))return false;
  const DioramaVoxelCell *a=terrain?DioramaVoxelEdits_Cell(&terrain->voxel,cx,cy):NULL;
  unsigned fields=DioramaVoxelCell_EffectiveFields(a);
  if(a&&a->faces==kDioramaVoxelFaceMask_All)fields&=~kDioramaVoxelCell_Faces;
  if(fields)return false;
  const DioramaPixelEdit *paint=terrain?DioramaLayerOrder_PixelEditAt(terrain,0,cx,cy,id):NULL;
  for(unsigned q=0;q<4;++q)
    if(m->kind[q]!=kVoxelRearOnly||!SurfaceStaticWord(s,m->words[q]))return false;
  /* An explicitly painted/erased cell owns its terminal source. It cannot be
   * repaired from a neighboring cell, including an authored opaque black. */
  for(unsigned y=0;y<16;++y)
    if((paint&&(paint->black[y]||paint->transparent[y]))||m->cutouts[y])return false;
  return true;
}
static bool JungleTerminalAppend(VoxelJungleTerminal *t,unsigned edge,int start,
    unsigned index,unsigned high,uint16_t color) {
  VoxelJungleTerminalRun *last=t->count?t->runs+t->count-1:NULL;
  if(last&&last->edge==edge&&last->end==start&&last->palette_index==index&&
      last->high_priority==high&&last->color==color){++last->end;return true;}
  if(t->count==kVoxelMaxJungleTerminalRuns)return false;
  t->runs[t->count++]=(VoxelJungleTerminalRun){.start=start,.end=start+1,
    .color=color,.palette_index=(uint8_t)index,.edge=(uint8_t)edge,.high_priority=high!=0};
  return true;
}
static void BuildJungleTerminal(ActionVoxelGeometry *c,const ActionRoomScene *s,
    const DioramaRoomOverride *terrain,const DioramaStampLayerOverride *stamps,
    const CastleMaterial native[256],const uint16_t palette[128],
    const uint16_t *tiles,size_t tile_pitch) {
  if(s->group!=5||s->map!=3||c->profile!=kActionVoxelProfile_MarahnaJungle||
      c->settings.depth<=0||!JungleWaterMainColor(s,terrain,palette))return;
  VoxelJungleTerminal *t=calloc(1,sizeof(*t));if(!t)return;
  t->left=c->origin_x;t->top=c->origin_y;t->right=t->left+c->width;t->bottom=t->top+c->height;
  /* Exactly three finite boundaries. Left background stops at architecture;
   * only the reviewed water sources may continue on that side. */
  for(unsigned edge=1;edge<=3;++edge) {
    const int start=edge==2?t->left:t->top,end=edge==2?t->right:t->bottom;
    for(int cell=start/16;cell<end/16;++cell) {
      const int cx=edge==2?cell:edge==1?t->right/16-1:t->left/16;
      const int cy=edge==2?t->bottom/16-1:cell;
      CastleMaterial m;if(!JungleTerminalCell(s,terrain,stamps,native,cx,cy,edge,&m,tiles,tile_pitch))continue;
      for(unsigned p=0;p<16;++p) {
        const unsigned x=edge==2?p:edge==1?15:0,y=edge==2?15:p,q=(y/8)*2+x/8;
        const unsigned ink=m.pixels[q][(y&7)*8+(x&7)],word=m.words[q];
        if(!ink)continue; /* RGB black with native opaque ink remains a color. */
        const unsigned index=((word>>10)&7)*16+ink;
        if(!JungleTerminalAppend(t,edge,cell*16+(int)p,index,(word>>13)&1,
            (uint16_t)(0x8000|palette[index]))) {free(t);return;}
      }
    }
  }
  if(t->count)c->jungle_terminal=t;else free(t);
}
