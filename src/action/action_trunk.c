#include "action_trunk.h"
#include "action/action_bg_metatile.h"
#include "diorama/diorama_layer_order.h"
#include "byte_order.h"
#include "snes_bgr555.h"
#include <string.h>

static unsigned Pixel(const ActionRoomScene *s, uint16_t word, int x, int y) {
  const unsigned tile=word&1023;
  if(tile>=512)return 0;
  x=(word&0x4000)?7-(x&7):(x&7);
  y=(word&0x8000)?7-(y&7):(y&7);
  const uint8_t *c=s->characters+tile*32;
  const unsigned shift=7-x;
  return ((c[y*2]>>shift)&1)|(((c[y*2+1]>>shift)&1)<<1)|
      (((c[y*2+16]>>shift)&1)<<2)|(((c[y*2+17]>>shift)&1)<<3);
}

static bool Sample(const ActionRoomScene *s,const DioramaRoomOverride *edits,
    int x,int y,uint16_t *word,unsigned *pixel) {
  uint8_t id=0;
  if(x<0||y<0||!ActionRoomScene_LookupTile(s,1,(unsigned)x/8,(unsigned)y/8,word,&id))return false;
  if(edits) {
    const DioramaTileStamp *stamp=DioramaLayerOrder_StampAt(&edits->stamp_layers[0],x/16,y/16);
    if(stamp) {
      if(stamp->blank)return false;
      *word=stamp->words[((y&15)/8)*2+(x&15)/8];id=stamp->metatile;
    }
    const DioramaPixelEdit *edit=DioramaLayerOrder_PixelEditAt(edits,0,x/16,y/16,id);
    if(edit) {
      const unsigned bit=1u<<(15-(x&15));
      if(edit->transparent[y&15]&bit)return false;
      if(edit->black[y&15]&bit){*pixel=1;return true;}
    }
  }
  *pixel=Pixel(s,*word,x,y);return true;
}

typedef struct TrunkSideProfile {
  uint64_t key;
  uint16_t rooms;
  uint8_t native_slope;
  uint16_t solid[4],rear[4],snow[4],snow_face[4];
} TrunkSideProfile;
#include "action_trunk_side_materials.inc"
typedef struct TrunkSideMaterial {
  uint16_t words[4],solid[4],rear[4],snow[4],snow_face[4];
  uint8_t pixels[256],collision,native_slope;
  uint64_t key;
} TrunkSideMaterial;

