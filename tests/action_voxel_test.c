/* ROM-free lifecycle/validation checks; optional exported-room fixtures extend
 * the same executable across regional art and live native gate publications. */
#include "support/test_assert.h"
#include "action/action_voxel.h"
#include "action/action_scene_snapshot.h"
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void SameGeometry(const ActionVoxelGeometry *a, const ActionVoxelGeometry *b) {
  unsigned na, nb;
  const VoxelEdge *ea=ActionVoxel_Edges(a,&na), *eb=ActionVoxel_Edges(b,&nb);
  assert(na==nb);
  for(unsigned i=0;i<na;++i) {
    VoxelEdge a_edge=ea[i],b_edge=eb[i];
    /* Owned image indices may deduplicate differently under a palette fade
     * or when an authored image precedes the same automatic material. */
    a_edge.top_owned_art=b_edge.top_owned_art=0;
    assert(!memcmp(&a_edge,&b_edge,sizeof(a_edge)));
  }
  if(ActionVoxel_Profile(a)!=kActionVoxelProfile_FillmoreForest &&
     ActionVoxel_Profile(a)!=kActionVoxelProfile_BloodpoolMarsh)
    for(unsigned i=0;i<na;++i)assert(!ea[i].rear_fold_top && !ea[i].rear_fold_bottom &&
        (ea[i].surface_texture || (!ea[i].top_sample_size && !ea[i].top_sample_x && !ea[i].top_sample_y)));
  const VoxelGate *ga=ActionVoxel_Gates(a,&na), *gb=ActionVoxel_Gates(b,&nb);
  assert(na==nb && !memcmp(ga,gb,na*sizeof(*ga)));
  const VoxelRearTrim *ra=ActionVoxel_RearTrims(a,&na), *rb=ActionVoxel_RearTrims(b,&nb);
  assert(na==nb && !memcmp(ra,rb,na*sizeof(*ra)));
  const VoxelPatch *pa=ActionVoxel_Patches(a,&na), *pb=ActionVoxel_Patches(b,&nb);
  assert(na==nb && !memcmp(pa,pb,na*sizeof(*pa)));
  const VoxelWaterSurface *wa=ActionVoxel_WaterSurfaces(a,&na), *wb=ActionVoxel_WaterSurfaces(b,&nb);
  assert(na==nb);
  for(unsigned i=0;i<na;++i)assert(wa[i].x==wb[i].x && wa[i].y==wb[i].y &&
      wa[i].width==wb[i].width && wa[i].high_priority==wb[i].high_priority &&
      wa[i].background==wb[i].background && wa[i].sample_y==wb[i].sample_y &&
      wa[i].sample_height==wb[i].sample_height && wa[i].opaque==wb[i].opaque);
  const VoxelWaterfall *fa=ActionVoxel_Waterfalls(a,&na),*fb=ActionVoxel_Waterfalls(b,&nb);
  assert(na==nb);
  for(unsigned i=0;i<na;++i)assert(fa[i].x==fb[i].x && fa[i].y==fb[i].y && fa[i].width==fb[i].width &&
      fa[i].height==fb[i].height && fa[i].high_priority==fb[i].high_priority);
  assert(ActionVoxel_HasRearRelief(a)==ActionVoxel_HasRearRelief(b));
  assert(ActionVoxel_HasCutouts(a)==ActionVoxel_HasCutouts(b));
  if(ActionVoxel_HasCutouts(a))
    for(int y=-1;y<226;++y)for(int x=-1;x<258;++x) {
      uint8_t ma[8]={0},mb[8]={0};
      assert(ActionVoxel_CutoutRows(a,x,y,ma)==ActionVoxel_CutoutRows(b,x,y,mb));
      assert(!memcmp(ma,mb,8));
    }
  if(ActionVoxel_HasRearRelief(a))
    for(int y=-16;y<1312;y+=13)for(int x=-16;x<2080;x+=19)
      assert(ActionVoxel_RearRelief(a,x,y)==ActionVoxel_RearRelief(b,x,y));
}
static void Renumber(ActionRoomScene *s) {
  uint8_t definitions[sizeof(s->bg[0].metatiles)], characters[sizeof(s->characters)];
  unsigned remap[256];
  const unsigned stride=ActionRoomScene_HasCharacterAnimation(s)
      ? ActionRoomScene_CharacterAnimationStride(s)/32 : 0;
  const unsigned animated=stride*ActionRoomScene_CharacterAnimationPhaseCount(s);
  /* Native animation targets are bank origins, not arbitrary CHR addresses.
   * Apply the same permutation within every phase, then rotate static art
   * separately. A global +37 shift would leave stale animation metadata and
   * falsely advertise animated sand as a safe static surface donor. */
  for(unsigned i=0;i<256;++i)
    remap[i]=i<animated ? i/stride*stride+(i+37)%stride
        : animated+(i-animated+37)%(256-animated);
  memcpy(characters,s->characters,sizeof(characters));
  for(unsigned bank=0;bank<2;++bank)for(unsigned i=0;i<256;++i)
    memcpy(s->characters+(bank*256+remap[i])*32,characters+(bank*256+i)*32,32);
  for(unsigned layer=0;layer<2;++layer) {
    memcpy(definitions,s->bg[layer].metatiles,sizeof(definitions));
    for(unsigned i=0;i<256;++i) {
      const unsigned id=(i+73)&255;
      memcpy(s->bg[layer].metatiles+id*8,definitions+i*8,8);
      for(unsigned q=0;q<4;++q)
        s->bg[layer].metatiles[id*8+q*2+1]=remap[definitions[i*8+q*2+1]];
    }
    for(size_t i=0;i<s->bg[layer].map_size;++i)s->bg[layer].map[i]=(s->bg[layer].map[i]+73)&255;
  }
}
static void Lifecycle(void) {
  /* The walking plane stays fixed; only the back half of a folded deck rises. */
  const float contact=.05f,rear=.18f;
  for(unsigned i=0;i<=5;++i)assert(ActionVoxel_RearFoldOffset(7,i*.01f,contact,rear)==0);
  assert(fabsf(ActionVoxel_RearFoldOffset(7,(contact+rear)*.5f,contact,rear)-3.5f)<.00001f);
  assert(ActionVoxel_RearFoldOffset(7,rear,contact,rear)==7);
  assert(ActionVoxel_RearFoldOffset(7,.25f,contact,rear)==7);
  assert(ActionVoxel_RearFoldOffset(0,.12f,contact,rear)==0);
  assert(ActionVoxel_RearFoldOffset(7,.12f,rear,contact)==0);
  assert(ActionVoxel_RearFoldOffset(7,.12f,contact,contact)==0);
  assert(ActionVoxel_RearFoldOffset(7,NAN,contact,rear)==0);
  assert(ActionVoxel_RearFoldOffset(7,.12f,-INFINITY,rear)==0);
  assert(ActionVoxel_RearFoldOffset(7,.12f,contact,INFINITY)==0);
  assert(ActionVoxel_ValidBounds(-320,0,4224,768));
  assert(ActionVoxel_ValidBounds(0,0,1024,1792));
  assert(!ActionVoxel_ValidBounds(0,0,8192,8192));
  assert(!ActionVoxel_ValidBounds(-32769,0,0,16));
  assert(!ActionVoxel_ValidBounds(0,0,32768,16));
  assert(!ActionVoxel_ValidBounds(0,0,0,16));
  ActionVoxelGeometry *a=ActionVoxel_Create(); assert(a);
  assert(ActionVoxel_Revision(a) && !ActionVoxel_Revision(NULL));
  static ActionRoomScene scene;
  assert(ActionVoxel_Build(a,&scene,NULL,ActionVoxel_DefaultSettings()));
  assert(!ActionVoxel_Active(a,2,2));
  assert(ActionVoxel_Depth(a)==.24f);
  const ActionVoxelGeometry *retained=ActionVoxel_Retain(a);
  const uint64_t old_revision=ActionVoxel_Revision(retained);
  assert(!ActionVoxel_Build(a,&scene,NULL,(ActionVoxelSettings){.3f,.1f}));
  assert(ActionVoxel_Revision(retained)==old_revision);
  ActionVoxel_Release(a);
  assert(ActionVoxel_Depth(retained)==.24f);
  assert(ActionVoxel_Build((ActionVoxelGeometry *)retained,&scene,NULL,(ActionVoxelSettings){.3f,.1f}));
  assert(ActionVoxel_Revision(retained)!=old_revision);
  assert(!ActionVoxel_Build((ActionVoxelGeometry *)retained,&scene,NULL,(ActionVoxelSettings){NAN,.1f}));
  assert(ActionVoxel_Depth(retained)==.3f);
  ActionVoxel_Release(retained);
  ActionVoxel_Release(NULL);
}
static unsigned Fixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  unsigned checked=0;
  for (unsigned room=2;room<=8;++room) for (unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/02%02u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    ActionRoomScene *source=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(), *b=ActionVoxel_Create();assert(a&&b);
    assert(ActionVoxel_Build(a,source,NULL,ActionVoxel_DefaultSettings()));
    assert(ActionVoxel_Active(a,2,room) && !ActionVoxel_Active(a,2,room+1));
    remapped=*source;Renumber(&remapped);
    assert(ActionVoxel_Build(b,&remapped,NULL,ActionVoxel_DefaultSettings()));SameGeometry(a,b);
    const unsigned width=ActionRoomScene_TileWidth(source,1), height=ActionRoomScene_TileHeight(source,1);
    uint16_t *tiles=malloc((size_t)width*height*sizeof(*tiles));assert(tiles);
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x)
      assert(ActionRoomScene_LookupTile(source,1,x,y,&tiles[y*width+x],NULL));
    assert(ActionVoxel_BuildWithTiles(b,source,NULL,ActionVoxel_DefaultSettings(),tiles,width));SameGeometry(a,b);
    unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(a,&count);assert(count);
    const float actor=ActionVoxel_PlayRecess(a)-.01f, depth=ActionVoxel_Depth(a);
    for (unsigned i=0;i<count;++i) {
      const VoxelEdge *e=&edges[i];VoxelEdgeSpan rear,front;
      if (e->high_priority && e->colliding && !e->side && e->far_rank==2) {
        assert(ActionVoxel_ResolveEdgeSpan(a,e,kVoxelEdgeHighDeckBehind,0,depth,&rear));
        assert(ActionVoxel_ResolveEdgeSpan(a,e,kVoxelEdgeNative,0,depth,&front));
        assert(front.far_depth==actor && rear.near_depth==actor && rear.far_depth==depth);
      }
      if (e->high_priority && !e->colliding) assert(e->far_rank==1);
    }
    /* Padded input/output pitches must not touch their guards; split is const
     * and must remain stable after another context publishes changed doors. */
    enum {w=37,h=19,ip=41,op=43};
    uint32_t low[ip*h],rear[op*h],front[op*h],saved[op*h];
    for(unsigned i=0;i<ip*h;++i)low[i]=0xff030507;
    for(unsigned i=0;i<op*h;++i)rear[i]=front[i]=0xdeadbeef;
    const ActionRoomSceneFrameState colors={.brightness=11,.cgadsub=0x81,.fixed_color=0x421};
    assert(ActionVoxel_Split(a,24,room==2?464:192,w,h,op,&colors,low,ip,rear,front));
    memcpy(saved,front,sizeof(saved));
    for(unsigned y=0;y<h;++y)for(unsigned x=w;x<op;++x)
      assert(rear[y*op+x]==0xdeadbeef && front[y*op+x]==0xdeadbeef);
    const ActionVoxelGeometry *retained=ActionVoxel_Retain(a);
    assert(!ActionVoxel_Build(a,source,NULL,ActionVoxel_DefaultSettings()));
    unsigned gates;const VoxelGate *gate=ActionVoxel_Gates(a,&gates);
    if (gates) {
      unsigned blank=256;
      for(unsigned ch=0;ch<256;++ch){unsigned bits=0;for(unsigned byte=0;byte<32;++byte)bits|=source->characters[ch*32+byte];if(!bits){blank=ch;break;}}
      assert(blank<256);
      for(unsigned i=0;i<gates;++i)for(int y=gate[i].y/8;y<(gate[i].y+gate[i].height)/8;++y)
        tiles[y*width+gate[i].x/8]=(tiles[y*width+gate[i].x/8]&0xfc00u)|blank;
      assert(ActionVoxel_BuildWithTiles(b,source,NULL,ActionVoxel_DefaultSettings(),tiles,width));
      unsigned after;ActionVoxel_Gates(b,&after);assert(after==0);
      ActionVoxel_Gates(retained,&after);assert(after==gates);
    }
    ActionVoxel_Release(a);
    assert(ActionVoxel_Split(retained,24,room==2?464:192,w,h,op,&colors,low,ip,rear,front));
    assert(!memcmp(saved,front,sizeof(saved)));
    ActionVoxel_Release(retained);ActionVoxel_Release(b);free(tiles);++checked;
  }
  return checked;
}
static unsigned FixturePixel(const ActionRoomScene *s,int x,int y);
static bool EdgeInPatch(const VoxelEdge *e,const VoxelPatch *p) {
  return e->x>=p->x && e->x+((e->side&1)?1:e->length)<=p->x+p->width &&
      e->y>=p->y && e->y+((e->side&1)?e->length:1)<=p->y+p->height;
}
static bool StumpOuterTop(const ActionRoomScene *s,const VoxelEdge *e,const VoxelPatch *p) {
  if(e->side)return false;
  for(int x=e->x;x<e->x+e->length;++x)
    for(int y=p->y;y<e->y;++y)if(FixturePixel(s,x,y))return false;
  return true;
}
static void ForestProfiles(const ActionRoomScene *source, const ActionVoxelGeometry *a,
                           const ActionVoxelGeometry *renumbered, ActionVoxelSettings settings) {
  unsigned count, types=0, branches=0, stump_caps=0,stump_sides=0,stump_steps=0;
  unsigned recessed=0,thin_sides=0;
  assert(ActionVoxel_FrontRecess(a,INT_MIN,INT_MAX)==0);
  assert(ActionVoxel_FrontRecess(a,INT_MAX,INT_MIN)==0);
  for(unsigned y=0;y<ActionRoomScene_TileHeight(source,1);++y)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(source,1);++x) {
      const float depth=ActionVoxel_FrontRecess(a,x,y);
      assert(depth==ActionVoxel_FrontRecess(renumbered,x,y));
      if(depth) { assert(fabsf(depth-(settings.play_recess-.01f))<.00001f);++recessed; }
    }
  assert(recessed>0);
  const VoxelPatch *patch=ActionVoxel_Patches(a,&count);
  unsigned edge_count;const VoxelEdge *edges=ActionVoxel_Edges(a,&edge_count);
  assert(ActionVoxel_PlayRecess(a)<ActionVoxel_BranchDepth(a));
  assert(ActionVoxel_BranchDepth(a)<ActionVoxel_Depth(a)*.5f);
  for(unsigned i=0;i<count;++i) {
    const VoxelPatch *p=&patch[i];types|=1u<<p->kind;
    if(p->kind==kVoxelPatch_RearGrass) {
      /* A reviewed standalone tuft has no dirt/root rows or collision.
       * Its native silhouette remains entirely on the rear texture. */
      assert(p->width%16==0 && p->height==16 && !p->high_priority);
      uint32_t low[256],rear[256],front[256];
      const ActionRoomSceneFrameState frame={.brightness=15};
      for(unsigned j=0;j<256;++j)low[j]=0xff128a23;
      for(int x=p->x;x<p->x+p->width;x+=16) {
        assert(ActionVoxel_Split(a,x,p->y,16,16,16,&frame,low,16,rear,front));
        for(unsigned j=0;j<256;++j)assert(rear[j]==low[j] && !front[j]);
      }
    }
    if(p->kind!=kVoxelPatch_SideProfile)continue;
    /* The first eight pixels of the native cap metatile are transparent. */
    assert(p->width==32 && p->height==56);
    unsigned char contour[32*56]={0};
    for(unsigned j=0;j<edge_count;++j) {
      const VoxelEdge *e=&edges[j];
      if(!EdgeInPatch(e,p))continue;
      assert(!e->trunk_curve && !e->rear_fold_top && !e->rear_fold_bottom);
      if(e->side&1)++stump_sides;
      else if(StumpOuterTop(source,e,p))++stump_caps;
      else ++stump_steps;
      for(int n=0;n<e->length;++n) {
        const int x=e->x-p->x+((e->side&1)?0:n),y=e->y-p->y+((e->side&1)?n:0);
        assert(!(contour[y*32+x]&(1u<<e->side)));
        contour[y*32+x]|=1u<<e->side;
      }
    }
    /* Every exposed native bark pixel has its own extrusion, including the
     * internal notches that broad side panels previously concealed. Ignore
     * outside neighbours here: the stump may join native ground there. */
    const int dx[]={0,1,0,-1},dy[]={-1,0,1,0};
    for(int y=0;y<56;++y)for(int x=0;x<32;++x)for(unsigned side=0;side<4;++side) {
      const int xx=x+dx[side],yy=y+dy[side];
      if(xx<0 || xx>=32 || yy<0 || yy>=56)continue;
      const bool exposed=FixturePixel(source,p->x+x,p->y+y) &&
          !FixturePixel(source,p->x+xx,p->y+yy);
      assert(!!(contour[y*32+x]&(1u<<side))==exposed);
    }
  }
  assert(types==(7|(1u<<kVoxelPatch_RearGrass)) && stump_caps && stump_sides && stump_steps);
  for(unsigned i=0;i<edge_count;++i)if(edges[i].far_rank==3) {
    VoxelEdgeSpan span;
    assert(ActionVoxel_ResolveEdgeSpan(a,&edges[i],kVoxelEdgeNative,0,ActionVoxel_Depth(a),&span));
    assert(span.far_depth==ActionVoxel_BranchBackDepth(a));++branches;
  }
  assert(branches>0);
  for(unsigned i=0;i<edge_count;++i)if(edges[i].far_rank==1 && !edges[i].near_rank) {
    VoxelEdgeSpan span;
    assert(ActionVoxel_ResolveEdgeSpan(a,&edges[i],kVoxelEdgeNative,0,ActionVoxel_Depth(a),&span));
    assert(fabsf(span.near_depth-ActionVoxel_SceneryRecess(a))<.00001f);
    assert(fabsf((span.far_depth-span.near_depth)-ActionVoxel_TrimDepth(a))<.00001f);
    ++thin_sides;
  }
  assert(thin_sides>0);

  /* Reviewed thorn beside a two-row grass border in every regional layout.
   * The old repair filled only the bottom row, leaving a rectangular notch.
   * Capture only the destination: donor art must not require an onscreen tile. */
  enum {x=1344,y=640,w=32,h=32,pitch=37};
  uint32_t rear[pitch*h],front[pitch*h],expected[pitch*h];
  const ActionRoomSceneFrameState frame={.brightness=15};
  for(unsigned i=0;i<pitch*h;++i)rear[i]=front[i]=expected[i]=0xdeadbeef;
  assert(ActionVoxel_Split(a,x,y-16,w,h,pitch,&frame,NULL,0,rear,front));
  unsigned tall=0,base=0;
  for(unsigned yy=0;yy<h;++yy)for(unsigned xx=0;xx<pitch;++xx) {
    if(xx>=w) { assert(rear[yy*pitch+xx]==0xdeadbeef && front[yy*pitch+xx]==0xdeadbeef);continue; }
    if(xx<16 && rear[yy*pitch+xx]) { if(yy<16)++tall;else ++base; }
    assert(front[yy*pitch+xx]==0);
  }
  if (!(tall>16 && base>128)) fprintf(stderr,"thorn backing: tall=%u, base=%u\n",tall,base);
  assert(tall>16 && base>128);
  memcpy(expected,rear,sizeof(expected));
  assert(ActionVoxel_Split(renumbered,x,y-16,w,h,pitch,&frame,NULL,0,rear,front));
  assert(!memcmp(rear,expected,sizeof(rear)));
  const ActionRoomSceneFrameState dark={.brightness=0,.cgadsub=0x81,.fixed_color=0x421};
  assert(ActionVoxel_Split(a,x,y-16,w,h,pitch,&dark,NULL,0,rear,front));
  for(unsigned yy=0;yy<h;++yy)for(unsigned xx=0;xx<w;++xx)
    assert(rear[yy*pitch+xx]==(expected[yy*pitch+xx]?0xff000000u:0));

  /* Published repairs respect authored pixel masks, including their donor.
   * Rebuild the same context to exercise replacement of both sparse storage
   * and branch depth tags, while the original stays immutable. */
  static DioramaRoomOverride terrain;
  memset(&terrain,0,sizeof(terrain));
  DioramaPixelLayerOverride *layer=&terrain.pixel_layers[0];
  layer->count=1;
  layer->edits[0]=(DioramaPixelEdit){.by_cell=true,.x=x/16-1,.y=y/16};
  for(unsigned row=0;row<16;++row)layer->edits[0].black[row]=0xffff;
  ActionVoxelGeometry *edited=ActionVoxel_Create();assert(edited);
  assert(ActionVoxel_Build(edited,source,&terrain,settings));
  assert(ActionVoxel_Split(edited,x,y,16,16,pitch,&frame,NULL,0,rear,front));
  for(unsigned yy=0;yy<16;++yy)for(unsigned xx=0;xx<16;++xx)assert(rear[yy*pitch+xx]==0xff000000u);
  memset(&terrain,0,sizeof(terrain));layer->count=1;
  layer->edits[0]=(DioramaPixelEdit){.by_cell=true,.x=x/16,.y=y/16};
  for(unsigned row=0;row<16;++row)layer->edits[0].transparent[row]=0xffff;
  assert(ActionVoxel_Build(edited,source,&terrain,settings));
  assert(ActionVoxel_Split(edited,x,y,16,16,pitch,&frame,NULL,0,rear,front));
  for(unsigned yy=0;yy<16;++yy)for(unsigned xx=0;xx<16;++xx)assert(rear[yy*pitch+xx]==0 && front[yy*pitch+xx]==0);
  ActionVoxel_Release(edited);
}
static void ForestStumpCapSamples(const ActionRoomScene *source,const ActionRoomScene *remapped,
                                 const ActionVoxelGeometry *a,const ActionVoxelGeometry *renumbered) {
  /* This reviewed four-pixel square is inside the native top grain, without
   * the elliptical rim/bark outline. The two native moss pixels stay intact.
   * Fixture indices describe the artwork, never runtime material identity. */
  static const unsigned char grain[16]={14,15,15,14,15,14,15,3,3,11,15,14,14,14,11,15};
  VoxelPatch target={0};
  for(unsigned variant=0;variant<2;++variant) {
    const ActionVoxelGeometry *geometry=variant?renumbered:a;
    const ActionRoomScene *scene=variant?remapped:source;
    unsigned np,ne,caps=0,stumps=0;
    const VoxelPatch *patches=ActionVoxel_Patches(geometry,&np);
    const VoxelEdge *edges=ActionVoxel_Edges(geometry,&ne);
    for(unsigned i=0;i<np;++i) {
      const VoxelPatch *p=&patches[i];
      if(p->kind!=kVoxelPatch_SideProfile)continue;
      assert(p->width==32 && p->height==56);++stumps;target=*p;
      unsigned local_caps=0;
      for(unsigned j=0;j<ne;++j) {
        const VoxelEdge *e=&edges[j];
        if(!EdgeInPatch(e,p))continue;
        assert(!e->trunk_curve && !e->rear_fold_top && !e->rear_fold_bottom);
        if(!StumpOuterTop(scene,e,p)) {assert(e->surface_texture || !e->top_sample_size);continue;}
        assert(e->top_sample_x==p->x+9 && e->top_sample_y==p->y+7 && e->top_sample_size==4);
        assert(e->far_rank==2 && e->high_priority==p->high_priority);
        uint16_t word;uint8_t material;
        assert(ActionRoomScene_LookupTile(scene,1,e->x/8,e->y/8,&word,&material));
        const unsigned q=((e->y&8)>>2)|((e->x&8)>>3);
        assert(e->colliding==!!(scene->bg[0].metatiles[material*8+q*2]&2));
        /* Source metadata must not change the edge's physical depth or
         * priority partition, including the behind-actor deck pass. */
        VoxelEdge ordinary=*e;ordinary.top_sample_x=ordinary.top_sample_y=ordinary.top_sample_size=0;
        for(unsigned pass=0;pass<=kVoxelEdgeHighDeckBehind;++pass) {
          VoxelEdgeSpan sampled={0},original={0};
          assert(ActionVoxel_ResolveEdgeSpan(geometry,e,pass,0,ActionVoxel_Depth(geometry),&sampled)==
                 ActionVoxel_ResolveEdgeSpan(geometry,&ordinary,pass,0,ActionVoxel_Depth(geometry),&original));
          assert(sampled.near_depth==original.near_depth && sampled.far_depth==original.far_depth);
        }
        assert(ActionVoxel_EdgeRecess(geometry,e)==ActionVoxel_EdgeRecess(geometry,&ordinary));
        ++local_caps;++caps;
      }
      assert(local_caps);
      for(int y=0;y<4;++y)for(int x=0;x<4;++x) {
        uint16_t word;
        assert(FixturePixel(scene,p->x+9+x,p->y+7+y)==grain[y*4+x]);
        assert(ActionRoomScene_LookupTile(scene,1,(p->x+9+x)/8,(p->y+7+y)/8,&word,NULL));
        assert(!!(word&0x2000)==p->high_priority);
      }
      /* The original native front remains byte-for-byte artwork, including
       * transparent silhouette pixels; only generated top UVs will differ. */
      uint32_t native[32*56],rear[32*56],front[32*56];
      const ActionRoomSceneFrameState frame={.brightness=15};
      for(int y=0;y<56;++y)for(int x=0;x<32;++x) {
        const unsigned pixel=FixturePixel(scene,p->x+x,p->y+y);
        native[y*32+x]=pixel?0xff000000u|pixel:0;
      }
      assert(ActionVoxel_Split(geometry,p->x,p->y,32,56,32,&frame,native,32,rear,front));
      for(unsigned j=0;j<32*56;++j)assert(front[j]==native[j] && rear[j]==native[j]);
    }
    for(unsigned j=0;j<ne;++j)if(edges[j].top_sample_size && !edges[j].surface_texture) {
      unsigned owners=0;
      for(unsigned i=0;i<np;++i)owners+=patches[i].kind==kVoxelPatch_SideProfile &&
          EdgeInPatch(&edges[j],&patches[i]);
      assert(!edges[j].side && owners==1);
    }
    assert(stumps && caps);
    printf("forest stump samples%s: %u profiles, %u outer top edges\n",variant?" renumbered":"",stumps,caps);
  }
  /* Authored transparency inside the sample, or a shortened profile, must
   * fall back without borrowing a nearby stump's art or changing its sides. */
  for(unsigned variant=0;variant<2;++variant)for(unsigned cut=0;cut<2;++cut) {
    static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
    DioramaPixelLayerOverride *layer=&terrain.pixel_layers[0];
    if(!cut) {
      const int x=target.x+9,y=target.y+7;
      layer->count=1;layer->edits[0]=(DioramaPixelEdit){.by_cell=true,.x=x/16,.y=y/16};
      layer->edits[0].transparent[y&15]=1u<<(15-(x&15));
    } else {
      layer->count=2;
      for(unsigned i=0;i<2;++i) {
        layer->edits[i]=(DioramaPixelEdit){.by_cell=true,.x=target.x/16+i,.y=(target.y+48)/16};
        for(unsigned y=8;y<16;++y)layer->edits[i].transparent[y]=0xffff;
      }
    }
    ActionVoxelGeometry *edited=ActionVoxel_Create();assert(edited);
    assert(ActionVoxel_Build(edited,variant?remapped:source,&terrain,ActionVoxel_SettingsForRoom(1,1)));
    unsigned np,ne,local_caps=0,other_samples=0,profiles=0;
    const VoxelPatch *patches=ActionVoxel_Patches(edited,&np);
    const VoxelEdge *edges=ActionVoxel_Edges(edited,&ne);
    for(unsigned i=0;i<np;++i)if(patches[i].kind==kVoxelPatch_SideProfile &&
        patches[i].x==target.x && patches[i].y==target.y) {
      assert(patches[i].width==32 && patches[i].height==(cut?48:56));++profiles;
    }
    for(unsigned i=0;i<ne;++i) {
      const VoxelEdge *e=&edges[i];
      if(EdgeInPatch(e,&target)) {
        assert(e->surface_texture || (!e->top_sample_size && !e->top_sample_x && !e->top_sample_y));
        local_caps+=e->side==0;
      } else other_samples+=e->top_sample_size!=0;
    }
    assert(profiles==1 && local_caps && other_samples);ActionVoxel_Release(edited);
  }
  puts("forest stump samples: transparent source and shortened profile keep ordinary cap sampling");
}
static uint32_t ForestNativeColor(const ActionRoomScene *s,int x,int y,const ActionRoomSceneFrameState *frame) {
  uint16_t word;assert(ActionRoomScene_LookupTile(s,1,x/8,y/8,&word,NULL));
  const unsigned pixel=FixturePixel(s,x,y);if(!pixel)return 0;
  const unsigned index=(((word>>10)&7)*16+pixel)*2;
  const unsigned color=s->palette[index]|((unsigned)s->palette[index+1]<<8);
  const unsigned subtract=frame->fixed_color && !frame->cgwsel && (frame->cgadsub&0xc1)==0x81?frame->fixed_color:0;
  uint32_t rgb=0xff000000u;
  for(unsigned ch=0;ch<3;++ch) {
    int value=((color>>(ch*5))&31)-((subtract>>(ch*5))&31);if(value<0)value=0;
    rgb|=(uint32_t)((value*8+(value>>2))*frame->brightness/15)<<(16-ch*8);
  }
  return rgb;
}
static void ForestJoinColumns(const ActionVoxelGeometry *g,int x,int y,int joins[16]) {
  unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(g,&count);
  memset(joins,0,16*sizeof(*joins));
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->side || !e->flat_rear_join || e->y/16!=y/16+1)continue;
    for(int j=0;j<16;++j)if(x+j>=e->x && x+j<e->x+e->length)joins[j]=e->y;
  }
}
static void ForestRearGrassTransitions(const ActionRoomScene *source,
                             const ActionVoxelGeometry *original,
                             const ActionVoxelGeometry *renumbered) {
  /* Rising and descending grass wedges belong to the same rear border strip.
   * A rear copy alone hides the gap but leaves a spurious foreground wedge.
   * IDs below locate reviewed fixture art only; renumbered geometry must give
   * identical output. Feed the real native opacity, including its open slope. */
  enum { width=16, height=16, pitch=21 };
  const ActionRoomSceneFrameState frame={.brightness=15};
  uint32_t low[pitch*height],rear[pitch*height],front[pitch*height];
  for(unsigned q=0;q<4;++q) {
    const uint8_t *rise=source->bg[0].metatiles+0x41*8+q*2;
    const uint8_t *fall=source->bg[0].metatiles+0x39*8+(q^1)*2;
    /* These two border slopes are the same native artwork mirrored. */
    assert(((rise[0]<<8)|rise[1])==(((fall[0]<<8)|fall[1])^0x4000));
  }
  unsigned corners=0,controls=0,opaque=0,transparent=0;
  for(unsigned ty=0;ty<ActionRoomScene_TileHeight(source,1);ty+=2)
    for(unsigned tx=0;tx<ActionRoomScene_TileWidth(source,1);tx+=2) {
      uint16_t word;uint8_t material;
      assert(ActionRoomScene_LookupTile(source,1,tx,ty,&word,&material));
      const bool corner=material==0x41;
      if(!corner && material!=0x08 && material!=0x39)continue;
      for(unsigned q=0;q<4;++q) {
        const uint8_t *d=source->bg[0].metatiles+material*8+q*2;
        assert(!(d[0]&2)); /* The source decoration adds no collision. */
      }
      for(unsigned y=0;y<height;++y)for(unsigned x=0;x<pitch;++x) {
        const unsigned pixel=x<width?FixturePixel(source,tx*8+x,ty*8+y):0;
        low[y*pitch+x]=pixel?0xff000000u|(pixel<<16)|(y<<8)|x:0;
        if(corner && x<width) { opaque+=pixel!=0;transparent+=pixel==0; }
      }
      for(unsigned variant=0;variant<2;++variant) {
        const ActionVoxelGeometry *geometry=variant?renumbered:original;
        for(unsigned i=0;i<pitch*height;++i)rear[i]=front[i]=0xdeadbeef;
        assert(ActionVoxel_Split(geometry,tx*8,ty*8,width,height,pitch,&frame,low,pitch,rear,front));
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<pitch;++x) {
          const unsigned at=y*pitch+x;
          if(x>=width) { assert(rear[at]==0xdeadbeef && front[at]==0xdeadbeef);continue; }
          assert(rear[at]==low[at]);
          assert(front[at]==0);
        }
      }
      corners+=corner;controls+=!corner;
    }
  assert(corners && controls && opaque && transparent);
  printf("forest rear grass transitions: %u corners, %u rear-border controls, %u opaque/%u transparent pixels\n",
         corners,controls,opaque,transparent);
}
static bool ForestFoldJoin(const VoxelEdge *edges,unsigned count,int x,int y) {
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];
    if(e->side==0 && e->rear_fold_top==7 && e->y==y && x>=e->x && x<=e->x+e->length)return true;
  }
  return false;
}
static void ForestGroundFolds(const ActionRoomScene *source,const ActionRoomScene *remapped,
                              const ActionVoxelGeometry *a,const ActionVoxelGeometry *renumbered) {
  for(unsigned variant=0;variant<2;++variant) {
    const ActionVoxelGeometry *geometry=variant?renumbered:a;
    unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(geometry,&count);
    unsigned tops=0,micro=0,sides=0,branches=0,stumps=0,hazards=0,high=0;
    unsigned grass=0,dirt=0;
    assert(ActionVoxel_PlayRecess(geometry)==.06f && ActionVoxel_Depth(geometry)==.18f);
    for(unsigned i=0;i<count;++i) {
      const VoxelEdge *e=&edges[i];uint16_t word;uint8_t material;
      assert(ActionRoomScene_LookupTile(source,1,e->x/8,e->y/8,&word,&material));
      const unsigned q=((e->y&8)>>2)|((e->x&8)>>3);
      const uint8_t *definition=source->bg[0].metatiles+material*8+q*2;
      /* Collision and native layer priority remain the source's own bits. */
      assert(e->colliding==((definition[0]&2)!=0));
      assert(e->high_priority==((word&0x2000)!=0));
      assert((!e->rear_fold_top || e->rear_fold_top==7) &&
             (!e->rear_fold_bottom || e->rear_fold_bottom==7));
      const bool folded=e->rear_fold_top || e->rear_fold_bottom;
      assert(!!e->flat_rear_join==folded);
      if(folded)assert(e->far_rank==2 && e->colliding && !e->high_priority && e->side!=2);
      if(e->far_rank==3) {assert(!folded);++branches;}
      /* These IDs identify reviewed fixture specimens, not runtime policy. */
      const bool stump=material==0x06 || material==0x07 || material==0x0e || material==0x0f ||
          material==0x1e || material==0x1f || material==0x26 || material==0x27;
      const bool hazard=material==0xc2 || material==0xc3;
      if(stump) {assert(!folded);++stumps;}
      if(hazard) {assert(!folded);++hazards;}
      if(!folded)continue;
      high+=e->high_priority!=0;
      const float contact=ActionVoxel_PlayRecess(geometry)-.01f,depth=ActionVoxel_Depth(geometry);
      assert(ActionVoxel_RearFoldOffset(e->rear_fold_top,contact,contact,depth)==0);
      assert(ActionVoxel_RearFoldOffset(e->rear_fold_bottom,contact,contact,depth)==0);
      if(e->side==0) {
        assert(e->rear_fold_top==7 && !e->rear_fold_bottom);++tops;
        micro+=(e->y&15)!=0;grass+=e->textured_top!=0;dirt+=e->textured_top==0;
      } else {
        const int x=e->x+(e->side==1);
        const int fold_y=e->fold_length?e->fold_origin_y:e->y;
        const int fold_length=e->fold_length?e->fold_length:e->length;
        if(e->rear_fold_top)assert(ForestFoldJoin(edges,count,x,fold_y));
        if(e->rear_fold_bottom)assert(ForestFoldJoin(edges,count,x,fold_y+fold_length));
        ++sides;
      }
    }
    unsigned plateau=0;
    for(unsigned i=0;i<count;++i) {
      const VoxelEdge *e=&edges[i];
      if(e->side==0 && e->far_rank==2 && e->colliding && e->y==640 && e->x>=912 && e->x<1152) {
        assert(e->rear_fold_top==7);++plateau;
      }
    }
    assert(tops && sides && branches && stumps && hazards && grass && dirt && plateau && !high);
    printf("forest ground fold%s: %u tops (%u micro), %u side joins, %u grass/%u dirt, %u high; unchanged %u branch/%u stump/%u hazard edges\n",
           variant?" renumbered":"",tops,micro,sides,grass,dirt,high,branches,stumps,hazards);
  }
  /* Native forest banks happen to have aligned opaque contours. An authored
   * two-row corner cutout creates the small stepped contour that exposed the
   * preview's cell-support ambiguity, without changing its semantic artwork. */
  static DioramaRoomOverride edit;
  memset(&edit,0,sizeof(edit));edit.pixel_layers[0].count=1;
  edit.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=1136/16,.y=640/16};
  edit.pixel_layers[0].edits[0].transparent[0]=0xffff;
  edit.pixel_layers[0].edits[0].transparent[1]=0xff00;
  for(unsigned variant=0;variant<2;++variant) {
    ActionVoxelGeometry *cut=ActionVoxel_Create();assert(cut);
    assert(ActionVoxel_Build(cut,variant?remapped:source,&edit,ActionVoxel_SettingsForRoom(1,1)));
    unsigned count;const VoxelEdge *edges=ActionVoxel_Edges(cut,&count);unsigned micro_tops=0,micro_side=0;
    for(unsigned i=0;i<count;++i) {
      const VoxelEdge *e=&edges[i];
      if(e->side==0 && e->x>=1136 && e->x<1152 && (e->y==641 || e->y==642)) {
        assert(e->rear_fold_top==7 && e->colliding && !e->high_priority);++micro_tops;
      }
      if((e->side&1) && e->x+(e->side==1)==1144 && e->y==641 && e->length==1) {
        assert(e->rear_fold_top==7 && e->rear_fold_bottom==7);++micro_side;
      }
    }
    assert(micro_tops==2 && micro_side==1);
    ActionVoxel_Release(cut);
  }
  /* A high-priority authored bank still receives its native high geometry,
   * but must not acquire a fold textured from the native-low rear source. */
  static ActionRoomScene high_source;high_source=*source;
  for(unsigned q=0;q<4;++q)high_source.bg[0].metatiles[0x10*8+q*2]|=0x20;
  ActionVoxelGeometry *high_geometry=ActionVoxel_Create();assert(high_geometry);
  assert(ActionVoxel_Build(high_geometry,&high_source,NULL,ActionVoxel_SettingsForRoom(1,1)));
  unsigned count,high_banks=0;const VoxelEdge *edges=ActionVoxel_Edges(high_geometry,&count);
  for(unsigned i=0;i<count;++i) {
    const VoxelEdge *e=&edges[i];uint16_t word;uint8_t material;
    assert(ActionRoomScene_LookupTile(source,1,e->x/8,e->y/8,&word,&material));
    if(e->high_priority)assert(!e->rear_fold_top && !e->rear_fold_bottom);
    if(material==0x10 && e->colliding) {assert(e->high_priority);++high_banks;}
  }
  assert(high_banks);ActionVoxel_Release(high_geometry);
  puts("forest ground fold: authored micro-corner and native-high exclusion passed");
}
#include "action_voxel_forest_join.inc"
#include "action_voxel_forest_trunks.inc"
static unsigned ForestFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/0101-%u.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t bytes=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,bytes,&fixture));
    const ActionRoomScene *source=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,1);
    assert(ActionVoxel_Build(a,source,NULL,settings));
    assert(ActionVoxel_Active(a,1,1) && !ActionVoxel_Active(a,2,1));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_FillmoreForest);
    unsigned edges;ActionVoxel_Edges(a,&edges);
    assert(edges>0 && edges<=kVoxelMaxEdges);
    remapped=*source;Renumber(&remapped);
    assert(ActionVoxel_Build(b,&remapped,NULL,settings));SameGeometry(a,b);
    /* Opaque black native terrain remains a solid volume. The rear copy of
     * a forest must never turn a transparent source pixel into masonry. */
    enum {width=32,height=16};uint32_t low[width*height],rear[width*height],front[width*height];
    memset(low,0,sizeof(low));
    const ActionRoomSceneFrameState frame={.brightness=15};
    assert(ActionVoxel_Split(a,128,560,width,height,width,&frame,low,width,rear,front));
    for(unsigned i=0;i<width*height;++i)assert(rear[i]==0 && front[i]==0);
    /* Fixture grass is two non-colliding 16px rows above a solid bank.
     * Its tall art belongs only to the rear; the bank remains the front
     * volume that supplies the short-grass deck. Renumbering above verifies
     * that the production material decision does not depend on these IDs. */
    bool grass_checked=false;
    for(unsigned y=0;!grass_checked && y+4<ActionRoomScene_TileHeight(source,1);y+=2)
      for(unsigned x=0;!grass_checked && x<ActionRoomScene_TileWidth(source,1);x+=2) {
        uint16_t word;uint8_t grass,below,bank;
        assert(ActionRoomScene_LookupTile(source,1,x,y,&word,&grass));
        assert(ActionRoomScene_LookupTile(source,1,x,y+2,&word,&below));
        assert(ActionRoomScene_LookupTile(source,1,x,y+4,&word,&bank));
        if(grass!=1 || below!=8 || bank!=0x10)continue;
        for(unsigned i=0;i<width*height;++i)low[i]=0xff11aa33;
        for(unsigned row=0;row<2;++row) {
          assert(ActionVoxel_Split(a,x*8,(y+row*2)*8,16,16,width,&frame,low,width,rear,front));
          for(unsigned yy=0;yy<16;++yy)for(unsigned xx=0;xx<16;++xx) {
            assert(rear[yy*width+xx]==low[yy*width+xx] && front[yy*width+xx]==0);
          }
        }
        assert(ActionVoxel_Split(a,x*8,(y+4)*8,16,16,width,&frame,low,width,rear,front));
        unsigned front_pixels=0;
        for(unsigned yy=0;yy<16;++yy)for(unsigned xx=0;xx<16;++xx)
          front_pixels+=front[yy*width+xx]!=0;
        assert(front_pixels>128);grass_checked=true;
      }
    assert(grass_checked);
    /* The same shrub crown occurs on the ground and over a solid thorn cap.
     * Ground crowns follow their rear border base without moving that cap.
     * Exercise both original and renumbered material/character definitions. */
    unsigned border_cases = 0;
    for(unsigned y=0;y+2<ActionRoomScene_TileHeight(source,1);y+=2)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(source,1);x+=2) {
        uint16_t word;uint8_t crown,base;
        assert(ActionRoomScene_LookupTile(source,1,x,y,&word,&crown));
        assert(ActionRoomScene_LookupTile(source,1,x,y+2,&word,&base));
        const bool ground=(crown==2 && base==0x0a) || crown==0xb0 || crown==0xb8 || crown==0x39;
        const bool cap=crown==0xc1, mixed=crown==0x30 || crown==0x31;
        if(!ground && !cap && !mixed)continue;
        for(unsigned variant=0;variant<2;++variant) {
          assert(ActionVoxel_Split(variant?b:a,x*8,y*8,16,16,width,&frame,low,width,rear,front));
          unsigned front_pixels=0;
          for(unsigned yy=0;yy<16;++yy)for(unsigned xx=0;xx<16;++xx) {
            front_pixels+=front[yy*width+xx]!=0;
            if(ground || mixed) {
              const bool curve=mixed && front[yy*width+xx] &&
                  ActionVoxel_TrunkCurveForCell(variant?b:a,x+xx/8,y+yy/8);
              /* A native shrub donor may fill the removed bark footprint;
               * it must not retain the duplicated synthetic bark source. */
              if(curve)assert(rear[yy*width+xx]!=low[yy*width+xx]);
              else assert(rear[yy*width+xx]==low[yy*width+xx]);
            }
          }
          if(mixed) {
            /* Native roots share a tile with the border bush. Bark stays in
             * front while foliage joins the rear. Owned curved roots have
             * one bark surface rather than a coincident rear bark copy.
             * These fixture points are independently reviewed wood/leaves. */
            const unsigned wood=crown==0x30?15:0, leaves=crown==0x30?0:10;
            assert(front[wood]==low[wood] && front[leaves]==0);
            for(unsigned xx=0;xx<16;++xx)assert(front[15*width+xx]==0);
            assert(front_pixels>32 && front_pixels<128);
          } else assert(ground?front_pixels==0:front_pixels>64);
        }
        border_cases |= crown==0xb0?4:crown==0xb8?8:crown==0x39?16:
                        crown==0x30?32:crown==0x31?64:ground?1:2;
      }
    assert(border_cases==127);
    ForestProfiles(source,a,b,settings);
    ForestStumpCapSamples(source,&remapped,a,b);
    ForestRearGrassTransitions(source,a,b);
    ForestGroundFolds(source,&remapped,a,b);
    ForestNativeRearRows(source,a);
    ForestTrunkCurves(source,a,b);
    printf("forest terrain %u: %u merged contours\n",region,edges);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 3;
}
static void CaveCutouts(const ActionRoomScene *source, const ActionVoxelGeometry *original) {
  /* Native spike tip at (320,368). Black matte clears; its coloured center and
   * enclosed shadows on other rock art remain opaque, even in a black fade. */
  uint8_t left[8]={0},right[8]={0};
  assert(ActionVoxel_HasCutouts(original));
  assert(ActionVoxel_CutoutRows(original,40,47,left));
  assert(ActionVoxel_CutoutRows(original,41,47,right));
  const uint8_t expected_left[8]={0xfe,0xfe,0xfe,0xfe,0xff,0xff,0xff,0xff};
  const uint8_t expected_right[8]={0x3f,0x3f,0x3f,0x3f,0x7f,0x7f,0x7f,0x7f};
  assert(!memcmp(left,expected_left,8) && !memcmp(right,expected_right,8));
  memset(left,0xa5,8);
  assert(!ActionVoxel_CutoutRows(original,INT_MIN,INT_MAX,left));
  for(unsigned i=0;i<8;++i)assert(left[i]==0xa5);
  assert(!ActionVoxel_CutoutRows(NULL,40,47,left));
  assert(!ActionVoxel_CutoutRows(original,40,47,NULL));
  bool enclosed_shadow=false;
  for(unsigned y=0;y<ActionRoomScene_TileHeight(source,1) && !enclosed_shadow;y+=2)
    for(unsigned x=0;x<ActionRoomScene_TileWidth(source,1) && !enclosed_shadow;x+=2) {
      uint16_t word;uint8_t id;
      assert(ActionRoomScene_LookupTile(source,1,x,y,&word,&id));
      if(id!=0x8d)continue;
      uint8_t rows[8]={0};assert(ActionVoxel_CutoutRows(original,x,y+1,rows));
      assert(!(rows[6]&0x20)); /* Native black pixel (2,14) enclosed by rock. */
      enclosed_shadow=true;
    }
  assert(enclosed_shadow);

  static ActionRoomScene variant;
  variant=*source;
  memset(variant.palette,0,sizeof(variant.palette));
  ActionVoxelGeometry *edited=ActionVoxel_Create();assert(edited);
  const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,2);
  assert(ActionVoxel_Build(edited,&variant,NULL,settings));SameGeometry(original,edited);
  /* Authored mirrors move both the art and mask. Shift the stamp outside the
   * original map to cover signed world coordinates and custom bounds too. */
  static DioramaRoomOverride terrain;
  memset(&terrain,0,sizeof(terrain));
  uint16_t words[4];uint8_t metatile;
  for(unsigned q=0;q<4;++q)
    assert(ActionRoomScene_LookupTile(source,1,40+(q&1),46+(q>>1),&words[q],&metatile));
  DioramaTileStamp stamp={.x=-1,.y=0,.metatile=metatile};
  terrain.stamp_layers[0]=(DioramaStampLayerOverride){.cells=&stamp,.count=1,
      .set_bounds=true,.x0=-1,.y0=0,.x1=128,.y1=80};
  for(unsigned flip=0;flip<4;++flip) {
    for(unsigned q=0;q<4;++q)stamp.words[q]=words[q^flip]^((flip&1)?0x4000:0)^((flip&2)?0x8000:0);
    assert(ActionVoxel_Build(edited,source,&terrain,settings));
    for(unsigned q=0;q<4;++q) {
      uint8_t actual[8]={0},expected[8]={0};
      assert(ActionVoxel_CutoutRows(edited,-2+(q&1),q>>1,actual));
      assert(ActionVoxel_CutoutRows(original,40+((q^flip)&1),46+((q^flip)>>1),expected));
      for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
        assert(!!(actual[y]&(128>>x))==!!(expected[(flip&2)?7-y:y]&(128>>((flip&1)?7-x:x))));
    }
  }
  /* Painting one matte pixel black wins over the automatic rule. Clearing an
   * art pixel explicitly still works through the existing capture edit path. */
  memset(&terrain,0,sizeof(terrain));
  terrain.pixel_layers[0].count=1;
  DioramaPixelEdit *paint=&terrain.pixel_layers[0].edits[0];
  *paint=(DioramaPixelEdit){.by_cell=true,.x=20,.y=23};
  paint->black[15]=0x8000;
  assert(ActionVoxel_Build(edited,source,&terrain,settings));
  memset(left,0,8);assert(ActionVoxel_CutoutRows(edited,40,47,left));assert(left[7]==0x7f);
  uint32_t native[256],rear[256],front[256];
  for(unsigned i=0;i<256;++i)native[i]=0xff000000;
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(ActionVoxel_Split(edited,320,368,16,16,16,&frame,native,16,rear,front));
  assert(front[15*16]==0xff000000);
  assert(ActionVoxel_Split(original,320,368,16,16,16,&frame,native,16,rear,front));
  assert(front[15*16]==0);
  memset(left,0,8);assert(ActionVoxel_CutoutRows(original,40,47,left));assert(left[7]==0xff);
  /* A different picture at the same numeric ID is not authorized by the rule. */
  variant=*source;
  variant.characters[(words[0]&1023)*32]^=1;
  assert(ActionVoxel_Build(edited,&variant,NULL,settings));
  memset(left,0,8);assert(!ActionVoxel_CutoutRows(edited,40,47,left));
  assert(ActionVoxel_Build(edited,source,NULL,settings));
  const ActionVoxelGeometry *retained=ActionVoxel_Retain(edited);
  assert(!ActionVoxel_Build(edited,&variant,NULL,settings));
  ActionVoxel_Release(edited);
  memset(left,0,8);assert(ActionVoxel_CutoutRows(retained,40,47,left));assert(left[7]==0xff);
  ActionVoxel_Release(retained);
}
static unsigned CaveFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/0102-%u.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t bytes=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,bytes,&fixture));
    ActionRoomScene *source=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,2);
    assert(ActionVoxel_Build(a,source,NULL,settings) && ActionVoxel_Active(a,1,2));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_FillmoreCave);
    CaveCutouts(source,a);
    unsigned spike_count;const VoxelPatch *spikes=ActionVoxel_Patches(a,&spike_count);
    assert(spike_count>0);
    for(unsigned i=0;i<spike_count;++i)
      assert(spikes[i].kind==kVoxelPatch_Spikes && spikes[i].width%16==0 && spikes[i].height%16==0);
    unsigned n;const VoxelWaterSurface *water=ActionVoxel_WaterSurfaces(a,&n);assert(n==3);
    const int expected[][3]={{1088,304,224},{896,688,208},{0,896,880}};
    for(unsigned i=0;i<n;++i)assert(water[i].x==expected[i][0] && water[i].y==expected[i][1] &&
        water[i].width==expected[i][2] && water[i].high_priority);
    const VoxelWaterfall *falls=ActionVoxel_Waterfalls(a,&n);assert(n==4);
    const int fall_positions[][2]={{720,896},{992,688},{1104,304},{1248,304}};
    for(unsigned i=0;i<n;++i)assert(falls[i].x==fall_positions[i][0] && falls[i].y==0 &&
        falls[i].width==32 && falls[i].height==fall_positions[i][1] && falls[i].high_priority);
    for(unsigned phase=0;phase<4;++phase) {
      variant=*source;
      assert(ActionRoomScene_BuildCharacters(source,0,phase,variant.characters,sizeof(variant.characters)));
      assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    }
    variant=*source;Renumber(&variant);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    /* Removing a native water cell splits the compiled surface at the edit;
     * the previously retained geometry and the other pools stay untouched. */
    static DioramaRoomOverride terrain;
    memset(&terrain,0,sizeof(terrain));
    assert(ActionVoxel_HasRearRelief(a));
    float peak=0;int peak_x=0,peak_y=0;
    for(int y=8;y<1280;y+=16)for(int x=8;x<2048;x+=16) {
      const float relief=ActionVoxel_RearRelief(a,x,y);
      assert(isfinite(relief) && relief>=0 && relief<=.075f);
      if(relief>peak){peak=relief;peak_x=x;peak_y=y;}
      /* A native tile boundary must not introduce a depth discontinuity. */
      assert(fabsf(ActionVoxel_RearRelief(a,x-8.01f,y)-
                   ActionVoxel_RearRelief(a,x-7.99f,y))<.001f);
    }
    assert(peak>.04f && !ActionVoxel_RearRelief(a,-100,100) &&
           !ActionVoxel_RearRelief(a,NAN,100));
    terrain.pixel_layers[0].count=1;
    DioramaPixelEdit *rock_edit=&terrain.pixel_layers[0].edits[0];
    *rock_edit=(DioramaPixelEdit){.by_cell=true,.x=peak_x/16,.y=peak_y/16};
    for(unsigned y=0;y<16;++y)rock_edit->transparent[y]=0xffff;
    assert(ActionVoxel_Build(b,source,&terrain,settings));
    assert(ActionVoxel_RearRelief(b,peak_x,peak_y)<peak);
    assert(ActionVoxel_RearRelief(a,peak_x,peak_y)==peak);
    memset(&terrain,0,sizeof(terrain));
    terrain.pixel_layers[1].count=1;
    DioramaPixelEdit *fall_edit=&terrain.pixel_layers[1].edits[0];
    *fall_edit=(DioramaPixelEdit){.by_cell=true,.x=45,.y=10};
    for(unsigned y=0;y<16;++y)fall_edit->transparent[y]=0xffff;
    assert(ActionVoxel_Build(b,source,&terrain,settings));
    ActionVoxel_Waterfalls(b,&n);assert(n==6); /* One column splits while its neighbour stays whole. */
    ActionVoxel_Waterfalls(a,&n);assert(n==4);
    memset(&terrain,0,sizeof(terrain));
    terrain.pixel_layers[1].count=1;
    DioramaPixelEdit *edit=&terrain.pixel_layers[1].edits[0];
    *edit=(DioramaPixelEdit){.by_cell=true,.x=70,.y=19};
    for(unsigned y=0;y<16;++y)edit->transparent[y]=0xffff;
    assert(ActionVoxel_Build(b,source,&terrain,settings));ActionVoxel_WaterSurfaces(b,&n);assert(n==4);
    ActionVoxel_WaterSurfaces(a,&n);assert(n==3);
    memset(&terrain,0,sizeof(terrain));
    DioramaTileStamp blank={.x=70,.y=19,.blank=true};
    terrain.stamp_layers[1].cells=&blank;terrain.stamp_layers[1].count=1;
    assert(ActionVoxel_Build(b,source,&terrain,settings));ActionVoxel_WaterSurfaces(b,&n);assert(n==4);
    terrain.stamp_layers[1].cells=NULL;terrain.stamp_layers[1].count=0;
    unsigned edges;ActionVoxel_Edges(a,&edges);
    printf("cave terrain %u: %u merged contours, 3 continuous pools, 4 animation phases\n",region,edges);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 3;
}
static unsigned TempleFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned room=3;room<=4;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/01%02u-%u.arscene",directory,room,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));
    const ActionRoomScene *source=&fixture.scene;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(1,room);
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    assert(ActionVoxel_Build(a,source,NULL,settings) && ActionVoxel_Active(a,1,room));
    assert(ActionVoxel_Profile(a)==(room==3?kActionVoxelProfile_FillmoreTemple:kActionVoxelProfile_FillmoreTower));
    assert(ActionVoxel_HasCutouts(a));
    variant=*source;Renumber(&variant);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    unsigned width=ActionRoomScene_TileWidth(source,1),height=ActionRoomScene_TileHeight(source,1),recessed=0;
    uint16_t *tiles=malloc((size_t)width*height*sizeof(*tiles));assert(tiles);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
      assert(ActionRoomScene_LookupTile(source,1,x,y,&tiles[y*width+x],NULL));
      assert(ActionVoxel_FrontRecess(a,x,y)==ActionVoxel_FrontRecess(b,x,y));
      recessed+=ActionVoxel_FrontRecess(a,x,y)>0;
    }
    assert(recessed);
    assert(ActionVoxel_BuildWithTiles(b,source,NULL,settings,tiles,width));SameGeometry(a,b);free(tiles);
    variant=*source;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);

    /* A shallow column occupies the rear of a full-depth deck. The cap
     * must close its front gap, and native high priority splits that cap
     * at the actor plane instead of clipping over the actor's feet. */
    unsigned count,caps=0,relief=0;const VoxelEdge *edges=ActionVoxel_Edges(a,&count);
    for(unsigned i=0;i<count;++i) {
      const VoxelEdge *edge=&edges[i];VoxelEdgeSpan span;
      if(edge->near_rank==1 && edge->far_rank==2) {
        const float column_front=ActionVoxel_SceneryRecess(a)-
            (edge->neighbor_raised_scenery?settings.depth*.125f:0);
        assert(ActionVoxel_ResolveEdgeSpan(a,edge,kVoxelEdgeNative,0,settings.depth,&span));
        assert(span.near_depth==0 && fabsf(span.far_depth-column_front)<.00001f);++caps;
        if(!edge->side && edge->colliding) {
          VoxelEdge high=*edge;high.high_priority=true;
          assert(ActionVoxel_ResolveEdgeSpan(a,&high,kVoxelEdgeNative,0,settings.depth,&span));
          const float join=span.far_depth;
          assert(ActionVoxel_ResolveEdgeSpan(a,&high,kVoxelEdgeHighDeckBehind,0,settings.depth,&span));
          assert(join==span.near_depth && fabsf(span.far_depth-column_front)<.00001f);
        }
      }
      if(edge->far_rank==1 && !edge->near_rank) {
        assert(ActionVoxel_ResolveEdgeSpan(a,edge,kVoxelEdgeNative,0,settings.depth,&span));
        const float column_front=ActionVoxel_SceneryRecess(a)-
            (edge->raised_scenery?settings.depth*.125f:0);
        assert(fabsf(span.near_depth-column_front)<.00001f);
        assert(fabsf(span.far_depth-settings.depth)<.00001f);++relief;
      }
    }
    assert(caps && relief);
    unsigned patches;const VoxelPatch *patch=ActionVoxel_Patches(a,&patches);
    if(room==3) { unsigned spikes=0;for(unsigned i=0;i<patches;++i)spikes+=patch[i].kind==kVoxelPatch_Spikes;assert(spikes); }
    ActionVoxel_WaterSurfaces(a,&patches);assert(!patches);
    ActionVoxel_Waterfalls(a,&patches);assert(!patches);
    printf("temple room %u terrain %u: %u contours, %u column joins\n",room,region,count,caps);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 6;
}
static void MarshRockJoins(const ActionVoxelGeometry *geometry) {
  /* The native entrance grass tiles also contain an opaque rock underside.
   * Sending their entire cell to the rear opened rectangular holes below the
   * rock. Reviewed nonblack vegetation belongs behind the path; shadow stays
   * at the shallow rock plane. This must survive remapping and black fades. */
  const unsigned columns[]={0,6,8}, shadow_pixels[]={150,166,128};
  const ActionRoomSceneFrameState frame={.brightness=15};
  uint32_t low[256],rear[256],front[256];
  for(unsigned i=0;i<256;++i)low[i]=0xff030507;
  for(unsigned cell=0;cell<3;++cell) {
    assert(ActionVoxel_Split(geometry,columns[cell]*16,27*16,16,16,16,&frame,low,16,rear,front));
    unsigned shadow=0;
    for(unsigned i=0;i<256;++i) {
      assert((rear[i]==low[i] && !front[i]) || (front[i]==low[i] && !rear[i]));
      if(i<128)assert(front[i]==low[i]);
      shadow+=front[i]!=0;
    }
    assert(shadow==shadow_pixels[cell]);
    for(unsigned q=0;q<4;++q) {
      uint8_t cutouts[8]={0};
      assert(!ActionVoxel_CutoutRows(geometry,columns[cell]*2+(q&1),54+(q>>1),cutouts));
    }
  }
}
static unsigned MarshFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/0201-%u.arscene",directory,region);
    FILE *file=fopen(path,"rb");assert(file);
    const size_t size=fread(encoded,1,sizeof(encoded),file);assert(!ferror(file));fclose(file);
    assert(ActionSceneSnapshot_Decode(encoded,size,&fixture));
    const ActionRoomScene *source=&fixture.scene;
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(2,1);
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    assert(ActionVoxel_Build(a,source,NULL,settings) && ActionVoxel_Active(a,2,1));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_BloodpoolMarsh && ActionVoxel_HasCutouts(a));
    assert(fabsf(ActionVoxel_SceneryRecess(a)-(settings.depth-ActionVoxel_TrimDepth(a)))<.00001f &&
           ActionVoxel_TrimDepth(a)==settings.depth*.25f);
    /* Freestanding islands never receive an inferred distant wall. The
     * only added pixels are the reviewed ground underneath two native
     * opaque rock shadows; every other empty input stays transparent. */
    {
      const ActionRoomSceneFrameState f={.brightness=15};
      uint32_t empty[32*32]={0},rear[32*32],front[32*32];
      const int width=ActionRoomScene_TileWidth(source,1)*8;
      const int height=ActionRoomScene_TileHeight(source,1)*8;
      unsigned ground_backing=0;
      for(int y=-32;y<height+32;y+=32)for(int x=-32;x<width+32;x+=32) {
        memset(rear,0xff,sizeof(rear));memset(front,0xff,sizeof(front));
        assert(ActionVoxel_Split(a,x,y,32,32,32,&f,empty,32,rear,front));
        for(unsigned p=0;p<32*32;++p) {
          const int wx=x+(int)(p%32),wy=y+(int)(p/32);
          uint32_t expected=0;
          if(wy>=440 && wy<448 && ((wx>=112 && wx<128) || (wx>=144 && wx<160)) &&
              FixturePixel(source,wx,wy)==14) {
            const int donor_x=160+(wx&15);
            uint16_t word;assert(ActionRoomScene_LookupTile(source,1,donor_x/8,wy/8,&word,NULL));
            const unsigned ink=FixturePixel(source,donor_x,wy);assert(ink);
            const unsigned index=((word>>10)&7)*16+ink;
            const unsigned color=source->palette[index*2]|source->palette[index*2+1]<<8;
            expected=0xff000000u;
            for(unsigned channel=0;channel<3;++channel) {
              const unsigned v=(color>>(channel*5))&31;
              expected|=((v<<3)|(v>>2))<<(16-channel*8);
            }
            ++ground_backing;
          }
          assert(rear[p]==expected && !front[p]);
        }
      }
      assert(ground_backing==64);
    }
    MarshRockJoins(a);
    variant=*source;Renumber(&variant);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);MarshRockJoins(b);
    variant=*source;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);MarshRockJoins(b);
    const unsigned width=ActionRoomScene_TileWidth(source,1),height=ActionRoomScene_TileHeight(source,1);
    uint16_t *tiles=malloc((size_t)width*height*sizeof(*tiles));assert(tiles);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x)
      assert(ActionRoomScene_LookupTile(source,1,x,y,&tiles[y*width+x],NULL));
    assert(ActionVoxel_BuildWithTiles(b,source,NULL,settings,tiles,width));SameGeometry(a,b);free(tiles);
    unsigned count,full=0,shallow=0;const VoxelEdge *edges=ActionVoxel_Edges(a,&count);
    for(unsigned i=0;i<count;++i) {
      const VoxelEdge *edge=&edges[i];VoxelEdgeSpan span;
      assert(ActionVoxel_ResolveEdgeSpan(a,edge,kVoxelEdgeNative,0,settings.depth,&span));
      assert(span.near_depth>=0 && span.far_depth<=settings.depth);
      full+=edge->far_rank==2;shallow+=edge->far_rank==1;
      /* Water's native collision flags never turn its drawn surface into a
       * solid slab. Every terrain contour is above the lake, or island soil. */
      if(edge->y>=480 && edge->x>=192 && edge->x<864)assert(edge->far_rank!=2);
    }
    assert(full && shallow);
    unsigned pools;const VoxelWaterSurface *water=ActionVoxel_WaterSurfaces(a,&pools);assert(pools);
    for(unsigned i=0;i<pools;++i) {
      assert(water[i].background==0 && water[i].high_priority && water[i].y==480);
      assert(water[i].sample_y>=8 && water[i].sample_y+water[i].sample_height<=16);
    }
    /* Identical black artwork has two distinct structural contexts. Keep a
     * scenic rock interior with its shallow outline; fill soil all the way
     * through the island. Neither may be erased as an exterior matte. */
    const ActionRoomSceneFrameState frame={.brightness=15};
    uint32_t low[256],rear[256],front[256];
    for(unsigned i=0;i<256;++i)low[i]=0xff030507;
    assert(ActionVoxel_Split(a,24*16,28*16,16,16,16,&frame,low,16,rear,front));
    for(unsigned i=0;i<256;++i)assert(!rear[i] && front[i]==low[i]);
    assert(ActionVoxel_Split(a,1*16,29*16,16,16,16,&frame,low,16,rear,front));
    for(unsigned i=0;i<256;++i)assert(rear[i]==low[i] && front[i]==low[i]);
    uint8_t cutout[8]={0};assert(!ActionVoxel_CutoutRows(a,24*2,28*2,cutout));
    /* A terrain edit breaks the connected lake at that cell, while the
     * published original and native map remain immutable. */
    static DioramaRoomOverride terrain;memset(&terrain,0,sizeof(terrain));
    DioramaTileStamp blank={.x=13,.y=30,.blank=true};
    terrain.stamp_layers[0].cells=&blank;terrain.stamp_layers[0].count=1;
    assert(ActionVoxel_Build(b,source,&terrain,settings));
    unsigned edited;ActionVoxel_WaterSurfaces(b,&edited);assert(edited==pools+1);
    ActionVoxel_WaterSurfaces(a,&edited);assert(edited==pools);
    uint8_t id;uint16_t words[4];
    for(unsigned q=0;q<4;++q)
      assert(ActionRoomScene_LookupTile(source,1,26+(q&1),60+(q>>1),&words[q],&id));
    blank.blank=false;blank.metatile=id;
    /* Horizontal artwork mirrors keep the surface. A vertical mirror puts
     * the water band above the shoreline and must not sample the empty slot. */
    for(unsigned q=0;q<4;++q)blank.words[q]=words[q^1]^0x4000;
    assert(ActionVoxel_Build(b,source,&terrain,settings));
    ActionVoxel_WaterSurfaces(b,&edited);assert(edited==pools);
    for(unsigned q=0;q<4;++q)blank.words[q]=words[q^2]^0x8000;
    assert(ActionVoxel_Build(b,source,&terrain,settings));
    ActionVoxel_WaterSurfaces(b,&edited);assert(edited==pools+1);

    /* Mirroring a mixed rock/grass cell must reflect its colour membership,
     * including the different palette-index sets in its lower quadrants. */
    for(unsigned q=0;q<4;++q)
      assert(ActionRoomScene_LookupTile(source,1,12+(q&1),54+(q>>1),&words[q],&id));
    blank.x=6;blank.y=27;blank.metatile=id;
    for(unsigned q=0;q<4;++q)blank.words[q]=words[q^1]^0x4000;
    assert(ActionVoxel_Build(b,source,&terrain,settings));
    uint32_t mirrored_rear[256],mirrored_front[256];
    assert(ActionVoxel_Split(a,96,432,16,16,16,&frame,low,16,rear,front));
    assert(ActionVoxel_Split(b,96,432,16,16,16,&frame,low,16,mirrored_rear,mirrored_front));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      assert(mirrored_rear[y*16+x]==rear[y*16+15-x]);
      assert(mirrored_front[y*16+x]==front[y*16+15-x]);
    }

    printf("marsh terrain %u: %u contours, %u water runs, intact rock/island infill\n",region,count,pools);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 3;
}
/* IDs here locate reviewed art in exported fixtures only. Renumbering below
 * proves the runtime classification does not depend on those addresses. */
