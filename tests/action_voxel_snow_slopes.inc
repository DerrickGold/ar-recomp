#include "action/action_voxel_internal.h"

/* These coordinates and IDs locate the independently reviewed native artwork
 * in the exported room, not production classification rules. All three ROM
 * regions have these nineteen collision9 cells and identical contact ink. */
static const uint8_t kSnowAct2SlopeCells[][3]={
  {4,21,0x80},{5,21,0x81},{6,22,0x76},{8,27,0x80},{9,27,0x81},
  {15,37,0x1e},{16,37,0x1f},{17,38,0x1e},{18,38,0x1f},
  {19,39,0x1e},{20,39,0x1f},{21,40,0x1e},{22,40,0x1f},
  {23,41,0x1e},{24,41,0x1f},{32,42,0x1e},{33,42,0x1f},
  {34,43,0x1e},{35,43,0x1f}
};
static unsigned SnowAct2Collision(const ActionRoomScene *s,int x,int y) {
  uint16_t word;uint8_t id;
  if(x<0 || y<0 || !ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id))return 0;
  unsigned result=0;
  for(unsigned q=0;q<4;++q)result|=!!(s->bg[0].metatiles[id*8+q*2]&2)<<q;
  return result;
}
/* Native $00:9084/$90A9 contact, including the priority of the upper paired
 * neighbor. This oracle is independent of source pixel silhouettes. */
static unsigned SnowAct2Contact(unsigned collision,unsigned left,unsigned right,unsigned x) {
  assert(collision==6 || collision==9);
  unsigned height=(collision==6?x:~x)&14;
  if((collision==6?left:right)==collision)height=height/2+8;
  else if((collision==6?right:left)==collision)height/=2;
  return 16-height;
}
static size_t SnowAct2At(const ActionVoxelGeometry *g,int x,int y) {
  assert(x>=g->origin_x && y>=g->origin_y && x<g->origin_x+g->width && y<g->origin_y+g->height);
  return (size_t)(y-g->origin_y)*g->width+x-g->origin_x;
}
/* Count actual generated TOP edges only after resolving their physical depth.
 * Solid metadata alone failed to expose the original half-cell shelves. */