static uint64_t SideMaterialKey(const TrunkSideMaterial *m,unsigned flip) {
  uint64_t key=UINT64_C(14695981039346656037);
  for(unsigned q=0;q<4;++q) {
    const unsigned source=q^flip,w=m->words[source];
    const unsigned header[3]={(w>>10)&7,(m->collision>>source)&1,0};
    /* Native side artwork is low priority. Authoring a high word changes its
     * actor slot, not its structural identity; the priority mask records it. */
    for(unsigned i=0;i<3;++i)key=(key^header[i])*UINT64_C(1099511628211);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x) {
      const unsigned sx=(source&1)*8+((flip&1)?7-x:x);
      const unsigned sy=(source>>1)*8+((flip&2)?7-y:y);
      key=(key^m->pixels[sy*16+sx])*UINT64_C(1099511628211);
    }
  }
  return key;
}
static void SideMaterial(const ActionRoomScene *s,const uint16_t words[4],
    unsigned collision,TrunkSideMaterial *m) {
  memset(m,0,sizeof(*m));memcpy(m->words,words,sizeof(m->words));
  m->collision=(uint8_t)collision;
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
    m->pixels[y*16+x]=(uint8_t)Pixel(s,words[(y/8)*2+x/8],x,y);
  m->key=SideMaterialKey(m,0);
  for(unsigned flip=0;flip<4;++flip) {
    const uint64_t key=flip?SideMaterialKey(m,flip):m->key;
    unsigned lo=0,hi=sizeof(kTrunkSideProfiles)/sizeof(*kTrunkSideProfiles);
    while(lo<hi) {
      const unsigned mid=lo+(hi-lo)/2;
      if(kTrunkSideProfiles[mid].key<key)lo=mid+1;else hi=mid;
    }
    if(lo==sizeof(kTrunkSideProfiles)/sizeof(*kTrunkSideProfiles)||
        kTrunkSideProfiles[lo].key!=key||!(kTrunkSideProfiles[lo].rooms&(1u<<s->map)))continue;
    const TrunkSideProfile *p=&kTrunkSideProfiles[lo];
    for(unsigned q=0;q<4;++q) {
      m->solid[q]=p->solid[q^flip];m->rear[q]=p->rear[q^flip];
      m->snow[q]=p->snow[q^flip];
      m->snow_face[q]=p->snow_face[q^flip];
    }
    /* A vertical artwork reflection is not a native walking-ramp contract. */
    m->native_slope=p->native_slope&&!(flip&2);
    break;
  }
}
static unsigned SideNativeWords(const ActionRoomScene *s,unsigned id,uint16_t words[4]) {
  unsigned collision=0;
  for(unsigned q=0;q<4;++q) {
    const uint8_t *p=s->bg[0].metatiles+id*8+q*2;
    const unsigned raw=((unsigned)p[0]<<8)|p[1];
    collision|=!!(raw&0x200)<<q;
    words[q]=ActionBg_ComposeTilemapWord(raw,kActionRoomSceneTileWordMask,
        ActionRoomScene_BgAttributes(s,1));
  }
  return collision;
}
static bool SideCell(const ActionRoomScene *s,const DioramaRoomOverride *edits,
    const TrunkSideMaterial native[256],int cx,int cy,TrunkSideMaterial *out,
    const DioramaPixelEdit **pixels) {
  const DioramaStampLayerOverride *stamps=edits?&edits->stamp_layers[0]:NULL;
  if(stamps&&stamps->set_bounds&&(cx<stamps->x0||cy<stamps->y0||cx>=stamps->x1||cy>=stamps->y1))return false;
  uint16_t w;uint8_t id;
  if(cx<0||cy<0||!ActionRoomScene_LookupTile(s,1,(unsigned)cx*2,(unsigned)cy*2,&w,&id))return false;
  const DioramaTileStamp *stamp=stamps?DioramaLayerOrder_StampAt(stamps,cx,cy):NULL;
  if(stamp) {
    if(stamp->blank)return false;
    id=stamp->metatile;SideMaterial(s,stamp->words,native[id].collision,out);
  } else *out=native[id];
  if(pixels)*pixels=edits?DioramaLayerOrder_PixelEditAt(edits,0,cx,cy,id):NULL;
  return true;
}
static unsigned SideCollision(const ActionRoomScene *s,const DioramaRoomOverride *edits,
    const TrunkSideMaterial native[256],int cx,int cy) {
  TrunkSideMaterial m;
  return SideCell(s,edits,native,cx,cy,&m,NULL)?m.collision:0;
}
static bool BuildSides(ActionTrunkAssets *out,const ActionRoomScene *s,
    const DioramaRoomOverride *edits,const uint32_t palette[128]) {
  const unsigned width=ActionRoomScene_TileWidth(s,1)*8;
  if(!width||width>kActionTrunkSideWidth)return false;
  TrunkSideMaterial native[256];const TrunkSideMaterial *snow=NULL,*plain_snow=NULL;
  for(unsigned id=0;id<256;++id) {
    uint16_t words[4];const unsigned collision=SideNativeWords(s,id,words);
    SideMaterial(s,words,collision,&native[id]);
    if(native[id].key==kTrunkSideSnowKey)snow=&native[id];
    if(native[id].key==UINT64_C(0xf2bf3e50c5387e31))plain_snow=&native[id];
  }
  bool have_snow=snow||plain_snow;
  for(unsigned y=0;have_snow&&y<16;++y)for(unsigned x=0;x<16;++x) {
    const unsigned sx=snow?2+(x&7):x,sy=snow?2+(y&7):y;
    const TrunkSideMaterial *donor=snow?snow:plain_snow;
    const unsigned p=donor->pixels[sy*16+sx],w=donor->words[(sy/8)*2+sx/8];
    if(((w>>10)&7)!=4||p<13){have_snow=false;break;}
    out->side_snow[y*16+x]=palette[64+p];
  }
  /* Edited CHR may remove both reviewed snow donors. This cosmetic atlas
   * must not disable native priority or unrelated side geometry. In that
   * case the existing native bark atlas remains the visible cap fallback. */
  if(!have_snow)memset(out->side_snow,0,sizeof(out->side_snow));
  out->side_width=width;
  for(unsigned cy=0;cy<out->height/16;++cy)for(unsigned cx=0;cx<width/16;++cx) {
    if(cx>=20&&cx<44)continue;
    TrunkSideMaterial m;const DioramaPixelEdit *edit=NULL;
    if(!SideCell(s,edits,native,(int)cx,(int)cy,&m,&edit))continue;
    const unsigned left=SideCollision(s,edits,native,(int)cx-1,(int)cy);
    const unsigned right=SideCollision(s,edits,native,(int)cx+1,(int)cy);
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      const unsigned q=(y/8)*2+x/8,word=m.words[q],edit_bit=1u<<(15-x);
      if(edit&&(edit->transparent[y]&edit_bit))continue;
      const bool painted_black=edit&&(edit->black[y]&edit_bit);
      const unsigned p=painted_black?1:m.pixels[y*16+x];if(!p)continue;
      const unsigned bit=1u<<p,at=(cy*16+y)*kActionTrunkSideMaskPitch+cx*2+x/8,mask=1u<<(x&7);
      const bool high=!!(word&0x2000);
      bool solid=!painted_black&&!!(m.solid[q]&bit);
      bool rear=!painted_black&&!!(m.rear[q]&bit);
      if(solid&&m.native_slope&&(m.collision==6||m.collision==9)) {
        /* Match the native $90A9/$9084 adjacent-ramp rule, also used by the
         * generic voxel compiler. Fringe above contact is real rear art. */
        const unsigned phase=m.collision==6?(x&14):((~x)&14);
        const bool upper=m.collision==6?left==6:right==9;
        const bool lower=m.collision==6?right==6:left==9;
        const unsigned height=upper?8+phase/2:lower?phase/2:phase;
        if(y<16-height){solid=false;rear=true;}
      }
      if(solid)out->side_masks[kActionTrunkSideSolid][at]|=(uint8_t)mask;
      /* Explicit high artwork must keep its native actor occlusion even if
       * its low-priority source was an interior matte or rear decoration. */
      if(rear&&!high)out->side_masks[kActionTrunkSideRear][at]|=(uint8_t)mask;
      if(high) {
        out->side_masks[kActionTrunkSidePriority][at]|=(uint8_t)mask;
        ++out->side_high_pixels;
      }
      if(have_snow&&solid&&(m.snow[q]&bit)) {
        out->side_masks[kActionTrunkSideSnow][at]|=(uint8_t)mask;
        /* A pale icicle highlight is not a snow coating. Tall side art has
         * its own reviewed material/quarter contract, separate from the cap
         * family. The bank's native C12 outline is included, while blue
         * wood and icicle highlights retain their original side material. */
        if(m.snow_face[q]&bit)out->side_masks[kActionTrunkSideSnowFace][at]|=(uint8_t)mask;
      }
    }
  }
  return true;
}

