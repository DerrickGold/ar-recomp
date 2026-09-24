#include "actraiser/actraiser_actor_art.h"
#include "actraiser/actraiser_lzss.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ActRaiserLzssObserver observer;
static void *observer_context;
static bool requested,accept=true;
static unsigned captures;
void ActRaiserLzss_SetObserver(ActRaiserLzssObserver next,void *context) {
  observer=next;observer_context=context;
}
bool ActRaiserRegional_ActorArtwork(unsigned area,bool activate,bool *enabled) {
  assert(area<7 && enabled);
  if(!accept)return false;
  if(activate)++captures;
  *enabled=requested;return true;
}
static void Write16(uint8_t *p,unsigned value) {p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);}
static void Write32(uint8_t *p,unsigned value) {Write16(p,value);Write16(p+2,value>>16);}
static void Record(uint8_t *record,unsigned kind,unsigned offset,unsigned size) {
  Write16(record,0x0202);record[2]=(uint8_t)kind;
  Write32(record+4,offset);Write32(record+8,size);
}
static ArRegionalMediaBytes ReadFixture(const char *path,size_t maximum) {
  FILE *file=fopen(path,"rb");assert(file);
  assert(!fseek(file,0,SEEK_END));const long size=ftell(file);
  assert(size>0 && (size_t)size<=maximum && !fseek(file,0,SEEK_SET));
  uint8_t *bytes=malloc((size_t)size);assert(bytes);
  assert(fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
  return (ArRegionalMediaBytes){bytes,(size_t)size};
}
static unsigned Read16(const uint8_t *p) {return p[0]|(unsigned)p[1]<<8;}
static void CheckNative(unsigned source,unsigned base,const char *native_path,const char *donor_path) {
  ArRegionalMediaBytes native=ReadFixture(native_path,65536),packet=ReadFixture(donor_path,1048576);
  ArRegionalActorArtView donor;assert(ArRegionalActorArt_Parse(packet,&donor));
  ActRaiserActorArt_Initialize(&donor);assert(observer);
  requested=true;ActRaiserActorArt_BeginRoom(0x0101);
  const ArRegionalActorArtBinding *chr=ArRegionalActorArt_Binding(0x0101,kArRegionalActorArt_Characters,0);
  ArRegionalMediaBytes pixels;assert(ActRaiserActorArt_Upload(0x0101,kArRegionalActorArt_Characters,0,chr->source,&pixels));
  const unsigned snes=(source>>15)<<16|(source&0x7fff)|0x8000;
  observer(observer_context,snes+2,(uint16_t)base,native.data,native.size);
  const unsigned table=Read16(native.data),count=(Read16(native.data+table)-table)/2;
  for(unsigned v=0;v<count;++v) {
    const unsigned composition=base+Read16(native.data+table+2*v);
    const ArRegionalActorArtBinding *binding=NULL;
    for(unsigned area=1;area<=7 && !binding;++area)for(unsigned room=1;room<=8;++room) {
      const ArRegionalActorArtBinding *candidate=ArRegionalActorArt_Binding((uint16_t)(room<<8|area),kArRegionalActorArt_Pictures,(base-0x4000)/0x1000);
      if(candidate && candidate->source==source) {binding=candidate;break;}
    }
    assert(binding);
    const bool absent=(binding->scene==0x0202 && v==0x3a) ||
        ((binding->scene==0x0303 || binding->scene==0x0407) && v==0x28);
    ActRaiserActorArtDraw draw;
    assert(ActRaiserActorArt_Draw((uint16_t)base,(uint16_t)composition,v,&draw)==!absent);
    if(absent)continue;
    assert(draw.attributes_only==(binding->scene==0x0305 && binding->slot==1));
    assert(draw.picture.count && draw.picture.count<=128);
    for(unsigned p=0;p<draw.picture.count;++p)for(unsigned flip=0;flip<4;++flip) {
      ArRegionalActorArtPart expected;assert(ArRegionalActorArt_Part(&draw.picture,p,&expected));
      const int16_t left=flip&1?-31:23,top=flip&2?53:-17;
      ActRaiserActorArtPart part={89,-73,0xffff,!expected.large};
      ActRaiserActorArt_ResolvePart(&draw,p,(flip&1)!=0,(flip&2)!=0,left,top,&part);
      assert(part.attributes==expected.attributes);
      if(draw.attributes_only)assert(part.x==89 && part.y==-73 && part.large!=expected.large);
      else {
        // Cancellation of the native collision anchor must be exact, also
        // for negative coordinates and asymmetric flipped pictures.
        assert(211-left+part.x==211+expected.x[(flip&1)!=0]);
        assert(173-top+part.y==173+expected.y[(flip&2)!=0]);
        assert(part.large==expected.large);
      }
    }
    assert(!ActRaiserActorArt_Draw((uint16_t)base,(uint16_t)(composition+1),v,&draw));
  }
  ActRaiserActorArt_Shutdown();free((void *)native.data);free((void *)packet.data);
}
int main(int argc,char **argv) {
  if(argc==6 && !strcmp(argv[1],"--native")) {
    CheckNative((unsigned)strtoul(argv[2],NULL,16),(unsigned)strtoul(argv[3],NULL,16),argv[4],argv[5]);
    return 0;
  }
  assert(argc==1);
  const unsigned pictures=58,table_size=4+4*(pictures+1)+pictures*16;
  const unsigned size=60+8192+128+table_size;
  uint8_t *data=calloc(size,1);assert(data);
  memcpy(data,"ARACTOR1",8);Write32(data+8,3);
  Record(data+12,1,60,8192);Record(data+28,2,60+8192,128);
  Record(data+44,3,60+8192+128,table_size);
  uint8_t *table=data+60+8192+128;
  Write16(table,pictures);
  for(unsigned v=0;v<pictures;++v) {
    const unsigned offset=4+4*(pictures+1)+v*16;
    Write32(table+4+v*4,offset);Write16(table+offset,1);
    table[offset+4]=1;
    Write16(table+offset+6,(unsigned)-23);Write16(table+offset+8,31);
    Write16(table+offset+10,37);Write16(table+offset+12,(unsigned)-41);
    Write16(table+offset+14,0x123);
  }
  Write32(table+4+pictures*4,table_size);
  ArRegionalActorArtView donor;
  assert(ArRegionalActorArt_Parse((ArRegionalMediaBytes){data,size},&donor));
  ActRaiserActorArt_Initialize(&donor);assert(observer);
  const ArRegionalActorArtBinding *animation=ArRegionalActorArt_Binding(0x0202,kArRegionalActorArt_Pictures,0);
  const ArRegionalActorArtBinding *characters=ArRegionalActorArt_Binding(0x0202,kArRegionalActorArt_Characters,0);
  const ArRegionalActorArtBinding *palette=ArRegionalActorArt_Binding(0x0202,kArRegionalActorArt_Palette,0);
  assert(animation && characters && palette && animation->pictures==pictures+1);
  uint8_t native[8192]={0};Write16(native,animation->table);
  const unsigned first=animation->table+animation->pictures*2;
  for(unsigned v=0;v<animation->pictures;++v)Write16(native+animation->table+v*2,first);
  native[first+4]=1;
  const uint32_t source_snes=(animation->source>>15)<<16|(animation->source&0x7fff)|0x8000;
  observer(observer_context,source_snes+2,0x4000,native,animation->size);
  ActRaiserActorArtDraw draw={0};
  assert(!ActRaiserActorArt_Draw(0x4000,0x4000+first,0,&draw));
  requested=true;ActRaiserActorArt_BeginRoom(0x0202);
  assert(ActRaiserActorArt_NeedsUpload(0x0202,kArRegionalActorArt_Characters,0,characters->source));
  assert(!captures);
  ArRegionalMediaBytes pixels={0};
  assert(ActRaiserActorArt_Upload(0x0202,kArRegionalActorArt_Characters,0,0,&pixels) && !pixels.data && !captures);
  assert(ActRaiserActorArt_Upload(0x0202,kArRegionalActorArt_Characters,0,characters->source,&pixels));
  assert(pixels.size==8192 && captures==1);
  // Mid-load request cannot mix US palette with the just-selected JP atlas.
  requested=false;
  assert(ActRaiserActorArt_Upload(0x0202,kArRegionalActorArt_Palette,0,palette->source,&pixels));
  assert(pixels.size==128 && captures==1);
  assert(ActRaiserActorArt_Draw(0x4000,0x4000+first,0,&draw) && draw.picture.count==1 && !draw.attributes_only);
  for(unsigned flip=0;flip<4;++flip) {
    ActRaiserActorArtPart part={0};
    ActRaiserActorArt_ResolvePart(&draw,0,(flip&1)!=0,(flip&2)!=0,-7,9,&part);
    assert(part.x==((flip&1)?31:-23)-7 && part.y==((flip&2)?-41:37)+9);
    assert(part.large && part.attributes==0x123);
    draw.attributes_only=true;
    part=(ActRaiserActorArtPart){17,-51,0x456,false};
    ActRaiserActorArt_ResolvePart(&draw,0,(flip&1)!=0,(flip&2)!=0,-7,9,&part);
    assert(part.x==17 && part.y==-51 && !part.large && part.attributes==0x123);
    ActRaiserActorArt_ResolvePart(&draw,1,false,false,0,0,&part);
    assert(part.x==17 && part.attributes==0x123);
    draw.attributes_only=false;
  }
  assert(!ActRaiserActorArt_Draw(0x4000,0x4000+first,pictures,&draw)); // US blank extra.
  assert(!ActRaiserActorArt_Draw(0x4000,0x4000+first+1,0,&draw));
  ActRaiserActorArt_BeginRoom(0x0302); // Native script inherits its actor resources.
  assert(captures==1 && ActRaiserActorArt_Draw(0x4000,0x4000+first,0,&draw));
  ActRaiserActorArt_BeginRoom(0x0202); // Actual reload captures pending US.
  assert(ActRaiserActorArt_NeedsUpload(0x0202,kArRegionalActorArt_Characters,0,characters->source));
  assert(ActRaiserActorArt_Upload(0x0202,kArRegionalActorArt_Characters,0,characters->source,&pixels));
  assert(!pixels.data && captures==2 && !ActRaiserActorArt_Draw(0x4000,0x4000+first,0,&draw));
  // A partial in-stage upload must not consume a new setting or mix its
  // palette with inherited graphics. Bloodpool room 7 reloads CHR/palette
  // without replacing the ordinary animation bank.
  requested=true;ActRaiserActorArt_BeginRoom(0x0702);
  const ArRegionalActorArtBinding *partial=ArRegionalActorArt_Binding(0x0702,kArRegionalActorArt_Characters,0);
  assert(partial);
  assert(!ActRaiserActorArt_NeedsUpload(0x0702,kArRegionalActorArt_Characters,0,partial->source));
  assert(ActRaiserActorArt_Upload(0x0702,kArRegionalActorArt_Characters,0,partial->source,&pixels));
  assert(!pixels.data && captures==2);
  requested=true;ActRaiserActorArt_BeginRoom(0x0202);accept=false;
  assert(!ActRaiserActorArt_Upload(0x0202,kArRegionalActorArt_Characters,0,characters->source,&pixels));
  assert(captures==2);accept=true;
  assert(ActRaiserActorArt_Upload(0x0202,kArRegionalActorArt_Characters,0,characters->source,&pixels));
  assert(ActRaiserActorArt_Draw(0x4000,0x4000+first,0,&draw));
  observer(observer_context,0,0x4000+first,native,1);
  assert(!ActRaiserActorArt_Draw(0x4000,0x4000+first,0,&draw));
  ActRaiserActorArt_Shutdown();assert(!observer);
  ActRaiserActorArt_Initialize(NULL);ActRaiserActorArt_BeginRoom(0x0202);
  assert(!ActRaiserActorArt_NeedsUpload(0x0202,kArRegionalActorArt_Characters,0,characters->source));
  assert(ActRaiserActorArt_Upload(0x0202,kArRegionalActorArt_Characters,0,characters->source,&pixels) && !pixels.data);
  assert(!ActRaiserActorArt_Draw(0x4000,0x4000+first,0,&draw));
  ActRaiserActorArt_Shutdown();free(data);
  return 0;
}