static uint8_t *SnowAct2TopCoverage(const ActionVoxelGeometry *g) {
  uint8_t *tops=calloc((size_t)g->width,g->height);assert(tops);
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];if(e->side || e->far_rank!=2)continue;
    VoxelEdgeSpan span;
    if(!ActionVoxel_ResolveEdgeSpanAtY(g,e,kVoxelEdgeNative,0,ActionVoxel_Depth(g),e->y,&span))continue;
    if(fabsf(span.near_depth)>.000001f || fabsf(span.far_depth-ActionVoxel_Depth(g))>.000001f)continue;
    for(int x=e->x;x<e->x+e->length;++x)tops[SnowAct2At(g,x,e->y)]=1;
  }
  return tops;
}
static void SnowAct2SlopeRoles(const ActionVoxelGeometry *g,int cx,int cy,
    unsigned collision,unsigned left,unsigned right) {
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned m=g->front[SnowAct2At(g,cx*16+x,cy*16+y)];if(!m)continue;
    assert((m&kVoxelKindMask)==(y<SnowAct2Contact(collision,left,right,x)?kVoxelRearOnly:kVoxelSolid));
    assert(!!(m&kVoxelColliding)==!!(collision&(1u<<((y/8)*2+x/8))));
    assert(!(m&kVoxelHighPriority));
  }
}
static void SnowAct2SlopeFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];static DioramaRoomOverride terrain;
  assert(ActionVoxel_ProfileForRoom(6,6)==kActionVoxelProfile_None &&
      ActionVoxel_ProfileForRoom(6,7)==kActionVoxelProfile_None);
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/0605.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&snapshot));original=snapshot.scene;
    const ActionRoomScene *s=&snapshot.scene;assert(s->group==6 && s->map==5);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(6,5);
    ActionVoxelGeometry *base=ActionVoxel_Create(),*edited=ActionVoxel_Create();assert(base&&edited);
    assert(ActionVoxel_Build(base,s,NULL,settings));uint8_t *tops=SnowAct2TopCoverage(base);
    const ActionRoomSceneFrameState frame={.brightness=15};
    unsigned contacts=0,footer_contacts=0,rear_pixels=0,placed_slopes=0,other_slopes=0;
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2;++y)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1)/2;++x) {
        const unsigned collision=SnowAct2Collision(s,x,y);if(collision!=6 && collision!=9)continue;
        uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x*2,y*2,&word,&id));
        bool family=false,reviewed=false;
        for(unsigned i=0;i<sizeof(kSnowAct2SlopeCells)/sizeof(*kSnowAct2SlopeCells);++i) {
          family|=id==kSnowAct2SlopeCells[i][2];
          reviewed|=x==kSnowAct2SlopeCells[i][0] && y==kSnowAct2SlopeCells[i][1];
        }
        /* Other native ramp families already have their own contour policy;
         * this patch changes only the five reviewed snow/blue-bark families. */
        if(family){assert(reviewed);++placed_slopes;}else ++other_slopes;
      }
    assert(placed_slopes==19 && other_slopes==42);
    for(unsigned i=0;i<sizeof(kSnowAct2SlopeCells)/sizeof(*kSnowAct2SlopeCells);++i) {
      const int cx=kSnowAct2SlopeCells[i][0],cy=kSnowAct2SlopeCells[i][1];
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,cx*2,cy*2,&word,&id));
      assert(id==kSnowAct2SlopeCells[i][2]);
      const unsigned collision=SnowAct2Collision(s,cx,cy),left=SnowAct2Collision(s,cx-1,cy),
          right=SnowAct2Collision(s,cx+1,cy);assert(collision==9);
      SnowAct2SlopeRoles(base,cx,cy,collision,left,right);
      uint32_t native[256],rear[256],front[256];
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
        native[py*16+px]=ForestNativeColor(s,cx*16+px,cy*16+py,&frame);
      assert(ActionVoxel_Split(base,cx*16,cy*16,16,16,16,&frame,native,16,rear,front));
      for(unsigned px=0;px<16;++px) {
        const unsigned contact=SnowAct2Contact(collision,left,right,px);
        assert(FixturePixel(s,cx*16+px,cy*16+contact));
        const size_t at=SnowAct2At(base,cx*16+px,cy*16+contact);
        if(!tops[at])fprintf(stderr,"NorthwallAct2 terrain%u contact missing cell%d,%d pixel%u,%u role%u\n",
            region,cx,cy,px,contact,base->front[at]&kVoxelKindMask);
        assert((base->front[at]&kVoxelKindMask)==kVoxelSolid && tops[at]);
        ++contacts;footer_contacts+=contact==16;
        if(contact==16) {
          uint8_t footer;assert(ActionRoomScene_LookupTile(s,1,cx*2,(cy+1)*2,&word,&footer));
          assert(footer==0x5f || footer==0x8f || footer==0x27);
          assert(SnowAct2Collision(s,cx,cy+1)==15); /* Already native solid; no new foot rule. */
        }
        for(unsigned py=0;py<16;++py) {
          const bool backing=py<contact;
          if(backing)assert(!tops[SnowAct2At(base,cx*16+px,cy*16+py)]);
          assert(rear[py*16+px]==native[py*16+px]);
          assert(front[py*16+px]==(backing?0:native[py*16+px]));rear_pixels+=backing;
        }
      }
    }
    assert(contacts==304 && footer_contacts==20);
    remapped=*s;Renumber(&remapped);assert(ActionVoxel_Build(edited,&remapped,NULL,settings));SameGeometry(base,edited);
    assert(base->width==edited->width && base->height==edited->height &&
        !memcmp(base->front,edited->front,(size_t)base->width*base->height));
    /* Native source stamps retain the 1:2 paired grade and switch to the
     * authentic 1:1 grade when isolated, even outside original room bounds. */
    DioramaTileStamp stamps[2]={{.x=-2,.y=-2,.metatile=0x80},{.x=-1,.y=-2,.metatile=0x81}};
    for(unsigned i=0;i<2;++i) {
      int tx,ty;assert(FixtureCell(s,stamps[i].metatile,&tx,&ty));
      for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&stamps[i].words[q],NULL));
    }
    memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0]=(DioramaStampLayerOverride){
        .set_bounds=true,.x0=-2,.y0=-2,.x1=0,.y1=-1,.cells=stamps,.count=2};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    SnowAct2SlopeRoles(edited,-2,-2,9,0,9);SnowAct2SlopeRoles(edited,-1,-2,9,9,0);
    stamps[1].blank=true;assert(ActionVoxel_Build(edited,s,&terrain,settings));
    SnowAct2SlopeRoles(edited,-2,-2,9,0,0);
    /* An authored depth owns its cell without resetting its adjacent slope. */
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    DioramaVoxelCell *edit=&terrain.voxel.cells[0];
    *edit=(DioramaVoxelCell){.x=4,.y=21,.fields=kDioramaVoxelCell_Front|kDioramaVoxelCell_Back,
        .front=.03f,.back=.13f};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    const VoxelAuthoredCell *a=ActionVoxel_AuthoredCellAt(edited,4,21);assert(a&&a->surface);
    for(unsigned q=0;q<4;++q)assert(a->front[q]==.03f && a->back[q]==.13f && !a->priority[q]);
    SnowAct2SlopeRoles(edited,5,21,9,9,0);
    *edit=(DioramaVoxelCell){.x=4,.y=21,.fields=kDioramaVoxelCell_Faces,.faces=0};
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    a=ActionVoxel_AuthoredCellAt(edited,4,21);assert(a && !a->faces);
    uint8_t *hidden=SnowAct2TopCoverage(edited);
    for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px)
      assert(!hidden[SnowAct2At(edited,64+px,336+py)]);
    free(hidden);SnowAct2SlopeRoles(edited,5,21,9,9,0);
    edit->faces=kDioramaVoxelFaceMask_All;
    assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(base,edited);
    /* Actual source erasure stays empty at a contact; Auto must not heal it. */
    memset(&terrain,0,sizeof(terrain));terrain.pixel_layers[0].count=1;
    terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=4,.y=21};
    terrain.pixel_layers[0].edits[0].transparent[1]=0x8000;
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    assert(!edited->front[SnowAct2At(edited,64,337)]);
    hidden=SnowAct2TopCoverage(edited);assert(!hidden[SnowAct2At(edited,64,337)]);free(hidden);
    /* Explicit artwork remains owned artwork, rather than being re-carved
     * from the automatic native slope classification. */
    memset(&terrain,0,sizeof(terrain));terrain.voxel.cell_count=1;
    *edit=(DioramaVoxelCell){.x=4,.y=21,.fields=kDioramaVoxelCell_FrontArt};
    int dx,dy;assert(FixtureCell(s,0xca,&dx,&dy));
    edit->front_art=(DioramaVoxelArt){.bg=1,.size=16,.set=true};
    for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,dx+(q&1),dy+(q>>1),&edit->front_art.words[q],NULL));
    assert(ActionVoxel_Build(edited,s,&terrain,settings));
    a=ActionVoxel_AuthoredCellAt(edited,4,21);assert(a && a->surface && !a->native_front_art && a->pixels[0]);
    for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
      const unsigned ink=FixturePixel(s,dx*8+px,dy*8+py),q=(py/8)*2+px/8;
      const unsigned palette=(((edit->front_art.words[q]>>10)&7)*16+ink)*2;
      const uint16_t color=ink?0x8000u|s->palette[palette]|((uint16_t)s->palette[palette+1]<<8):0;
      assert(a->pixels[0][py*16+px]==color);
    }
    SnowAct2SlopeRoles(edited,5,21,9,9,0);
    memset(&terrain,0,sizeof(terrain));assert(ActionVoxel_Build(edited,s,&terrain,settings));SameGeometry(base,edited);
    assert(!memcmp(s,&original,sizeof(original)));
    printf("NorthwallAct2 slopes terrain%u: 19 native collision9 cells, %u full-depth contact caps (%u existing solid-footer endpoints), %u rear-art pixels; no raised shelves, remap/stamp/erase/depth/art/reset checks passed\n",
        region,contacts,footer_contacts,rear_pixels);
    free(tops);ActionVoxel_Release(base);ActionVoxel_Release(edited);
  }
}
