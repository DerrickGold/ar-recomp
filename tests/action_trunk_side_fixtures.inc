/* Native side-room geometry regressions.
 * No ROM pixels are stored here: all source art comes from the six .arscenes.
 * Native collision formulas are $00:9084/$90A9, not renderer policy flags. */
#include "action/action_trunk.h"
#include "action/action_scene_snapshot.h"
#include "diorama/diorama_layer_order.h"
#include "byte_order.h"
#include "snes_bgr555.h"
#include "support/test_assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Pixel TrunkLegacyPixel
#define Sample TrunkLegacySample
#define ActionTrunk_Build TrunkLegacyBuild
#define ActionTrunk_Split TrunkLegacySplit
#include "action_trunk_legacy.inc"
#undef ActionTrunk_Split
#undef ActionTrunk_Build
#undef Sample
#undef Pixel

static unsigned TrunkTestPixel(const ActionRoomScene *s,uint16_t word,int x,int y) {
  const unsigned tile=word&1023;if(tile>=512)return 0;
  const unsigned px=(word&0x4000)?7-(x&7):(x&7),py=(word&0x8000)?7-(y&7):(y&7);
  const uint8_t *chr=s->characters+tile*32;
  unsigned value=0;
  for(unsigned plane=0;plane<4;++plane)
    value|=((chr[py*2+(plane&1)+(plane/2)*16]>>(7-px))&1u)<<plane;
  return value;
}
static bool TrunkTestSample(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    int x,int y,uint16_t *word,unsigned *pixel,uint8_t *id) {
  if(x<0||y<0||!ActionRoomScene_LookupTile(s,1,x/8,y/8,word,id))return false;
  if(terrain) {
    const DioramaTileStamp *stamp=DioramaLayerOrder_StampAt(&terrain->stamp_layers[0],x/16,y/16);
    if(stamp) {
      if(stamp->blank)return false;
      *word=stamp->words[(y%16/8)*2+x%16/8];*id=stamp->metatile;
    }
    const DioramaPixelEdit *edit=DioramaLayerOrder_PixelEditAt(terrain,0,x/16,y/16,*id);
    if(edit) {
      const unsigned bit=1u<<(15-(x&15));
      if(edit->transparent[y&15]&bit)return false;
      if(edit->black[y&15]&bit){*pixel=1;return true;}
    }
  }
  *pixel=TrunkTestPixel(s,*word,x,y);return true;
}
static uint32_t TrunkTestColor(const ActionRoomScene *s,uint16_t word,unsigned ink,unsigned brightness) {
  if(!ink)return 0;
  const unsigned c=ByteOrder_ReadLe16(s->palette+((((word>>10)&7)*16+ink)*2));
  return 0xff000000u|(uint32_t)ExpandColor5(c,brightness)<<16|
      (uint32_t)ExpandColor5(c>>5,brightness)<<8|ExpandColor5(c>>10,brightness);
}
static unsigned TrunkTestCollision(const ActionRoomScene *s,const DioramaRoomOverride *terrain,int x,int y) {
  uint16_t word;uint8_t id;unsigned pixel;
  if(!TrunkTestSample(s,terrain,x*16,y*16,&word,&pixel,&id))return 0;
  unsigned value=0;
  for(unsigned q=0;q<4;++q)value|=!!(s->bg[0].metatiles[id*8+q*2]&2)<<q;
  return value;
}
static unsigned TrunkTestContact(unsigned collision,unsigned left,unsigned right,unsigned x) {
  assert(collision==6||collision==9);
  unsigned height=(collision==6?x:~x)&14;
  if((collision==6?left:right)==collision)height=8+height/2;
  else if((collision==6?right:left)==collision)height/=2;
  return 16-height;
}
static void TrunkTestCoreEqual(const ActionTrunkAssets *old,const ActionTrunkAssets *now) {
  assert(old->room==now->room && old->height==now->height);
  assert(!memcmp(old->masks,now->masks,sizeof(old->masks)));
  assert(!memcmp(old->wall,now->wall,sizeof(old->wall)));
  assert(!memcmp(old->bark,now->bark,sizeof(old->bark)));
}
static bool TrunkTestFind(const ActionRoomScene *s,unsigned wanted,int *x,int *y) {
  for(unsigned cy=0;cy<ActionRoomScene_TileHeight(s,1)/2;++cy)
    for(unsigned cx=0;cx<ActionRoomScene_TileWidth(s,1)/2;++cx) {
      if(cx>=20&&cx<44)continue;
      uint16_t word;uint8_t id;assert(ActionRoomScene_LookupTile(s,1,cx*2,cy*2,&word,&id));
      if(id==wanted){*x=(int)cx;*y=(int)cy;return true;}
    }
  return false;
}
static DioramaTileStamp TrunkTestStamp(const ActionRoomScene *s,int from_x,int from_y,int x,int y) {
  DioramaTileStamp stamp={.x=x,.y=y};
  for(unsigned q=0;q<4;++q)assert(ActionRoomScene_LookupTile(s,1,
      from_x*2+(q&1),from_y*2+(q>>1),&stamp.words[q],&stamp.metatile));
  return stamp;
}
static void TrunkTestReflect(DioramaTileStamp *stamp,unsigned flip) {
  uint16_t words[4];memcpy(words,stamp->words,sizeof(words));
  for(unsigned q=0;q<4;++q)stamp->words[q]=words[q^flip]^((flip&1)?0x4000:0)^((flip&2)?0x8000:0);
}
static void TrunkTestSideEqual(const ActionTrunkAssets *a,const ActionTrunkAssets *b) {
  assert(a->side_width==b->side_width && a->height==b->height);
  assert(!memcmp(a->side_masks,b->side_masks,sizeof(a->side_masks)));
  assert(!memcmp(a->side_snow,b->side_snow,sizeof(a->side_snow)));
}
static void TrunkTestRemap(ActionRoomScene *s) {
  /* The room also contains animated artwork. Keep each animation phase in
   * its original payload window and apply the same permutation inside every
   * phase. Static CHR rotate separately, so neither the hardware target nor
   * the native phase/cadence metadata is falsified by renumbering. */
  static ActionRoomScene before;before=*s;
  uint8_t characters[sizeof(s->characters)],definitions[kActionRoomSceneMetatileBytes];
  const unsigned stride=ActionRoomScene_HasCharacterAnimation(s)
      ?ActionRoomScene_CharacterAnimationStride(s)/32:0;
  const unsigned phases=stride?ActionRoomScene_CharacterAnimationPhaseCount(s):0;
  const unsigned animated=stride*phases;assert(animated<256);
  unsigned remap[256];
  for(unsigned tile=0;tile<256;++tile)
    remap[tile]=tile<animated?tile/stride*stride+(tile+37)%stride:
        animated+(tile-animated+37)%(256-animated);
  memcpy(characters,s->characters,sizeof(characters));
  for(unsigned bank=0;bank<2;++bank)for(unsigned tile=0;tile<256;++tile)
    memcpy(s->characters+(bank*256+remap[tile])*32,characters+(bank*256+tile)*32,32);
  for(unsigned layer=0;layer<2;++layer) {
    memcpy(definitions,s->bg[layer].metatiles,sizeof(definitions));
    for(unsigned id=0;id<256;++id) {
      const unsigned next=(id+73)%256;memcpy(s->bg[layer].metatiles+next*8,definitions+id*8,8);
      for(unsigned q=0;q<4;++q)s->bg[layer].metatiles[next*8+q*2+1]=remap[definitions[id*8+q*2+1]];
    }
    for(size_t i=0;i<s->bg[layer].map_size;++i)s->bg[layer].map[i]=(s->bg[layer].map[i]+73)%256;
  }
  assert(!memcmp(s->video_profile,before.video_profile,sizeof(s->video_profile)));
  uint8_t resolved_before[kActionRoomSceneCharacterBytes],resolved_after[kActionRoomSceneCharacterBytes];
  for(unsigned phase=0;phase<(phases?phases:1);++phase) {
    assert(ActionRoomScene_BuildCharacters(&before,0,(int)phase,resolved_before,sizeof(resolved_before)));
    assert(ActionRoomScene_BuildCharacters(s,0,(int)phase,resolved_after,sizeof(resolved_after)));
    for(unsigned bank=0;bank<2;++bank)for(unsigned tile=0;tile<256;++tile)
      assert(!memcmp(resolved_before+(bank*256+tile)*32,
          resolved_after+(bank*256+remap[tile])*32,32));
  }
  printf("trunk remap room06/%02u: %u native phases, stride%u bytes, target%04x; all resolved CHR preserved\n",
      s->map,phases,stride*32,ActionRoomScene_CharacterAnimationTarget(s));
}
static void TrunkTestSplit(const ActionRoomScene *s,const DioramaRoomOverride *terrain,
    const ActionTrunkAssets *old,const ActionTrunkAssets *now,unsigned brightness,bool whole_room) {
  enum {width=1056,height=32,pitch=1063};
  uint32_t *storage=calloc(9u*pitch*height,sizeof(uint32_t));assert(storage);
  uint32_t *source[3],*actual[3],*legacy[3];const uint32_t *input[3];
  const size_t input_pitch[3]={pitch,pitch,pitch};
  for(unsigned b=0;b<3;++b){input[b]=source[b]=storage+b*pitch*height;
    actual[b]=storage+(3+b)*pitch*height;legacy[b]=storage+(6+b)*pitch*height;}
  const int last=whole_room?(int)now->height+16:48;
  for(int oy=-16;oy<last;oy+=height) {
    memset(storage,0,3u*pitch*height*sizeof(*storage));
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
      uint16_t word;uint8_t id;unsigned ink;
      if(!TrunkTestSample(s,terrain,x-16,y+oy,&word,&ink,&id))continue;
      /* Virtual-band assignment is deliberately unrelated to native priority. */
      source[(x+y)%3][y*pitch+x]=TrunkTestColor(s,word,ink,brightness);
    }
    ActionTrunk_Split(now,-16,oy,width,height,pitch,input,input_pitch,actual,brightness);
    TrunkLegacySplit(old,-16,oy,width,height,pitch,input,input_pitch,legacy,brightness);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
      const int wx=x-16,wy=y+oy,at=y*pitch+x;
      if(wx>=320&&wx<704) {
        for(unsigned b=0;b<3;++b)assert(actual[b][at]==legacy[b][at]);
        continue;
      }
      uint32_t color=source[2][at];
      if(source[0][at]>>24)color=source[0][at];
      if(source[1][at]>>24)color=source[1][at];
      const bool solid=ActionTrunk_SideSolid(now,wx,wy,kActionTrunkSideSolid);
      const bool rear=ActionTrunk_SideSolid(now,wx,wy,kActionTrunkSideRear);
      const bool high=ActionTrunk_SideSolid(now,wx,wy,kActionTrunkSidePriority);
      assert(actual[0][at]==(rear||high?0:color));
      assert(actual[1][at]==(!rear&&high?color:0));
      assert(actual[2][at]==(solid||rear?color:0));
    }
    for(unsigned b=0;b<3;++b)for(int y=0;y<height;++y)for(int x=width;x<pitch;++x)
      assert(!actual[b][y*pitch+x]);
  }
  free(storage);
}
static bool TrunkTestNativeBlueWood(unsigned id) {
  /* Fixture specimens from the reviewed original source, not a runtime
   * classification rule. The remap test separately relocates every ID. */
  return id==0x2e || id==0x2f || id==0x38 || id==0x39 || id==0x3a ||
      id==0x3d || id==0x40 || id==0x47 ||
      id==0xbc || id==0xbf || id==0xc4 || id==0xc7 ||
      (id>=0xf7 && id<=0xff);
}
static void TrunkTestNative(const ActionRoomScene *s,const ActionTrunkAssets *a,unsigned region) {
  unsigned solid_count=0,snow_count=0,snow_face_count=0,rear_count=0,detail_count=0,sky_count=0,slopes=0,
      contacts=0,contact_mattes=0,contact_outside=0,outline12=0,mixed_dark_body=0,right_wall_outline=0,
      icicle_native_light=0,icicle_solid=0,bank_surface=0,transition_light=0;
  const unsigned w=ActionRoomScene_TileWidth(s,1)*8,h=ActionRoomScene_TileHeight(s,1)*8;
  int8_t snow_family[256];memset(snow_family,-1,sizeof(snow_family));
  assert(a->side_width==w&&a->height==h);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
    const bool core=x>=320&&x<704;
    const bool solid=ActionTrunk_SideSolid(a,x,y,kActionTrunkSideSolid),
        rear=ActionTrunk_SideSolid(a,x,y,kActionTrunkSideRear),
        snow=ActionTrunk_SideSolid(a,x,y,kActionTrunkSideSnow),
        snow_face=ActionTrunk_SideSolid(a,x,y,kActionTrunkSideSnowFace),
        high=ActionTrunk_SideSolid(a,x,y,kActionTrunkSidePriority);
    if(core){assert(!solid&&!rear&&!snow&&!snow_face&&!high);continue;}
    uint16_t word;uint8_t id;unsigned ink;assert(TrunkTestSample(s,NULL,x,y,&word,&ink,&id));
    assert(!(solid&&rear));assert(!snow||solid);
    assert(high==!!(ink&&(word&0x2000)));
    if(!ink){assert(!solid&&!rear&&!snow&&!snow_face&&!high);++sky_count;continue;}
    /* Structural side geometry must never consume native green foliage or
     * opaque black matte. This is an independent color-role safety limit,
     * not sufficient by itself to classify a snow/bark tile as structural. */
    if(solid)assert(((word>>10)&7)==4&&ink>=8);
    /* A snow face can contain shaded blue/brown pixels. Its material is
     * identified independently by actual pale snow ink elsewhere in the
     * complete native motif, not by the brightness of this one cap pixel.
     * The native palette's bright snow colors have all BGR555 channels>=24;
     * dark bark-only motifs do not. No production profile table is read. */
    if(snow_family[id]<0) {
      snow_family[id]=0;
      for(unsigned py=0;py<16;++py)for(unsigned px=0;px<16;++px) {
        uint16_t sw;uint8_t sid;unsigned sp;
        assert(TrunkTestSample(s,NULL,(x/16)*16+px,(y/16)*16+py,&sw,&sp,&sid));
        if(((sw>>10)&7)!=4||!sp)continue;
        const unsigned color=ByteOrder_ReadLe16(s->palette+(64+sp)*2);
        if((color&31)>=24&&((color>>5)&31)>=24&&((color>>10)&31)>=24)snow_family[id]=1;
      }
    }
    if(snow)assert(snow_family[id]);
    /* A vertical snow face requires source light-snow ink, but that color
     * alone is insufficient: pale highlights on native blue icicles are not
     * snowbank walls. Check the independently reviewed positive and negative
     * source families below without consulting the production table. */
    if(snow_face)assert(solid&&snow&&((word>>10)&7)==4&&ink>=12);
    outline12+=snow_face&&ink==12;
    mixed_dark_body+=solid&&snow&&ink>=9&&ink<=11&&!snow_face;
    if(s->map==6&&x/16==63&&y/16>=23&&y/16<=26&&ink==12) {
      assert(snow_face);++right_wall_outline;
    }
    if(s->map==6&&(x/16==61||x/16==62)&&y/16==27&&solid&&ink>=12) {
      assert(snow_face);++bank_surface;
    }
    if(TrunkTestNativeBlueWood(id)) {
      assert(!snow_face);
      icicle_solid+=solid;
      icicle_native_light+=solid&&((word>>10)&7)==4&&ink>=12;
    }
    if((id==0x51||id==0x53)&&(y&15)>=8) {
      assert(!snow_face);transition_light+=solid&&ink>=12;
    }
    solid_count+=solid;snow_count+=snow;snow_face_count+=snow_face;
    rear_count+=rear;detail_count+=!solid&&!rear;
  }
  for(unsigned y=0;y<h/16;++y)for(unsigned x=0;x<w/16;++x) {
    if(x>=20&&x<44)continue;
    const unsigned c=TrunkTestCollision(s,NULL,x,y);if(c!=6&&c!=9)continue;
    const unsigned l=TrunkTestCollision(s,NULL,(int)x-1,y),r=TrunkTestCollision(s,NULL,x+1,y);++slopes;
    for(unsigned px=0;px<16;++px) {
      const unsigned contact=TrunkTestContact(c,l,r,px);
      uint16_t word;uint8_t id;unsigned ink;
      if(!TrunkTestSample(s,NULL,x*16+px,y*16+contact,&word,&ink,&id)){++contact_outside;continue;}
      if(((word>>10)&7)!=4||ink<9){++contact_mattes;continue;}
      if(!ActionTrunk_SideSolid(a,x*16+px,y*16+contact,kActionTrunkSideSolid))
        fprintf(stderr,"missing side contact region%u room06/%02u cell%u,%u pixel%u,%u ink%u\n",region,s->map,x,y,px,contact,ink);
      assert(ActionTrunk_SideSolid(a,x*16+px,y*16+contact,kActionTrunkSideSolid));++contacts;
      /* Visible ink above the physics contour remains native rear art;
       * it must not create the old full-cell walking shelves. */
      for(unsigned py=0;py<contact&&py<16;++py)
        assert(!ActionTrunk_SideSolid(a,x*16+px,y*16+py,kActionTrunkSideSolid));
    }
  }
  assert(solid_count&&snow_count&&snow_face_count&&rear_count&&sky_count&&slopes&&contacts);
  assert(outline12&&mixed_dark_body);
  if(s->map==6)assert(right_wall_outline&&bank_surface&&icicle_solid&&icicle_native_light&&transition_light);
  /* All shipped native side motifs now have a reviewed role, including the
   * decorative foliage and its matte. Unknown edited artwork is still tested
   * separately below and must retain its flat native-priority fallback. */
  assert(!detail_count);
  for(unsigned kind=0;kind<kActionTrunkSideMaskCount;++kind) {
    assert(!ActionTrunk_SideSolid(a,-1,0,kind)&&!ActionTrunk_SideSolid(a,w,0,kind));
    assert(!ActionTrunk_SideSolid(a,0,-1,kind)&&!ActionTrunk_SideSolid(a,0,h,kind));
  }
  printf("trunk side native %u/06%02u: solid%u snow%u snowface%u rear%u detail%u sky%u; %u slope cells, %u opaque contacts, %u native-matte contacts, %u finite-room endpoints\n",
      region,s->map,solid_count,snow_count,snow_face_count,rear_count,detail_count,sky_count,slopes,contacts,contact_mattes,contact_outside);
}
static void TrunkTestEdits(const ActionRoomScene *s,const ActionTrunkAssets *base,ActionTrunkAssets *edited) {
  static DioramaRoomOverride terrain;int sx,sy;
  /* Mixed green/snow boundary, not a uniform white block: reflected role
   * masks must match actual reflected source pixels and exclude foliage. */
  assert(TrunkTestFind(s,0x70,&sx,&sy));
  DioramaTileStamp stamp=TrunkTestStamp(s,sx,sy,2,2),original=stamp;
  memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0].cells=&stamp;terrain.stamp_layers[0].count=1;
  for(unsigned flip=0;flip<4;++flip) {
    stamp=original;TrunkTestReflect(&stamp,flip);assert(ActionTrunk_Build(edited,s,&terrain));
    for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)for(unsigned kind=0;kind<kActionTrunkSideMaskCount;++kind)
      assert(ActionTrunk_SideSolid(edited,32+x,32+y,kind)==
          ActionTrunk_SideSolid(base,sx*16+((flip&1)?15-x:x),sy*16+((flip&2)?15-y:y),kind));
  }
  stamp=original;for(unsigned q=0;q<4;++q)stamp.words[q]|=0x2000;
  assert(ActionTrunk_Build(edited,s,&terrain));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    uint16_t word;uint8_t id;unsigned ink;assert(TrunkTestSample(s,&terrain,32+x,32+y,&word,&ink,&id));
    assert(ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSidePriority)==!!ink);
    assert(ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSideSolid)==
        ActionTrunk_SideSolid(base,sx*16+x,sy*16+y,kActionTrunkSideSolid));
  }
  TrunkTestSplit(s,&terrain,base,edited,7,false);
  /* Erasing authored ink removes all corresponding side ownership; black
   * paint cannot be reinterpreted as a snow/wood cap. Clearing the edit must
   * recover the same complete mask rather than retain stale holes. */
  terrain.pixel_layers[0].count=1;terrain.pixel_layers[0].edits[0]=(DioramaPixelEdit){.by_cell=true,.x=2,.y=2};
  terrain.pixel_layers[0].edits[0].transparent[8]=0xffff;
  terrain.pixel_layers[0].edits[0].black[9]=0xffff;
  assert(ActionTrunk_Build(edited,s,&terrain));
  for(unsigned x=0;x<16;++x) {
    for(unsigned kind=0;kind<kActionTrunkSideMaskCount;++kind)assert(!ActionTrunk_SideSolid(edited,32+x,40,kind));
    assert(!ActionTrunk_SideSolid(edited,32+x,41,kActionTrunkSideSolid));
    assert(!ActionTrunk_SideSolid(edited,32+x,41,kActionTrunkSideSnow));
    assert(!ActionTrunk_SideSolid(edited,32+x,41,kActionTrunkSideSnowFace));
  }
  terrain.pixel_layers[0].count=0;stamp=original;
  assert(ActionTrunk_Build(edited,s,&terrain));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)for(unsigned kind=0;kind<kActionTrunkSideMaskCount;++kind)
    assert(ActionTrunk_SideSolid(edited,32+x,32+y,kind)==ActionTrunk_SideSolid(base,sx*16+x,sy*16+y,kind));
  stamp.blank=true;assert(ActionTrunk_Build(edited,s,&terrain));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)for(unsigned kind=0;kind<kActionTrunkSideMaskCount;++kind)
    assert(!ActionTrunk_SideSolid(edited,32+x,32+y,kind));
  /* Unknown art retains its source-native priority instead of being dropped
   * into the low band or automatically modeled by matching its palette. */
  static ActionRoomScene unknown;unknown=*s;
  stamp=original;for(unsigned q=0;q<4;++q)stamp.words[q]|=0x2000;
  const unsigned changed=stamp.words[0]&1023;unknown.characters[changed*32]^=0x80;
  assert(ActionTrunk_Build(edited,&unknown,&terrain));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    uint16_t word;uint8_t id;unsigned ink;assert(TrunkTestSample(&unknown,&terrain,32+x,32+y,&word,&ink,&id));
    if(ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSidePriority)!=!!ink)
      fprintf(stderr,"unknown side high room%u source%d,%d modifiedCHR%u word%04x id%02x pixel%u,%u ink%u sidewidth%u highpixels%u\n",
          s->map,sx,sy,changed,word,id,x,y,ink,edited->side_width,edited->side_high_pixels);
    assert(ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSidePriority)==!!ink);
    assert(!ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSideSolid));
    assert(!ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSideRear));
    assert(!ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSideSnow));
    assert(!ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSideSnowFace));
  }
  for(size_t i=0;i<sizeof(edited->side_masks[0]);++i) {
    assert(!edited->side_masks[kActionTrunkSideSnow][i]);
    assert(!edited->side_masks[kActionTrunkSideSnowFace][i]);
  }
  bool retained_body=false;
  for(unsigned cy=0;cy<base->height/16&&!retained_body;++cy)
    for(unsigned cx=0;cx<base->side_width/16&&!retained_body;++cx) {
      if((cx>=20&&cx<44)||(cx==2&&cy==2))continue;
      bool same_source=true,has_body=false;
      for(unsigned q=0;q<4;++q) {
        uint16_t word;assert(ActionRoomScene_LookupTile(s,1,cx*2+(q&1),cy*2+(q>>1),&word,NULL));
        same_source&=(word&1023)!=changed;
      }
      if(!same_source)continue;
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        has_body|=ActionTrunk_SideSolid(base,cx*16+x,cy*16+y,kActionTrunkSideSolid);
      if(!has_body)continue;
      for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        for(unsigned kind=0;kind<kActionTrunkSideSnow;++kind)
          assert(ActionTrunk_SideSolid(edited,cx*16+x,cy*16+y,kind)==
              ActionTrunk_SideSolid(base,cx*16+x,cy*16+y,kind));
      retained_body=true;
    }
  assert(retained_body); /* Losing an optional atlas cannot disable other cells. */
  /* Promoting genuine rear foliage to native High must preserve its authored
   * actor-occlusion slot, not silently move it back to Far. */
  assert(TrunkTestFind(s,0x64,&sx,&sy));stamp=TrunkTestStamp(s,sx,sy,2,2);
  for(unsigned q=0;q<4;++q)stamp.words[q]|=0x2000;
  assert(ActionTrunk_Build(edited,s,&terrain));
  for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
    assert(ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSidePriority));
    assert(!ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSideRear));
    assert(!ActionTrunk_SideSolid(edited,32+x,32+y,kActionTrunkSideSolid));
  }
  TrunkTestSplit(s,&terrain,base,edited,15,false);
}
static void TrunkTestSlopeStamps(const ActionRoomScene *s,ActionTrunkAssets *edited) {
  static DioramaRoomOverride terrain;DioramaTileStamp stamps[2];
  int sx=-1,sy=-1;
  for(unsigned y=0;y<ActionRoomScene_TileHeight(s,1)/2&&sx<0;++y)
    for(unsigned x=0;x+1<ActionRoomScene_TileWidth(s,1)/2;++x) {
      if(x>=19&&x<44)continue;
      if(TrunkTestCollision(s,NULL,x,y)==6&&TrunkTestCollision(s,NULL,x+1,y)==6) {
        sx=(int)x;sy=(int)y;break;
      }
    }
  assert(sx>=0);
  stamps[0]=TrunkTestStamp(s,sx,sy,2,2);stamps[1]=TrunkTestStamp(s,sx+1,sy,3,2);
  memset(&terrain,0,sizeof(terrain));terrain.stamp_layers[0].cells=stamps;terrain.stamp_layers[0].count=2;
  for(unsigned isolated=0;isolated<2;++isolated) {
    stamps[1].blank=!!isolated;assert(ActionTrunk_Build(edited,s,&terrain));
    for(unsigned cell=0;cell<(isolated?1u:2u);++cell)for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
      uint16_t word;uint8_t id;unsigned ink;
      assert(TrunkTestSample(s,&terrain,(2+cell)*16+x,32+y,&word,&ink,&id));
      const unsigned contact=TrunkTestContact(6,cell?6:0,isolated||cell?0:6,x);
      const bool structural=((word>>10)&7)==4&&ink>=9&&y>=contact;
      if(ActionTrunk_SideSolid(edited,(2+cell)*16+x,32+y,kActionTrunkSideSolid)!=structural)
        fprintf(stderr,"stamped slope room%u pair%d source%d,%d cell%u pixel%u,%u ink%u contact%u\n",
            s->map,!isolated,sx,sy,cell,x,y,ink,contact);
      assert(ActionTrunk_SideSolid(edited,(2+cell)*16+x,32+y,kActionTrunkSideSolid)==structural);
    }
  }
}
static void TrunkSideFixtures(const char *directory) {
  if(!directory)return;
  static ActionSceneSnapshot snapshot;static ActionRoomScene original,remapped;
  static uint8_t encoded[kActionSceneSnapshotMaxBytes];
  ActionTrunkAssets *base=malloc(sizeof(*base)),*legacy=malloc(sizeof(*legacy)),*edited=malloc(sizeof(*edited));
  assert(base&&legacy&&edited);
  for(unsigned region=0;region<3;++region)for(unsigned room=6;room<=7;++room) {
    char path[1024];snprintf(path,sizeof(path),"%s/%u/06%02u.arscene",directory,region,room);
    FILE *file=fopen(path,"rb");assert(file);const size_t size=fread(encoded,1,sizeof(encoded),file);
    assert(!ferror(file));fclose(file);assert(ActionSceneSnapshot_Decode(encoded,size,&snapshot));original=snapshot.scene;
    const ActionRoomScene *s=&snapshot.scene;assert(s->group==6&&s->map==room);
    assert(TrunkLegacyBuild(legacy,s,NULL));assert(ActionTrunk_Build(base,s,NULL));TrunkTestCoreEqual(legacy,base);
    /* Historical witness: the old production output owns no outer geometry. */
    assert(!legacy->side_width);for(size_t i=0;i<sizeof(legacy->side_masks);++i)assert(!((const uint8_t *)legacy->side_masks)[i]);
    TrunkTestNative(s,base,region);
    TrunkTestSplit(s,NULL,legacy,base,15,true);TrunkTestSplit(s,NULL,legacy,base,7,false);TrunkTestSplit(s,NULL,legacy,base,0,false);
    remapped=*s;TrunkTestRemap(&remapped);assert(ActionTrunk_Build(edited,&remapped,NULL));TrunkTestSideEqual(base,edited);
    TrunkTestEdits(s,base,edited);TrunkTestSlopeStamps(s,edited);
    assert(!memcmp(s,&original,sizeof(original)));
  }
  free(base);free(legacy);free(edited);puts("trunk side six-region fixtures: PASS");
}