bool ActionTrunk_Build(ActionTrunkAssets *out,const ActionRoomScene *s,const DioramaRoomOverride *edits) {
  if(!out||!s||!DioramaTrunk_Room(s->group,s->map)||!s->have_palette||
      !s->have_character_bank[0]||!s->have_character_bank[1])return false;
  const unsigned height=ActionRoomScene_TileHeight(s,1)*8;
  if(!height||height>kActionTrunkMaxHeight||ActionRoomScene_TileWidth(s,1)*8<704)return false;
  memset(out,0,sizeof(*out));out->room=s->map;out->height=height;
  uint32_t palette[128];
  for(unsigned i=0;i<128;++i) {
    const unsigned c=ByteOrder_ReadLe16(s->palette+i*2);
    palette[i]=0xff000000u|((uint32_t)ExpandColor5(c,15)<<16)|
        ((uint32_t)ExpandColor5(c>>5,15)<<8)|ExpandColor5(c>>10,15);
  }
  for(unsigned y=0;y<height;++y)for(int x=320;x<704;++x) {
    uint16_t w;unsigned v;if(!Sample(s,edits,x,(int)y,&w,&v))continue;
    const unsigned p=(w>>10)&7,at=y*48+(unsigned)(x-320)/8,bit=1u<<(x&7);
    if((x<368||x>=656)&&p==4&&v>=9)out->masks[0][at]|=bit;
    if(x>=368&&x<656&&(x<480||x>=544)&&p==5&&(w&1023)>=0x40&&
        ((v>=2&&v<=9)||v==13))out->masks[1][at]|=bit;
    if(x>=352&&x<672&&(p==6||((p==4||p==5)&&v==1)))out->masks[2][at]|=bit;
  }
  /* Reconstruct the repeating native lattice behind the column and branches.
   * Tie-break in map order, matching the original reconstruction. */
  for(unsigned ty=0;ty<8;++ty)for(unsigned tx=0;tx<40;++tx) {
    unsigned sx=tx+44;if(sx<46)sx=46+(sx&1);if(sx>=82)sx=80+(sx&1);
    if(sx>=60&&sx<68)sx=58+(sx&1);
    uint16_t words[32];unsigned counts[32]={0},count=0,best=0;
    for(unsigned yy=ty;yy<height/8;yy+=8) {
      uint16_t w;if(!ActionRoomScene_LookupTile(s,1,sx,yy,&w,NULL)||((w>>10)&7)!=6)continue;
      unsigned i=0;while(i<count&&words[i]!=w)++i;
      if(i==count){if(count==32)return false;words[count++]=w;}
      ++counts[i];
    }
    if(!count)return false;
    for(unsigned i=1;i<count;++i)if(counts[i]>counts[best])best=i;
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
      out->wall[(ty*8+y)*320+tx*8+x]=palette[96+Pixel(s,words[best],(int)x,(int)y)];
  }
  for(int y=0;y<32;++y)for(int x=0;x<32;++x) {
    uint16_t w;unsigned v;
    if(!Sample(s,NULL,320+x,384+y,&w,&v)||((w>>10)&7)!=4||v<9)return false;
    out->bark[y*32+x]=palette[64+v];
  }
  /* An unsupported side source may retain its old flat presentation; it must
   * never disable the already established connected central enclosure. */
  (void)BuildSides(out,s,edits,palette);
  uint32_t hash=2166136261u;
  const uint8_t *bytes=(const uint8_t *)out;
  for(size_t i=offsetof(ActionTrunkAssets,masks);i<sizeof(*out);++i)hash=(hash^bytes[i])*16777619u;
  out->revision=hash;return true;
}