static void DesertMaterial(const ActionVoxelGeometry *g,const ActionRoomScene *s,
    unsigned id,unsigned expected) {
  int cx=-1,cy=-1;
  for(int y=0;y<ActionRoomScene_TileHeight(s,1) && cx<0;y+=2)
    for(int x=0;x<ActionRoomScene_TileWidth(s,1) && cx<0;x+=2) {
      uint16_t word;uint8_t m;
      assert(ActionRoomScene_LookupTile(s,1,x,y,&word,&m));
      if(m==id){cx=x;cy=y;}
    }
  assert(cx>=0);
  uint32_t low[256]={0},rear[256],front[256];
  unsigned opaque=0;
  for(unsigned q=0;q<4;++q) {
    uint16_t word;assert(ActionRoomScene_LookupTile(s,1,cx+(q&1),cy+(q>>1),&word,NULL));
    const uint8_t *tile=s->characters+(word&1023)*32;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
      const unsigned sx=word&0x4000?7-x:x,sy=word&0x8000?7-y:y;
      const unsigned bit=7-sx;
      const unsigned p=((tile[sy*2]>>bit)&1)|(((tile[sy*2+1]>>bit)&1)<<1)|
          (((tile[sy*2+16]>>bit)&1)<<2)|(((tile[sy*2+17]>>bit)&1)<<3);
      /* Opaque native shadow must remain opaque even at a black fade. */
      if(p){low[(q/2*8+y)*16+q%2*8+x]=0xff000000; ++opaque;}
    }
  }
  const ActionRoomSceneFrameState frame={.brightness=15};
  assert(opaque && ActionVoxel_Split(g,cx*8,cy*8,16,16,16,&frame,low,16,rear,front));
  for(unsigned i=0;i<256;++i) {
    assert(front[i]==(expected==kVoxelRearOnly?0:low[i]));
    assert(rear[i]==(expected==kVoxelForeground || expected==kVoxelFoliage?0:low[i]));
  }
}
static bool FixtureCell(const ActionRoomScene *s,unsigned id,int *tx,int *ty);
static void DesertNativeLow(const ActionRoomScene *s,int tx,int ty,uint32_t pixels[256]) {
  memset(pixels,0,256*sizeof(*pixels));
  for(unsigned q=0;q<4;++q) {
    uint16_t word;assert(ActionRoomScene_LookupTile(s,1,tx+(q&1),ty+(q>>1),&word,NULL));
    if(word&0x2000)continue; /* Native high art has its own actor-priority slot. */
    const uint8_t *tile=s->characters+(word&1023)*32;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
      const unsigned sx=word&0x4000?7-x:x,sy=word&0x8000?7-y:y,bit=7-sx;
      const unsigned p=((tile[sy*2]>>bit)&1)|(((tile[sy*2+1]>>bit)&1)<<1)|
          (((tile[sy*2+16]>>bit)&1)<<2)|(((tile[sy*2+17]>>bit)&1)<<3);
      if(!p)continue;
      const unsigned at=((((word>>10)&7)*16)+p)*2;
      const unsigned c=s->palette[at]|((unsigned)s->palette[at+1]<<8);
      const unsigned r=c&31,g=(c>>5)&31,b=(c>>10)&31;
      pixels[((q>>1)*8+y)*16+(q&1)*8+x]=0xff000000u|
          ((r*255/31)<<16)|((g*255/31)<<8)|(b*255/31);
    }
  }
}
static unsigned DesertPhaseSplit(const ActionVoxelGeometry *base,const ActionVoxelGeometry *phase,
    const ActionRoomScene *source,const ActionRoomScene *live) {
  uint32_t low[256],original[256],rear_a[256],front_a[256],rear_b[256],front_b[256];
  const ActionRoomSceneFrameState frame={.brightness=15};
  unsigned changed=0;
  for(unsigned ty=0;ty<ActionRoomScene_TileHeight(source,1);ty+=2)
    for(unsigned tx=0;tx<ActionRoomScene_TileWidth(source,1);tx+=2) {
      DesertNativeLow(live,tx,ty,low);DesertNativeLow(source,tx,ty,original);
      assert(ActionVoxel_Split(base,tx*8,ty*8,16,16,16,&frame,low,16,rear_a,front_a));
      assert(ActionVoxel_Split(phase,tx*8,ty*8,16,16,16,&frame,low,16,rear_b,front_b));
      assert(!memcmp(rear_a,rear_b,sizeof(rear_a)) && !memcmp(front_a,front_b,sizeof(front_a)));
      for(unsigned p=0;p<256;++p) {
        changed+=low[p]!=original[p];
        /* Cached topology must use this phase's actual palette/art, including
         * opaque black. Neither plane may substitute the reference phase. */
        assert(!front_a[p] || front_a[p]==low[p]);
        assert(!rear_a[p] || rear_a[p]==low[p]);
        assert(!low[p] || front_a[p]==low[p] || rear_a[p]==low[p]);
      }
      for(unsigned q=0;q<4;++q)
        assert(ActionVoxel_FrontRecess(base,tx+(q&1),ty+(q>>1))==
            ActionVoxel_FrontRecess(phase,tx+(q&1),ty+(q>>1)));
    }
  return changed;
}
/* Isolate one animation contract without relying on private compiler state.
 * Phase zero rotates the donor's nonzero colour indices, preserving exact
 * opacity; phase one displays its reviewed art. Other definitions are static. */
