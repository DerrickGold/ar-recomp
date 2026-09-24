#include "regional/regional_actor_art.h"
#include "regional/regional_actor_art_residency.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t Read32(const uint8_t *p) {
  return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void Write16(uint8_t *p,unsigned v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void Residency(void) {
  ArRegionalActorArtResidency resident={0};
  const ArRegionalActorArtBinding *ordinary=ArRegionalActorArt_Binding(0x0107,kArRegionalActorArt_Pictures,0);
  const ArRegionalActorArtBinding *boss=ArRegionalActorArt_Binding(0x0207,kArRegionalActorArt_Pictures,1);
  assert(ordinary && boss && ordinary->size>0x1000);
  uint8_t image[8192]={0};Write16(image,ordinary->table);
  const unsigned first=ordinary->table+2*ordinary->pictures;
  for(unsigned v=0;v<ordinary->pictures;++v)Write16(image+ordinary->table+v*2,first);
  image[first+4]=1;
  // The final picture straddles the other animation slot's allocation.
  Write16(image+ordinary->table+2*(ordinary->pictures-1),ordinary->size-12);
  image[ordinary->size-8]=1;
  assert(ArRegionalActorArt_ObserveDecode(&resident,ordinary->source,0x4000,
      (ArRegionalMediaBytes){image,ordinary->size}));
  assert(ArRegionalActorArt_Resident(&resident,0x4000,0x4000+first,0)==ordinary);
  const unsigned last=ordinary->pictures-1;
  assert(ArRegionalActorArt_Resident(&resident,0x4000,0x4000+ordinary->size-12,last)==ordinary);
  uint8_t other[8192]={0};Write16(other,boss->table);
  const unsigned second=boss->table+2*boss->pictures;
  for(unsigned v=0;v<boss->pictures;++v)Write16(other+boss->table+v*2,second);
  other[second+4]=1;
  assert(ArRegionalActorArt_ObserveDecode(&resident,boss->source,0x5000,
      (ArRegionalMediaBytes){other,boss->size}));
  assert(ArRegionalActorArt_Resident(&resident,0x4000,0x4000+first,0)==ordinary);
  assert(!ArRegionalActorArt_Resident(&resident,0x4000,0x4000+ordinary->size-12,last));
  assert(ArRegionalActorArt_Resident(&resident,0x5000,0x5000+second,0)==boss);
  assert(!ArRegionalActorArt_Resident(&resident,0x5000,0x5000+second+1,0));
  assert(!ArRegionalActorArt_Resident(&resident,0x5000,0x5000+second,boss->pictures));
  // An unrelated write retires the affected picture, even without a donor.
  assert(!ArRegionalActorArt_ObserveDecode(&resident,0,0x4000+first,
      (ArRegionalMediaBytes){image,1}));
  assert(!ArRegionalActorArt_Resident(&resident,0x4000,0x4000+first,0));
  // A wrapping write reaches the prefix and invalidates the complete binding.
  static uint8_t wrap[0x6000];
  assert(!ArRegionalActorArt_ObserveDecode(&resident,0,0xf800,
      (ArRegionalMediaBytes){wrap,sizeof(wrap)}));
  assert(!resident.banks[0].binding && !resident.banks[1].binding);
  // Known source with malformed bounds fails closed, without inventing poses.
  Write16(other+boss->table,0xffff);
  assert(!ArRegionalActorArt_ObserveDecode(&resident,boss->source,0x5000,
      (ArRegionalMediaBytes){other,boss->size}));
  assert(!resident.banks[1].binding);
  assert(!ArRegionalActorArt_ObserveDecode(NULL,0,0,(ArRegionalMediaBytes){image,1}));
}
static void Exercise(ArRegionalMediaBytes bytes) {
  ArRegionalActorArtView view;
  assert(ArRegionalActorArt_Parse(bytes,&view));
  for(unsigned i=0;i<view.count;++i) {
    const uint8_t *record=bytes.data+12+16*i;
    const uint16_t scene=(uint16_t)(record[0]|(uint16_t)record[1]<<8);
    ArRegionalMediaBytes resource=ArRegionalActorArt_Find(&view,scene,
        (ArRegionalActorArtKind)record[2],record[3]);
    assert(resource.data==bytes.data+Read32(record+4) && resource.size==Read32(record+8));
    if(record[2]!=kArRegionalActorArt_Pictures)continue;
    const unsigned count=resource.data[0]|(unsigned)resource.data[1]<<8;
    for(unsigned v=0;v<count;++v) {
      ArRegionalActorArtPicture picture;
      assert(ArRegionalActorArt_Picture(resource,v,&picture));
      for(unsigned j=0;j<picture.count;++j) {
        ArRegionalActorArtPart part;
        assert(ArRegionalActorArt_Part(&picture,j,&part));
        for(unsigned f=0;f<2;++f) {
          assert(part.x[f]>=-127 && part.x[f]<=383);
          assert(part.y[f]>=-127 && part.y[f]<=383);
        }
      }
      ArRegionalActorArtPart part;
      assert(!ArRegionalActorArt_Part(&picture,picture.count,&part));
    }
    ArRegionalActorArtPicture missing;
    assert(!ArRegionalActorArt_Picture(resource,count,&missing));
    assert(!ArRegionalActorArt_Picture(resource,~0u,&missing));
  }
  assert(!ArRegionalActorArt_Find(&view,0,kArRegionalActorArt_Characters,0).data);
}
int main(int argc,char **argv) {
  Residency();
  const uint8_t valid[]={
    'A','R','A','C','T','O','R','1',1,0,0,0,
    1,1,3,0,28,0,0,0,28,0,0,0,0,0,0,0,
    1,0,0,0,12,0,0,0,28,0,0,0,
    1,0,0,0,1,0,8,0,0xb1,0xff,0xfa,0xff,0xd1,0xff,0x34,0x42,
  };
  Exercise((ArRegionalMediaBytes){valid,sizeof(valid)});
  ArRegionalActorArtView sentinel={.count=123},out=sentinel;
  for(size_t n=0;n<sizeof(valid);++n) {
    assert(!ArRegionalActorArt_Parse((ArRegionalMediaBytes){valid,n},&out));
    assert(!memcmp(&out,&sentinel,sizeof(out)));
  }
  uint8_t bad[sizeof(valid)+1];memcpy(bad,valid,sizeof(valid));bad[sizeof(valid)]=0;
  assert(!ArRegionalActorArt_Parse((ArRegionalMediaBytes){bad,sizeof(bad)},&out));
  const unsigned invalid[]={0,7,8,9,10,11,12,13,14,15,16,19,20,23,24,27,
      28,29,30,31,32,35,36,39,40,41,42,43,44,45,47,49,53};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
    memcpy(bad,valid,sizeof(valid));bad[invalid[i]]=0x7f;
    assert(!ArRegionalActorArt_Parse((ArRegionalMediaBytes){bad,sizeof(valid)},&out));
    assert(!memcmp(&out,&sentinel,sizeof(out)));
  }
  assert(!ArRegionalActorArt_Parse((ArRegionalMediaBytes){NULL,100},&out));
  assert(!ArRegionalActorArt_Parse((ArRegionalMediaBytes){valid,sizeof(valid)},NULL));
  assert(!ArRegionalActorArt_Find(NULL,0,kArRegionalActorArt_Pictures,0).data);
  assert(!ArRegionalActorArt_Picture((ArRegionalMediaBytes){0},0,NULL));
  assert(!ArRegionalActorArt_Part(NULL,0,NULL));
  for(int i=1;i<argc;++i) {
    const bool native=strcmp(argv[i],"--native")==0;
    uint32_t source=0;uint16_t destination=0;
    if(native) {
      assert(i+3<argc);source=(uint32_t)strtoul(argv[++i],NULL,16);
      destination=(uint16_t)strtoul(argv[++i],NULL,16);++i;
    }
    FILE *f=fopen(argv[i],"rb");assert(f && !fseek(f,0,SEEK_END));
    const long size=ftell(f);assert(size>0 && size<=kArRegionalMediaMaximumBytes && !fseek(f,0,SEEK_SET));
    uint8_t *data=malloc((size_t)size);assert(data);
    assert(fread(data,1,(size_t)size,f)==(size_t)size);fclose(f);
    if(native) {
      ArRegionalActorArtResidency resident={0};
      assert(ArRegionalActorArt_ObserveDecode(&resident,source,destination,
          (ArRegionalMediaBytes){data,(size_t)size}));
      const ArRegionalActorArtResidentBank *bank=&resident.banks[(destination-0x4000)/0x1000];
      for(unsigned v=0;v<bank->binding->pictures;++v)
        assert(ArRegionalActorArt_Resident(&resident,destination,bank->begin[v],v)==bank->binding);
    } else Exercise((ArRegionalMediaBytes){data,(size_t)size});
    free(data);
  }
  puts("actor art: bounded scene resources and draw-only picture access passed");
  return 0;
}
