#include "action_trunk.h"
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
  }
}