static void DesertAnimationContract(const ActionRoomScene *source,unsigned material,
    ActionRoomScene *live,ActionRoomScene *reference) {
  *live=*source;*reference=*source;
  size_t offset,bytes;
  assert(ActionVoxel_AnimationMatches(source,source->characters,&offset,&bytes));
  assert(!offset && bytes==512 && ActionRoomScene_CharacterAnimationPhaseCount(source)==2);
  for(unsigned id=0;id<256;++id)for(unsigned q=0;q<4;++q) {
    live->bg[0].metatiles[id*8+q*2]=0;
    live->bg[0].metatiles[id*8+q*2+1]=0x80;
  }
  memset(live->characters+offset,0,bytes);
  memset(reference->characters+offset,0,bytes*2);
  for(unsigned q=0;q<4;++q) {
    const uint8_t *definition=source->bg[0].metatiles+material*8+q*2;
    live->bg[0].metatiles[q*2]=definition[0];
    live->bg[0].metatiles[q*2+1]=q;
    const uint8_t *donor=source->characters+definition[1]*32;
    memcpy(reference->characters+offset+bytes+q*32,donor,32);
    uint8_t *current=live->characters+offset+q*32;
    for(unsigned y=0;y<8;++y) {
      current[y*2]=donor[y*2+17];current[y*2+1]=donor[y*2];
      current[y*2+16]=donor[y*2+1];current[y*2+17]=donor[y*2+16];
    }
  }
  memcpy(reference->characters+offset,live->characters+offset,bytes);
  assert(ActionVoxel_AnimationMatches(reference,live->characters,&offset,&bytes));
}
static void DesertAnimation(const ActionRoomScene *source,ActionVoxelGeometry *a,
    ActionVoxelGeometry *b,ActionVoxelSettings settings) {
  static ActionRoomScene live,reference;
  size_t offset,bytes;
  assert(ActionVoxel_AnimationCacheCandidate(kActionVoxelProfile_KassandoraDesert));
  assert(ActionVoxel_AnimationMatches(source,source->characters,&offset,&bytes));
  assert(ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,NULL,0));
  unsigned changed=0;
  for(unsigned phase=0;phase<ActionRoomScene_CharacterAnimationPhaseCount(source);++phase) {
    live=*source;
    assert(ActionRoomScene_BuildCharacters(source,0,phase,live.characters,sizeof(live.characters)));
    assert(ActionVoxel_AnimationMatches(source,live.characters,&offset,&bytes));
    assert(ActionVoxel_AnimationIsPresentationOnly(&live,source,NULL,NULL,0));
    assert(ActionVoxel_Build(b,&live,NULL,settings));SameGeometry(a,b);
    changed+=DesertPhaseSplit(a,b,source,&live);
    /* Art-address changes are semantic: every native phase must still compile
     * the same dunes after both metatile and character renumbering. */
    Renumber(&live);assert(ActionVoxel_Build(b,&live,NULL,settings));SameGeometry(a,b);
  }
  assert(changed); /* The pixel oracle must actually exercise moving sand. */
  live=*source;live.characters[offset]^=0xff;
  assert(!ActionVoxel_AnimationMatches(source,live.characters,&offset,&bytes));
  assert(!ActionVoxel_AnimationIsPresentationOnly(&live,source,NULL,NULL,0));

  const unsigned w=ActionRoomScene_TileWidth(source,1),h=ActionRoomScene_TileHeight(source,1);
  uint16_t *tiles=malloc((size_t)w*h*sizeof(*tiles));assert(tiles);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
    assert(ActionRoomScene_LookupTile(source,1,x,y,&tiles[(size_t)y*w+x],NULL));
  assert(ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,tiles,w));
  assert(!ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,tiles,w-1));
  /* Authored mirrors and pixel masks can be safe; re-prove their actual
   * topology instead of unconditionally rejecting an edited room. */
  int tx,ty;assert(FixtureCell(source,2,&tx,&ty));
  DioramaRoomOverride *terrain=calloc(1,sizeof(*terrain));assert(terrain);
  DioramaTileStamp stamp={.x=tx/2,.y=ty/2,.metatile=2};
  terrain->stamp_layers[0].cells=&stamp;terrain->stamp_layers[0].count=1;
  for(unsigned q=0;q<4;++q)stamp.words[q]=tiles[(size_t)(ty+(q>>1))*w+tx+((q&1)^1)]^0x4000;
  terrain->pixel_layers[0].count=1;
  terrain->pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=tx/2,.y=ty/2};
  terrain->pixel_layers[0].edits[0].transparent[3]=0x5555;
  terrain->pixel_layers[0].edits[0].black[5]=0xaaaa;
  assert(ActionVoxel_AnimationIsPresentationOnly(source,source,terrain,tiles,w));
  assert(ActionVoxel_BuildWithTiles(a,source,terrain,settings,tiles,w));
  live=*source;assert(ActionRoomScene_BuildCharacters(source,0,1,live.characters,sizeof(live.characters)));
  assert(ActionVoxel_AnimationIsPresentationOnly(&live,source,terrain,tiles,w));
  assert(ActionVoxel_BuildWithTiles(b,&live,terrain,settings,tiles,w));SameGeometry(a,b);
  free(terrain);free(tiles);
  assert(ActionVoxel_Build(a,source,NULL,settings));

  /* The same words can have different collision-dependent semantics. A
   * noncolliding duplicate must not hide the later solid contract. */
  DesertAnimationContract(source,1,&live,&reference);
  memcpy(live.bg[0].metatiles+8,live.bg[0].metatiles,8);
  for(unsigned q=0;q<4;++q)live.bg[0].metatiles[q*2]&=~2u;
  assert(!ActionVoxel_AnimationIsPresentationOnly(&live,&reference,NULL,NULL,0));
  for(unsigned q=0;q<4;++q)live.bg[0].metatiles[8+q*2]&=~2u;
  assert(ActionVoxel_AnimationIsPresentationOnly(&live,&reference,NULL,NULL,0));
  /* A live or authored publication can introduce the unsafe colliding
   * context even when every stored definition is presentation-only. */
  for(unsigned q=0;q<4;++q) {
    live.bg[0].metatiles[8+q*2]=source->bg[0].metatiles[8+q*2];
    live.bg[0].metatiles[8+q*2+1]=0x80;
  }
  assert(ActionVoxel_AnimationIsPresentationOnly(&live,&reference,NULL,NULL,0));
  terrain=calloc(1,sizeof(*terrain));assert(terrain);
  stamp=(DioramaTileStamp){.x=-1,.y=-1,.metatile=1};
  for(unsigned q=0;q<4;++q)
    stamp.words[q]=(uint16_t)((source->bg[0].metatiles[8+q*2]<<8)&kActionRoomSceneTileWordMask)|
        ActionRoomScene_BgAttributes(source,1)|q;
  terrain->stamp_layers[0].cells=&stamp;terrain->stamp_layers[0].count=1;
  assert(!ActionVoxel_AnimationIsPresentationOnly(&live,&reference,terrain,NULL,0));
  tiles=malloc((size_t)w*h*sizeof(*tiles));assert(tiles);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
    assert(ActionRoomScene_LookupTile(&live,1,x,y,&tiles[(size_t)y*w+x],NULL));
  assert(ActionVoxel_AnimationIsPresentationOnly(&live,&reference,NULL,tiles,w));
  assert(FixtureCell(&live,1,&tx,&ty));
  for(unsigned q=0;q<4;++q)tiles[(size_t)(ty+(q>>1))*w+tx+(q&1)]=stamp.words[q];
  assert(!ActionVoxel_AnimationIsPresentationOnly(&live,&reference,NULL,tiles,w));
  free(tiles);
  free(terrain);

  /* Unrecognized solid art also needs an exact opacity proof. Merely keeping
   * fallback kinds/flags equal would let an animated hole change the mesh. */
  DesertAnimationContract(source,2,&live,&reference);
  memset(live.characters,0x55,bytes);memset(reference.characters,0x55,bytes*2);
  assert(ActionVoxel_AnimationIsPresentationOnly(&live,&reference,NULL,NULL,0));
  reference.characters[bytes]=0;reference.characters[bytes+1]=0;
  reference.characters[bytes+16]=0;reference.characters[bytes+17]=0;
  assert(!ActionVoxel_AnimationIsPresentationOnly(&live,&reference,NULL,NULL,0));
  /* A later exact phase may introduce reviewed foreground/bone structure;
   * accepting the current inert phase alone would be insufficient. */
  DesertAnimationContract(source,source->map==1?0x72:0x5c,&live,&reference);
  assert(!ActionVoxel_AnimationIsPresentationOnly(&live,&reference,NULL,NULL,0));
}
#include "action_voxel_skeleton.inc"
static unsigned DesertFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned room=1;room<=2;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/030%u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    ActionRoomScene *source=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(3,room);
    assert(ActionVoxel_Build(a,source,NULL,settings) && ActionVoxel_Active(a,3,room));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_KassandoraDesert);
    assert(!ActionVoxel_Active(a,3,3) && ActionVoxel_PlayRecess(a)==.065f);
    DesertAnimation(source,a,b,settings);
    variant=*source;Renumber(&variant);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    variant=*source;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    DesertMaterial(a,source,0x08,kVoxelSolid);
    if(room==1) {
      DesertSkeletonBody(a,source);
      DesertMaterial(a,source,0x6d,kVoxelForeground);
      DesertMaterial(a,source,0x69,kVoxelForeground);
      DesertMaterial(a,source,0x5e,kVoxelFoliage);
    } else {
      DesertMaterial(a,source,0x80,kVoxelRearOnly);
      DesertMaterial(a,source,0x99,kVoxelRearOnly);
      DesertMaterial(a,source,0xa2,kVoxelSolid);
      DesertMaterial(a,source,0x5c,kVoxelForeground);
    }
    unsigned count,solid=0,trim=0,soft=0;
    const VoxelEdge *edges=ActionVoxel_Edges(a,&count);
    assert(count && count<kVoxelMaxEdges);
    for(unsigned i=0;i<count;++i) {
      solid+=edges[i].far_rank==2;trim+=edges[i].far_rank==1;soft+=edges[i].soft_surface;
      VoxelEdgeSpan span;
      if(ActionVoxel_ResolveEdgeSpan(a,&edges[i],kVoxelEdgeNative,0,settings.depth,&span))
        assert(span.near_depth>=0 && span.far_depth<=settings.depth);
    }
    assert(solid && trim && soft);
    unsigned plants;const VoxelPatch *patches=ActionVoxel_Patches(a,&plants);
    assert(room==1?plants>0:plants==0);
    for(unsigned i=0;i<plants;++i)assert(patches[i].kind==kVoxelPatch_Thorns);
    printf("desert room %u terrain %u: %u contours, %u cactus patches\n",room,region,count,plants);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 6;
}
static bool FixtureCell(const ActionRoomScene *s,unsigned id,int *tx,int *ty) {
  for(int y=0;y<ActionRoomScene_TileHeight(s,1);y+=2)
    for(int x=0;x<ActionRoomScene_TileWidth(s,1);x+=2) {
      uint16_t word;uint8_t m;
      assert(ActionRoomScene_LookupTile(s,1,x,y,&word,&m));
      if(m==id){*tx=x;*ty=y;return true;}
    }
  return false;
}
static void TombIntervals(const ActionVoxelGeometry *g) {
  const float depth=ActionVoxel_Depth(g),trim=ActionVoxel_TrimDepth(g),rear=depth-trim;
  const struct {unsigned from,to,mounted,neighbor;float near,far;} cases[]={
    {1,0,0,0,0,trim}, {1,0,1,0,rear,depth},
    {2,1,0,1,0,rear}, {2,1,0,0,trim,depth},
    {1,1,0,1,0,trim}, {1,1,1,0,rear,depth}
  };
  for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
    VoxelEdge e={.far_rank=cases[i].from,.near_rank=cases[i].to,
      .rear_mounted=cases[i].mounted,.neighbor_rear_mounted=cases[i].neighbor};
    VoxelEdgeSpan span;
    assert(ActionVoxel_ResolveEdgeSpan(g,&e,kVoxelEdgeNative,0,depth,&span));
    assert(fabsf(span.near_depth-cases[i].near)<.00001f && fabsf(span.far_depth-cases[i].far)<.00001f);
    assert(fabsf(ActionVoxel_EdgeRecess(g,&e)-(cases[i].mounted?rear:0))<.00001f);
  }
}
static unsigned TombFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant, animation_reference;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned room=3;room<=6;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/030%u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    ActionRoomScene *source=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(3,room);
    assert(ActionVoxel_Build(a,source,NULL,settings) && ActionVoxel_Active(a,3,room));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_KassandoraTomb);
    assert(ActionVoxel_PlayRecess(a)==.08f && ActionVoxel_HasCutouts(a));
    size_t animation_offset,animation_bytes;
    assert(ActionVoxel_AnimationMatches(source,source->characters,&animation_offset,&animation_bytes));
    assert(ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,NULL,0));
    for(unsigned phase=0;phase<ActionRoomScene_CharacterAnimationPhaseCount(source);++phase) {
      variant=*source;
      assert(ActionRoomScene_BuildCharacters(source,0,phase,variant.characters,sizeof(variant.characters)));
      assert(ActionVoxel_AnimationMatches(source,variant.characters,&animation_offset,&animation_bytes));
      assert(ActionVoxel_AnimationIsPresentationOnly(&variant,source,NULL,NULL,0));
      assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
      /* Compare the observable split, not private rear/empty tags: every live
       * pixel must remain on the same plane at every native animation phase. */
      uint32_t input[256],rear_a[256],front_a[256],rear_b[256],front_b[256];
      for(unsigned i=0;i<256;++i)input[i]=0xff000000u+i;
      const ActionRoomSceneFrameState frame={.brightness=15};
      for(unsigned y=0;y<ActionRoomScene_TileHeight(source,1);y+=2)
        for(unsigned x=0;x<ActionRoomScene_TileWidth(source,1);x+=2) {
          assert(ActionVoxel_Split(a,x*8,y*8,16,16,16,&frame,input,16,rear_a,front_a));
          assert(ActionVoxel_Split(b,x*8,y*8,16,16,16,&frame,input,16,rear_b,front_b));
          assert(!memcmp(rear_a,rear_b,sizeof(rear_a)) && !memcmp(front_a,front_b,sizeof(front_a)));
          assert(ActionVoxel_FrontRecess(a,x,y)==ActionVoxel_FrontRecess(b,x,y));
        }
    }
    variant=*source;variant.characters[animation_offset]^=0xff;
    assert(!ActionVoxel_AnimationMatches(source,variant.characters,&animation_offset,&animation_bytes));
    assert(!ActionVoxel_AnimationIsPresentationOnly(&variant,source,NULL,NULL,0));
    /* Collision on an animated definition, a live solid publication or an
     * authored solid stamp disqualifies reuse, even outside the viewport. */
    variant=*source;
    variant.bg[0].metatiles[0]=2;variant.bg[0].metatiles[1]=animation_offset/32;
    assert(!ActionVoxel_AnimationIsPresentationOnly(&variant,source,NULL,NULL,0));
    const unsigned tile_w=ActionRoomScene_TileWidth(source,1),tile_h=ActionRoomScene_TileHeight(source,1);
    uint16_t *live_tiles=malloc((size_t)tile_w*tile_h*sizeof(*live_tiles));assert(live_tiles);
    for(unsigned y=0;y<tile_h;++y)for(unsigned x=0;x<tile_w;++x)
      assert(ActionRoomScene_LookupTile(source,1,x,y,&live_tiles[(size_t)y*tile_w+x],NULL));
    assert(ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,live_tiles,tile_w));
    int solid_x,solid_y;assert(FixtureCell(source,0x0a,&solid_x,&solid_y));
    live_tiles[(size_t)solid_y*tile_w+solid_x]=animation_offset/32;
    assert(!ActionVoxel_AnimationIsPresentationOnly(source,source,NULL,live_tiles,tile_w));
    free(live_tiles);
    DioramaRoomOverride *authored=calloc(1,sizeof(*authored));assert(authored);
    DioramaTileStamp stamp={.x=-1,.y=-1,.metatile=0x0a};
    authored->stamp_layers[0].cells=&stamp;
    authored->stamp_layers[0].count=1;
    for(unsigned q=0;q<4;++q)authored->stamp_layers[0].cells[0].words[q]=animation_offset/32;
    assert(!ActionVoxel_AnimationIsPresentationOnly(source,source,authored,NULL,0));free(authored);
    /* A later phase that turns an otherwise harmless definition into a
     * reviewed rear column must fail the proof before that phase is live. */
    variant=*source;animation_reference=*source;
    for(unsigned q=0;q<4;++q) {
      const uint8_t *column=source->bg[0].metatiles+0x36*8+q*2;
      variant.bg[0].metatiles[q*2]=column[0];
      variant.bg[0].metatiles[q*2+1]=animation_offset/32+q;
      memcpy(animation_reference.characters+animation_offset+2*animation_bytes+q*32,
             source->characters+column[1]*32,32);
    }
    assert(ActionVoxel_AnimationMatches(&animation_reference,variant.characters,&animation_offset,&animation_bytes));
    assert(!ActionVoxel_AnimationIsPresentationOnly(&variant,&animation_reference,NULL,NULL,0));
    variant=*source;Renumber(&variant);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    variant=*source;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    DesertMaterial(a,source,0x0a,kVoxelSolid);
    DesertMaterial(a,source,0x26,kVoxelRearOnly);
    int tx,ty;
    assert(FixtureCell(source,0x36,&tx,&ty));
    assert(fabsf(ActionVoxel_FrontRecess(a,tx,ty)-.18f)<.00001f);
    if(FixtureCell(source,0x3d,&tx,&ty)) {
      uint32_t low[256],rear[256],front[256];
      for(unsigned i=0;i<256;++i)low[i]=0xff123456;
      const ActionRoomSceneFrameState frame={.brightness=15};
      assert(ActionVoxel_Split(a,tx*8,ty*8,16,16,16,&frame,low,16,rear,front));
      /* Reviewed slope: black corner (12,4), gold face (4,12). The black
       * belongs to the back wall, not a cutout exposing the distant BG2. */
      assert(!front[4*16+12] && rear[4*16+12]==low[4*16+12]);
      assert(front[12*16+4]==low[12*16+4] && rear[12*16+4]==low[12*16+4]);
      uint8_t mask[8]={0};assert(!ActionVoxel_CutoutRows(a,tx+1,ty,mask));
    }
    if(FixtureCell(source,0x75,&tx,&ty))assert(ActionVoxel_FrontRecess(a,tx,ty)==0);
    TombIntervals(a);
    unsigned count,recessed=0,front_trim=0,join=0;
    const VoxelEdge *edges=ActionVoxel_Edges(a,&count);
    assert(count && count<kVoxelMaxEdges);
    for(unsigned i=0;i<count;++i) {
      recessed+=edges[i].rear_mounted;
      front_trim+=edges[i].far_rank==1 && !edges[i].rear_mounted;
      join+=edges[i].far_rank==2 && edges[i].neighbor_rear_mounted;
      VoxelEdgeSpan span;
      if(ActionVoxel_ResolveEdgeSpan(a,&edges[i],kVoxelEdgeNative,0,settings.depth,&span))
        assert(span.near_depth>=0 && span.far_depth<=settings.depth);
    }
    assert(recessed && front_trim && join);
    unsigned gates;
    const VoxelGate *panels=ActionVoxel_Gates(a,&gates);
    assert(room==3?gates==0:gates>0);
    for(unsigned i=0;i<gates;++i)assert(panels[i].centered && panels[i].width>0 && panels[i].width<=8);
    /* Publish the reviewed native open definitions in place of the closed
     * ones. The blue grille disappears; neither gold jamb becomes a panel. */
    variant=*source;
    const unsigned closed[]={0xba,0xbb,0xc2,0xc3},open[]={0x1a,0x1b,0x22,0x23};
    for(unsigned i=0;i<4;++i)memcpy(variant.bg[0].metatiles+closed[i]*8,source->bg[0].metatiles+open[i]*8,8);
    assert(ActionVoxel_Build(b,&variant,NULL,settings) && ActionVoxel_Active(b,3,room));
    unsigned opened;ActionVoxel_Gates(b,&opened);assert(!opened);
    unsigned patches;const VoxelPatch *p=ActionVoxel_Patches(a,&patches);unsigned spikes=0;
    for(unsigned i=0;i<patches;++i)spikes+=p[i].kind==kVoxelPatch_Spikes;
    assert((room==4 || room==5)?spikes>0:spikes==0);
    printf("tomb room %u terrain %u: %u contours, %u grille panels, %u spike patches\n",room,region,count,gates,spikes);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 12;
}
static unsigned FixturePixel(const ActionRoomScene *s,int x,int y) {
  uint16_t word;
  assert(ActionRoomScene_LookupTile(s,1,x/8,y/8,&word,NULL));
  const uint8_t *c=s->characters+(word&1023)*32;
  const unsigned sx=word&0x4000?7-(x&7):x&7,sy=word&0x8000?7-(y&7):y&7,bit=7-sx;
  return ((c[sy*2]>>bit)&1)|(((c[sy*2+1]>>bit)&1)<<1)|
      (((c[sy*2+16]>>bit)&1)<<2)|(((c[sy*2+17]>>bit)&1)<<3);
}
static unsigned MountainFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned room=1;room<=3;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/040%u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    const ActionRoomScene *source=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,room);
    assert(ActionVoxel_Build(a,source,NULL,settings) && ActionVoxel_Active(a,4,room));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_AitosMountains);
    assert(ActionVoxel_Depth(a)==.20f && ActionVoxel_PlayRecess(a)==.065f);
    variant=*source;Renumber(&variant);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    variant=*source;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    DesertMaterial(a,source,0x08,kVoxelSolid);
    DesertMaterial(a,source,0x0a,kVoxelRearOnly);
    unsigned count,soft=0;const VoxelEdge *edges=ActionVoxel_Edges(a,&count);
    assert(count && count<kVoxelMaxEdges && !ActionVoxel_HasCutouts(a));
    for(unsigned i=0;i<count;++i)soft+=edges[i].soft_surface;
    assert(soft);
    unsigned liquids;const VoxelWaterSurface *water=ActionVoxel_WaterSurfaces(a,&liquids);
    if(room==1) {
      DesertMaterial(a,source,0xd0,kVoxelRearOnly);
      DesertMaterial(a,source,0xd7,kVoxelRearOnly);
      assert(liquids);unsigned width=0,expected=0;
      for(size_t i=0;i<source->bg[0].map_size;++i)expected+=source->bg[0].map[i]==0xdf?16:0;
      for(unsigned i=0;i<liquids;++i) {
        assert(water[i].opaque && water[i].high_priority && !water[i].background);
        width+=water[i].width;
      }
      assert(width==expected);
    } else {
      assert(!liquids);
      int tx,ty;assert(FixtureCell(source,0x4e,&tx,&ty));
      uint32_t low[256],rear[256],front[256];
      unsigned foam=0,rock=0;
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
        unsigned pixel=FixturePixel(source,tx*8+x,ty*8+y);
        low[y*16+x]=pixel?0xff123456:0;foam+=pixel==7;rock+=pixel && pixel!=7;
      }
      const ActionRoomSceneFrameState frame={.brightness=15};
      assert(foam && rock && ActionVoxel_Split(a,tx*8,ty*8,16,16,16,&frame,low,16,rear,front));
      assert(!memcmp(front,low,sizeof(low)) && !memcmp(rear,low,sizeof(low)));
      unsigned island_edges=0;
      for(unsigned i=0;i<count;++i) {
        const VoxelEdge *e=&edges[i];uint8_t id;uint16_t word;
        assert(ActionRoomScene_LookupTile(source,1,e->x/8,e->y/8,&word,&id));
        if(id!=0x4e && id!=0x4f && id!=0xf4 && id!=0xf6 && id!=0xfc && id!=0xfe)continue;
        ++island_edges;
        assert(FixturePixel(source,e->x,e->y)!=7); /* White spray never becomes a solid cap. */
      }
      assert(island_edges);
    }
    variant=*source;memset(variant.characters,0,sizeof(variant.characters));
    assert(ActionVoxel_Build(b,&variant,NULL,settings) && !ActionVoxel_Active(b,4,room));
    printf("mountain room %u terrain %u: %u contours, %u molten surfaces\n",room,region,count,liquids);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 9;
}
static unsigned CavernFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;
  static ActionRoomScene variant,reference;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned room=4;room<=7;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/040%u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    const ActionRoomScene *s=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(4,room);
    assert(ActionVoxel_Build(a,s,NULL,settings) && ActionVoxel_Active(a,4,room));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_AitosCaverns);
    assert(ActionVoxel_Depth(a)==.24f && ActionVoxel_PlayRecess(a)==.08f);
    variant=*s;Renumber(&variant);assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    variant=*s;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    unsigned pools,width=0,expected=0,edges,gates;
    const VoxelWaterSurface *water=ActionVoxel_WaterSurfaces(a,&pools);
    for(size_t i=0;i<s->bg[0].map_size;++i)expected+=s->bg[0].map[i]==1?16:0;
    for(unsigned i=0;i<pools;++i) {
      assert(water[i].opaque && water[i].high_priority && !water[i].background);
      width+=water[i].width;
    }
    assert(width==expected && (room==7?!pools:pools>0));
    ActionVoxel_Edges(a,&edges);assert(edges && edges<kVoxelMaxEdges);
    const VoxelGate *gate=ActionVoxel_Gates(a,&gates);
    assert(room==4?!gates:gates>0);
    for(unsigned i=0;i<gates;++i)assert(gate[i].centered && gate[i].width && gate[i].width<=8);
    variant=*s;
    memcpy(variant.bg[0].metatiles+0x71*8,s->bg[0].metatiles+0x25*8,8);
    memcpy(variant.bg[0].metatiles+0x72*8,s->bg[0].metatiles+0x26*8,8);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));unsigned opened;ActionVoxel_Gates(b,&opened);assert(!opened);
    if(room<7) {
      size_t offset,bytes;
      assert(ActionVoxel_AnimationMatches(s,s->characters,&offset,&bytes));
      assert(ActionVoxel_AnimationIsPresentationOnly(s,s,NULL,NULL,0));
      for(unsigned phase=0;phase<ActionRoomScene_CharacterAnimationPhaseCount(s);++phase) {
        variant=*s;assert(ActionRoomScene_BuildCharacters(s,0,phase,variant.characters,sizeof(variant.characters)));
        assert(ActionVoxel_AnimationIsPresentationOnly(&variant,s,NULL,NULL,0));
        assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
        uint32_t input[256],ra[256],fa[256],rb[256],fb[256];
        for(unsigned i=0;i<256;++i)input[i]=0xff000000u+i;
        const ActionRoomSceneFrameState frame={.brightness=15};
        for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1);y+=2)
          for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1);x+=2) {
            assert(ActionVoxel_Split(a,x*8,y*8,16,16,16,&frame,input,16,ra,fa));
            assert(ActionVoxel_Split(b,x*8,y*8,16,16,16,&frame,input,16,rb,fb));
            assert(!memcmp(ra,rb,sizeof(ra)) && !memcmp(fa,fb,sizeof(fa)));
          }
      }
      variant=*s;variant.characters[offset]^=0xff;
      assert(!ActionVoxel_AnimationIsPresentationOnly(&variant,s,NULL,NULL,0));
      variant=*s;variant.bg[0].metatiles[8]|=2;
      assert(!ActionVoxel_AnimationIsPresentationOnly(&variant,s,NULL,NULL,0));
      /* A changed later phase must be rejected before it becomes visible. */
      reference=*s;reference.characters[offset+2*bytes+2*32]^=0xff;
      assert(!ActionVoxel_AnimationIsPresentationOnly(s,&reference,NULL,NULL,0));
    }
    variant=*s;memset(variant.characters,0,sizeof(variant.characters));
    assert(ActionVoxel_Build(b,&variant,NULL,settings) && !ActionVoxel_Active(b,4,room));
    printf("cavern room %u terrain %u: %u contours, %u molten surfaces, %u grilles\n",room,region,edges,pools,gates);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 12;
}
static unsigned JungleFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned room=1;room<=3;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/050%u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    const ActionRoomScene *s=&fixture.scene;
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(5,room);
    assert(ActionVoxel_Build(a,s,NULL,settings) && ActionVoxel_Active(a,5,room));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_MarahnaJungle);
    assert(ActionVoxel_Depth(a)==.20f && ActionVoxel_PlayRecess(a)==.065f);
    variant=*s;Renumber(&variant);assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    variant=*s;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    for(unsigned phase=0;phase<ActionRoomScene_CharacterAnimationPhaseCount(s);++phase) {
      variant=*s;assert(ActionRoomScene_BuildCharacters(s,0,phase,variant.characters,sizeof(variant.characters)));
      assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    }
    unsigned pools,width=0,expected=0,edges;
    const VoxelWaterSurface *water=ActionVoxel_WaterSurfaces(a,&pools);assert(pools);
    for(size_t i=0;i<s->bg[0].map_size;++i) {
      const unsigned id=s->bg[0].map[i];
      if(id==1 || id==0x34 || id==0x6e || id==0x6f || id==0xac || id==0xad || id==0xb0 || id==0xb8 || id==0xbf)expected+=16;
    }
    for(unsigned i=0;i<pools;++i) {
      assert(water[i].opaque && water[i].high_priority && !water[i].background);
      assert(water[i].sample_y==13 && water[i].sample_height==3);width+=water[i].width;
    }
    assert(width==expected);
    const VoxelEdge *contours=ActionVoxel_Edges(a,&edges);assert(edges && edges<kVoxelMaxEdges);
    unsigned rear=0,solid=0;
    for(unsigned i=0;i<edges;++i) {rear+=contours[i].rear_mounted!=0;solid+=contours[i].colliding!=0;}
    assert(rear && solid);
    /* Water collision remains gameplay data, never a solid wall extrusion. */
    uint32_t input[256],back[256],front[256];
    for(unsigned i=0;i<256;++i)input[i]=0xff123456;
    const ActionRoomSceneFrameState frame={.brightness=15};unsigned submerged=0;
    for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1);y+=2)
      for(unsigned x=0;x<ActionRoomScene_TileWidth(s,1);x+=2) {
        uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,x,y,&word,&id));
        if(id!=2 && id!=3 && id!=0xab)continue;
        assert(ActionVoxel_Split(a,x*8,y*8,16,16,16,&frame,input,16,back,front));
        for(unsigned i=0;i<256;++i)assert(back[i]==input[i] && !front[i]);
        ++submerged;
      }
    assert(submerged);
    variant=*s;variant.bg[0].map[0]=0x29;variant.bg[0].map[16]=0x31;
    assert(ActionVoxel_Build(b,&variant,NULL,settings));unsigned closed,opened;
    const VoxelGate *gates=ActionVoxel_Gates(b,&closed);assert(closed);
    for(unsigned i=0;i<closed;++i)assert(gates[i].centered && gates[i].width<=8);
    for(size_t i=0;i<variant.bg[0].map_size;++i)
      if(variant.bg[0].map[i]==0x29 || variant.bg[0].map[i]==0x31)++variant.bg[0].map[i];
    assert(ActionVoxel_Build(b,&variant,NULL,settings));ActionVoxel_Gates(b,&opened);assert(!opened);
    variant=*s;memset(variant.characters,0,sizeof(variant.characters));
    assert(ActionVoxel_Build(b,&variant,NULL,settings) && !ActionVoxel_Active(b,5,room));
    printf("jungle room %u terrain %u: %u contours (%u rear), %u shorelines, %u submerged cells\n",room,region,edges,rear,pools,submerged);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 9;
}
static unsigned MarahnaTempleFixtures(const char *directory) {
  static ActionSceneSnapshot fixture;static ActionRoomScene variant;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  for(unsigned room=4;room<=8;++room)for(unsigned region=0;region<3;++region) {
    char path[1024];snprintf(path,sizeof(path),"%s/050%u-%u.arscene",directory,room,region);
    FILE *f=fopen(path,"rb");assert(f);
    const size_t n=fread(encoded,1,sizeof(encoded),f);assert(!ferror(f));fclose(f);
    const ActionRoomScene *s=&fixture.scene;assert(ActionSceneSnapshot_Decode(encoded,n,&fixture));
    ActionVoxelGeometry *a=ActionVoxel_Create(),*b=ActionVoxel_Create();assert(a&&b);
    const ActionVoxelSettings settings=ActionVoxel_SettingsForRoom(5,room);
    assert(ActionVoxel_Build(a,s,NULL,settings) && ActionVoxel_Active(a,5,room));
    assert(ActionVoxel_Profile(a)==kActionVoxelProfile_MarahnaTemple);
    assert(ActionVoxel_Depth(a)==.24f && ActionVoxel_PlayRecess(a)==.08f);
    variant=*s;Renumber(&variant);assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    variant=*s;memset(variant.palette,0,sizeof(variant.palette));
    assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    size_t offset,bytes;
    assert(ActionVoxel_AnimationMatches(s,s->characters,&offset,&bytes));
    assert(ActionVoxel_AnimationIsPresentationOnly(s,s,NULL,NULL,0));
    for(unsigned phase=0;phase<ActionRoomScene_CharacterAnimationPhaseCount(s);++phase) {
      variant=*s;assert(ActionRoomScene_BuildCharacters(s,0,phase,variant.characters,sizeof(variant.characters)));
      assert(ActionVoxel_AnimationIsPresentationOnly(&variant,s,NULL,NULL,0));
      assert(ActionVoxel_Build(b,&variant,NULL,settings));SameGeometry(a,b);
    }
    variant=*s;variant.characters[offset]^=0xff;
    assert(!ActionVoxel_AnimationIsPresentationOnly(&variant,s,NULL,NULL,0));
    /* An animated solid, even in an unused definition, invalidates reuse. */
    variant=*s;variant.bg[0].metatiles[0]=2;variant.bg[0].metatiles[1]=offset/32;
    assert(!ActionVoxel_AnimationIsPresentationOnly(&variant,s,NULL,NULL,0));
    unsigned count,rear=0,solid=0,front=0;
    const VoxelEdge *edges=ActionVoxel_Edges(a,&count);assert(count && count<kVoxelMaxEdges);
    for(unsigned i=0;i<count;++i) {
      rear+=edges[i].rear_mounted!=0;solid+=edges[i].colliding!=0;
      front+=edges[i].high_priority && !edges[i].rear_mounted && !edges[i].colliding;
    }
    assert(rear && solid);if(room!=8)assert(front);
    TombIntervals(a);
    unsigned patches;const VoxelPatch *spikes=ActionVoxel_Patches(a,&patches);
    if(room!=4)assert(patches);
    for(unsigned i=0;i<patches;++i)assert(spikes[i].kind==kVoxelPatch_Spikes);
    int tx,ty;
    if(FixtureCell(s,0x35,&tx,&ty))assert(fabsf(ActionVoxel_FrontRecess(a,tx,ty)-.18f)<.00001f);
    if(FixtureCell(s,0x20,&tx,&ty))assert(ActionVoxel_FrontRecess(a,tx,ty)==0);
    DesertMaterial(a,s,0x08,kVoxelSolid);
    DesertMaterial(a,s,0x43,kVoxelRearOnly);
    /* Removing the source bars cannot leave a depth gate behind. Source
     * tile identities remain constant, exercising live composition changes. */
    unsigned closed,opened;ActionVoxel_Gates(a,&closed);
    if(room!=4)assert(closed);
    variant=*s;memset(variant.characters+0x9a*32,0,32);
    assert(ActionVoxel_Build(b,&variant,NULL,settings));ActionVoxel_Gates(b,&opened);assert(!opened);
    variant=*s;memset(variant.characters,0,sizeof(variant.characters));
    assert(ActionVoxel_Build(b,&variant,NULL,settings) && !ActionVoxel_Active(b,5,room));
    printf("Marahna temple %u terrain %u: %u contours, %u rear, %u spike strips, %u gates\n",room,region,count,rear,patches,closed);
    ActionVoxel_Release(a);ActionVoxel_Release(b);
  }
  return 15;
}
#include "action_voxel_northwall.inc"
#include "action_voxel_death_heim.inc"
#include "action_voxel_marsh_audit.inc"
#include "action_voxel_castle_columns.inc"
#include "action_voxel_castle_audit.inc"
#include "action_voxel_temple_audit.inc"
#include "action_voxel_authored.inc"
#include "action_voxel_groups.inc"
#include "action_voxel_surfaces.inc"
#include "action_voxel_surface_roles.inc"
#include "action_voxel_raised_scenery.inc"
#include "action_voxel_colored_caps.inc"
#include "action_voxel_ice_matte.inc"
#include "action_voxel_rock_surfaces.inc"
#include "action_voxel_snow_trunk.inc"
#include "action_voxel_snow_floor.inc"
#include "action_voxel_jungle_backing.inc"
#include "action_voxel_jungle_pillars.inc"
#include "action_voxel_jungle_course.inc"
#include "action_voxel_bamboo_backing.inc"
#include "action_voxel_mountain_slopes.inc"
#include "action_voxel_regional_spikes.inc"
#include "action_trunk_side_fixtures.inc"
#include "action_trunk_detail_backing.inc"
#include "action_voxel_root_backing.inc"
#include "action_voxel_aitos_cliff_foot.inc"
#include "action_voxel_jungle_exit_backing.inc"
#include "action_voxel_tomb_caps.inc"
#include "action_voxel_castle_terminal_matte.inc"
#include "action_voxel_castle_purple_band.inc"
#include "action_voxel_castle_backing.inc"
#include "action_voxel_castle_wolf_relief.inc"
#include "action_voxel_tomb_contours.inc"
#include "action_voxel_summit_crest.inc"
#include "action_voxel_castle_cap_finish.inc"
#include "action_voxel_castle_post_join.inc"
#include "action_voxel_snow_slopes.inc"
#include "diorama_trunk_guard.inc"
#include "action_voxel_jungle_water.inc"
#include "action_voxel_jungle_terminal.inc"
#include "action_voxel_wet_crown_risers.inc"
#include "action_voxel_desert_surfaces.inc"
#include "action_voxel_cavern_surfaces.inc"
#include "action_voxel_aitos_surface_findings.inc"
#include "action_voxel_temple_native_surfaces.inc"
#include "action_voxel_ice_inward.inc"
#include "action_voxel_ice_tips.inc"
#include "action_voxel_marsh_rock_surfaces.inc"
#include "action_voxel_marsh_actor_bands.inc"
#include "action_voxel_surface_budget.inc"
#include "action_voxel_marsh_beam_tops.inc"
#include "action_voxel_bloodpool_surface_findings.inc"
#include "action_voxel_head_crown.inc"
#include "action_voxel_hub_surfaces.inc"
#include "action_voxel_sand_live.inc"
#include "action_voxel_sand_transitions.inc"
#include "action_voxel_desert_pillars.inc"
#include "action_voxel_desert_audit_joins.inc"
#include "action_voxel_snow_cap_phase.inc"
#include "action_voxel_snow_closing_risers.inc"
int main(int argc,char **argv) {
  if(argc>1 && !strcmp(argv[1],"--surface-inventory"))return SurfaceFixtureInventory(argc,argv);
  SummitCrestFixtures(getenv("AR_VOXEL_SUMMIT_CREST_FIXTURES"));
  Lifecycle();
  TrunkGuardFixtures();
  JungleWaterColorFixtures(getenv("AR_VOXEL_JUNGLE_WATER_FIXTURES"));
  JungleTerminalFixtures(getenv("AR_VOXEL_JUNGLE_TERMINAL_FIXTURES"));
  AuthoredContracts();
  RaisedSceneryFixtures(getenv("AR_VOXEL_RAISED_FIXTURES"));
  ColoredCapFixtures(getenv("AR_VOXEL_COLORED_CAP_FIXTURES"));
  IceMatteFixtures(getenv("AR_VOXEL_ICE_MATTE_FIXTURES"));
  RockSurfaceFixtures(getenv("AR_VOXEL_ROCK_FIXTURES"));
  WetCrownRiserFixtures(getenv("AR_VOXEL_WET_CROWN_FIXTURES"));
  DesertSurfaceFixtures(getenv("AR_VOXEL_DESERT_SURFACE_FIXTURES"));
  CavernNativeSurfaceFixtures(getenv("AR_VOXEL_CAVERN_NATIVE_FIXTURES"));
  AitosSurfaceFindingsFixtures(getenv("AR_VOXEL_AITOS_SURFACE_FINDINGS_FIXTURES"));
  TempleNativeSurfaceFixtures(getenv("AR_VOXEL_TEMPLE_NATIVE_SURFACE_FIXTURES"));
  if(getenv("AR_VOXEL_ICE_INWARD_BASELINE"))
    IceInwardFixtures(getenv("AR_VOXEL_ICE_INWARD_FIXTURES"));
  else IceTipFixtures(getenv("AR_VOXEL_ICE_INWARD_FIXTURES"));
  IceTipFixtures(getenv("AR_VOXEL_ICE_TIP_FIXTURES"));
  MarshRockSurfaceFixtures(getenv("AR_VOXEL_MARSH_ROCK_FIXTURES"));
  MarshActorBandFixtures(getenv("AR_VOXEL_MARSH_ACTOR_FIXTURES"));
  SurfaceBudgetFixtures(getenv("AR_VOXEL_SURFACE_BUDGET_FIXTURES"));
  MarshBeamTopFixtures(getenv("AR_VOXEL_MARSH_BEAM_FIXTURES"));
  BloodpoolSurfaceFindingFixtures(getenv("AR_VOXEL_BLOODPOOL_FINDING_FIXTURES"));
  if(getenv("AR_VOXEL_HEAD_CROWN_FIXTURES")) HeadCrownFixtures(getenv("AR_VOXEL_HEAD_CROWN_FIXTURES"),getenv("AR_VOXEL_HEAD_CROWN_BASELINE")==NULL);
  HubSurfaceFixtures(getenv("AR_VOXEL_HUB_SURFACE_FIXTURES"));
  if(getenv("AR_SAND_SCENES")) SandLiveFixtures();
  if(getenv("AR_SAND_TRANSITION_SCENES")) SandTransitionFixtures();
  if(getenv("AR_DESERT_PILLAR_SCENES")) DesertPillarFixtures();
  DesertAuditJoinFixtures(getenv("AR_VOXEL_DESERT_AUDIT_FIXTURES"));
  SnowCapPhaseFixtures(getenv("AR_VOXEL_SNOW_CAP_PHASE_FIXTURES"));
  SnowClosingRiserFixtures(getenv("AR_VOXEL_SNOW_CLOSING_RISER_FIXTURES"));
  SnowTrunkFixtures(getenv("AR_VOXEL_SNOW_TRUNK_FIXTURES"));
  SnowFloorFixtures(getenv("AR_VOXEL_SNOW_FLOOR_FIXTURES"));
  JungleBackingFixtures(getenv("AR_VOXEL_JUNGLE_BACKING_FIXTURES"));
  JungleForegroundPillarFixtures(getenv("AR_VOXEL_JUNGLE_PILLAR_FIXTURES"));
  JungleCourseFixtures(getenv("AR_VOXEL_JUNGLE_COURSE_FIXTURES"));
  BambooBackingFixtures(getenv("AR_VOXEL_BAMBOO_FIXTURES"));
  MountainSlopeFixtures(getenv("AR_VOXEL_MOUNTAIN_SLOPE_FIXTURES"));
  RegionalSpikeFixtures(getenv("AR_VOXEL_REGIONAL_SPIKE_FIXTURES"));
  TrunkSideFixtures(getenv("AR_TRUNK_SIDE_FIXTURES"));
  TrunkDetailBackingFixtures(getenv("AR_TRUNK_SIDE_FIXTURES"));
  ForestRootBackingFixtures(getenv("AR_VOXEL_ROOT_BACKING_FIXTURES"));
  AitosCliffFootFixtures(getenv("AR_VOXEL_AITOS_CLIFF_FOOT_FIXTURES"));
  JungleExitBackingFixtures(getenv("AR_VOXEL_JUNGLE_EXIT_BACKING_FIXTURES"));
  TombCapFixtures(getenv("AR_VOXEL_TOMB_CAP_FIXTURES"));
  CastleTerminalMatteFixtures(getenv("AR_VOXEL_CASTLE_TERMINAL_MATTE_FIXTURES"));
  CastlePurpleBandFixtures(getenv("AR_VOXEL_CASTLE_PURPLE_BAND_FIXTURES"));
  CastleBackingFixtures(getenv("AR_VOXEL_CASTLE_BACKING_FIXTURES"));
  CastleWolfReliefFixtures(getenv("AR_VOXEL_CASTLE_WOLF_FIXTURES"));
  TombContourSkinFixtures(getenv("AR_VOXEL_TOMB_CONTOUR_FIXTURES"));
  CastleCapFinishFixtures(getenv("AR_VOXEL_CASTLE_CAP_FIXTURES"));
  CastlePostJoinFixtures(getenv("AR_VOXEL_CASTLE_POST_JOIN_FIXTURES"));
  SnowAct2SlopeFixtures(getenv("AR_VOXEL_SNOW_SLOPE_FIXTURES"));
  DesertSkeletonFixtures(getenv("AR_VOXEL_SKELETON_FIXTURES"));
  SurfaceFixtureEdits(getenv("AR_VOXEL_SURFACE_FIXTURE"));
  SurfaceRoleFixtures(getenv("AR_VOXEL_SURFACE_ROLE_FIXTURES"));
  SurfaceFlatRimFixture(getenv("AR_VOXEL_RIM_FIXTURE"));
  SurfaceTopMaterialFixtures(getenv("AR_VOXEL_TOP_FIXTURES"));
  SemanticGroupFixture(getenv("AR_VOXEL_GROUP_FIXTURE"));
  AuthoredVisibilityFixture(getenv("AR_VOXEL_VISIBILITY_FIXTURE"));
  AuthoredFixture(getenv("AR_VOXEL_AUTHORED_FIXTURE"));
  AuthoredStumpFixture(getenv("AR_VOXEL_AUTHORED_STUMP_FIXTURE"));
  AuthoredBranchFixture(getenv("AR_VOXEL_AUTHORED_STUMP_FIXTURE"));
  const unsigned fixtures=argc>1?Fixtures(argv[1]):0;
  if(argc>1)CastleColumnFixtures(argv[1]);
  if(argc>1)CastleAuditFixtures(argv[1]);
  const char *forest_directory=argc>2?argv[2]:getenv("AR_VOXEL_FOREST_FIXTURES");
  const unsigned forest=forest_directory?ForestFixtures(forest_directory):0;
  const unsigned caves=argc>2?CaveFixtures(argv[2]):0;
  const unsigned temples=argc>2?TempleFixtures(argv[2]):0;
  const char *temple_audit_directory=argc>2?argv[2]:getenv("AR_VOXEL_TEMPLE_AUDIT_FIXTURES");
  if(temple_audit_directory)TempleAuditFixtures(temple_audit_directory);
  const unsigned marsh=argc>3?MarshFixtures(argv[3]):0;
  if(argc>3)MarshAuditFixtures(argv[3]);
  const char *desert_directory=argc>4?argv[4]:getenv("AR_VOXEL_DESERT_FIXTURES");
  const unsigned desert=desert_directory?DesertFixtures(desert_directory):0;
  const unsigned tomb=argc>5?TombFixtures(argv[5]):0;
  const unsigned mountain=argc>6?MountainFixtures(argv[6]):0;
  const unsigned cavern=argc>7?CavernFixtures(argv[7]):0;
  const unsigned jungle=argc>8?JungleFixtures(argv[8]):0;
  const unsigned marahna_temple=argc>9?MarahnaTempleFixtures(argv[9]):0;
  const char *northwall_directory=getenv("AR_VOXEL_NORTHWALL_FIXTURES");
  const unsigned northwall=northwall_directory?NorthwallFixtures(northwall_directory):0;
  if(northwall)printf("Northwall: %u regional fixtures\n",northwall);
  const char *death_heim_directory=getenv("AR_VOXEL_DEATH_HEIM_FIXTURES");
  const unsigned death_heim=death_heim_directory?DeathHeimFixtures(death_heim_directory):0;
  if(death_heim)printf("Death Heim: %u regional fixtures\n",death_heim);
  printf("action_voxel_test: PASS (%u castle + %u forest + %u cave + %u temple/tower + %u marsh + %u desert + %u tomb + %u mountain + %u cavern + %u jungle + %u Marahna temple regional fixtures, immutable ownership, tile remapping, native gates, deck priority)\n",fixtures,forest,caves,temples,marsh,desert,tomb,mountain,cavern,jungle,marahna_temple);
  return 0;
}