void ActionTrunk_Split(const ActionTrunkAssets *a,int ox,int oy,int width,int height,int pitch,
    const uint32_t *const input[3],const size_t input_pitch[3],uint32_t *const output[3],unsigned brightness) {
  for(unsigned band=0;band<3;++band) {
    memset(output[band],0,(size_t)pitch*height*4);
  }
  /* Ordinary virtual bands are authoring hints, not enclosure geometry. Join
   * them before applying the native silhouette masks; a saved far/high rule
   * must not remove pixels from the trunk's material. */
  for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
    const int at=y*pitch+x;
    uint32_t pixel=input[2]?input[2][y*input_pitch[2]+x]:0;
    if(input[0]&&(input[0][y*input_pitch[0]+x]>>24))pixel=input[0][y*input_pitch[0]+x];
    if(input[1]&&(input[1][y*input_pitch[1]+x]>>24))pixel=input[1][y*input_pitch[1]+x];
    output[0][at]=pixel;
  }
  for(int y=0;y<height;++y) {
    const int wy=y+oy;if(wy<0||(unsigned)wy>=a->height)continue;
    const int first=320-ox>0?320-ox:0,last=704-ox<width?704-ox:width;
    const int wall_first=352-ox>0?352-ox:0,wall_last=672-ox<width?672-ox:width;
    if(wall_first<wall_last)memcpy(output[2]+y*pitch+wall_first,
        a->wall+(wy&63)*320+wall_first+ox-352,(size_t)(wall_last-wall_first)*4);
    if(brightness<15)for(int x=wall_first;x<wall_last;++x) {
      const uint32_t pixel=output[2][y*pitch+x];uint32_t faded=pixel&0xff000000u;
      for(unsigned shift=0;shift<=16;shift+=8)faded|=((((pixel>>shift)&255)*brightness+7)/15)<<shift;
      output[2][y*pitch+x]=faded;
    }
    for(int x=first;x<last;++x) {
      const int wx=x+ox,at=y*pitch+x;
      if(ActionTrunk_Solid(a,wx,wy,0)){output[1][at]=output[0][at];output[0][at]=0;}
      if(ActionTrunk_Solid(a,wx,wy,2))output[0][at]=output[1][at]=0;
    }
    for(int x=0;x<width;++x) {
      const int wx=x+ox,at=y*pitch+x;
      if(wx>=320&&wx<704)continue;
      const uint32_t pixel=output[0][at];
      if(ActionTrunk_SideSolid(a,wx,wy,kActionTrunkSideRear)) {
        output[2][at]=pixel;output[0][at]=0;
      } else if(ActionTrunk_SideSolid(a,wx,wy,kActionTrunkSideSolid)) {
        output[2][at]=pixel;
      }
      if(ActionTrunk_SideSolid(a,wx,wy,kActionTrunkSidePriority)) {
        output[1][at]=output[0][at];output[0][at]=0;
      }
    }
  }
}
