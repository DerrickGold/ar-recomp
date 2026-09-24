#include "randomizer.h"
#include "settings.h"
#include "regional/regional_placements.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
Settings g_settings;
const SettingDesc *Settings_Find(const char *key) {(void)key;return NULL;}
SettingChangeResult Settings_SetLong(const SettingDesc *d,long value) {(void)d;(void)value;return 0;}
static uint64_t Hash(uint64_t hash,const uint8_t *data,size_t size) {
  for(size_t i=0;i<size;++i)hash=(hash^data[i])*UINT64_C(1099511628211);
  return hash;
}
static void Fixture(uint8_t *rom) {
  const unsigned index=0x53100;
  const uint8_t first[]={5,30,0,255,10,20,255,3,20,20,0,1,30,20,1,128,40,20,2,2,
      50,20,5,128,254,60,0,62,20,65,20,0,1,70,20,7,128,80,20,4,128,255};
  const uint8_t second[]={5,30,0,255,20,20,0,2,30,20,6,128,40,20,0,1,50,20,5,128,255};
  ByteOrder_WriteLe16(rom+index,0x101);ByteOrder_WriteLe16(rom+index+2,0x100);
  ByteOrder_WriteLe16(rom+index+4,0x201);ByteOrder_WriteLe16(rom+index+6,0x200);
  ByteOrder_WriteLe16(rom+index+8,0xffff);
  memcpy(rom+index+0x100,first,sizeof(first));memcpy(rom+index+0x200,second,sizeof(second));
  ByteOrder_WriteLe16(rom+0x28f8,0xa930);ByteOrder_WriteLe16(rom+0x28fa,0xa940);
  ByteOrder_WriteLe16(rom+0x28fc,0xa960); /* direct code, not a spawn definition */
  ByteOrder_WriteLe16(rom+0x28fe,0xa930); /* alias: never scale twice */
  ByteOrder_WriteLe16(rom+0x2930,0x4000);rom[0x2932]=0x7e;
  ByteOrder_WriteLe16(rom+0x2940,0x4000);rom[0x2942]=0x7e;
  rom[0x2937]=rom[0x2938]=rom[0x2947]=rom[0x2948]=2;
  const uint8_t code[]={0xa5,0x82,0xc9,0x20,0,0xb0,3,0x20,0x49,0x88,0x60,0};
  memcpy(rom+0x2960,code,sizeof(code));
}
static size_t DirectRows(const uint8_t *rom,unsigned *out) {
  size_t count=0;
  for(unsigned index=0x53100;ByteOrder_ReadLe16(rom+index)!=0xffff;index+=4) {
    assert(index<0x53500);
    if(!(ByteOrder_ReadLe16(rom+index)&255))continue;
    unsigned cursor=0x53100+ByteOrder_ReadLe16(rom+index+2)+3;
    for(unsigned boxes=0;rom[cursor]!=255;++boxes){assert(boxes<64);cursor+=5;}
    ++cursor;
    for(unsigned guard=0;rom[cursor]!=255;++guard) {
      assert(guard<256 && cursor>=0x50000 && cursor<0x5fffb);
      if(rom[cursor]==254){cursor+=5;continue;}
      if(rom[cursor]==253){cursor+=2;continue;}
      if(rom[cursor]==252){cursor=0x50000+(ByteOrder_ReadLe16(rom+cursor+1)&0x7fff);continue;}
      if(rom[cursor+2]==255){assert(count<256);out[count++]=cursor;}
      cursor+=4;
    }
  }
  return count;
}
static void Stats(const uint8_t *baseline,uint8_t *rom) {
  static const unsigned tables[]={0x96af,0xa8f6,0xb449,0xc11e,0xcd9b,0xd928,0xe722,0xf39a};
  uint8_t *expected=malloc(0x100000);assert(expected);memcpy(expected,baseline,0x100000);
  bool seen[32768]={0};
  for(unsigned t=0;t<8;++t) {
    unsigned end=65536;
    for(unsigned at=tables[t];at<end && at<tables[t]+256;at+=2) {
      const unsigned ptr=ByteOrder_ReadLe16(baseline+(at&0x7fff));
      if(!ptr)continue;
      if(ptr<=tables[t])break;
      if(ptr<end)end=ptr;
      const unsigned offset=ptr&0x7fff,anim=ByteOrder_ReadLe16(baseline+offset),bank=baseline[offset+2];
      if(seen[offset])continue;
      seen[offset]=true;
      if(!((bank==0x7e&&(anim==0x4000||anim==0x5000)) || (bank==6&&(anim==0x8000||anim==0xa800))))continue;
      if(ByteOrder_ReadLe16(baseline+offset+4)&0x200)continue;
      for(unsigned field=7;field<=8;++field) {
        const unsigned scaled=2*baseline[offset+field];expected[offset+field]=scaled>255?255:scaled;
      }
    }
  }
  g_settings=(Settings){0};g_settings.rando_enable=true;
  g_settings.rando_enemy_hp=g_settings.rando_enemy_atk=200;
  Randomizer_Apply();assert(!memcmp(expected,rom,0x100000));
  free(expected);
}
static void Programs(uint8_t *rom) {
  static ActionPlacementProgram programs[49],before[49],again[49];
  RandomizerPlacementMap maps[49];
  uint8_t *rom_before=malloc(0x100000);assert(rom_before);memcpy(rom_before,rom,0x100000);
  unsigned count=0;
  const ArRegionalPlacementPolicy us={0,0};
  for(unsigned area=1;area<=7;++area)for(unsigned room=1;room<=8;++room) {
    const uint16_t scene=(uint16_t)(room<<8|area);
    ActionPlacementProgram candidate;
    if(!ArRegionalPlacements_Copy(&us,scene,false,0,&candidate))continue;
    assert(count<49);maps[count]=(RandomizerPlacementMap){scene,&programs[count]};++count;
  }
  assert(count==49);
  for(unsigned e=0;e<3;++e)for(unsigned p=0;p<3;++p)for(unsigned mode=0;mode<2;++mode)
    for(unsigned d=0;d<3;++d)for(unsigned scope=0;scope<2;++scope) {
      const ArRegionalPlacementPolicy policy={e,p};
      for(unsigned i=0;i<count;++i)assert(ArRegionalPlacements_Copy(&policy,maps[i].scene,mode,d,&programs[i]));
      memcpy(before,programs,sizeof(before));
      g_settings=(Settings){0};g_settings.rando_enable=true;g_settings.rando_seed=1234567;
      g_settings.rando_enemy_hp=g_settings.rando_enemy_atk=100;
      g_settings.rando_statue_drops=kRandomMode_Shuffle;g_settings.rando_statue_spots=kRandomMode_Shuffle;
      g_settings.rando_enemy_types=kRandomMode_Shuffle;g_settings.rando_enemy_scope=scope;
      RandomizerSummary summary,repeat;
      const RandomizerSummary last=*Randomizer_LastSummary();
      assert(Randomizer_ApplyPlacementPrograms(maps,count,&summary));
      assert(summary.applied && summary.seed==1234567 && summary.maps_touched==49);
      assert(!memcmp(&last,Randomizer_LastSummary(),sizeof(last)));
      memcpy(again,programs,sizeof(again));memcpy(programs,before,sizeof(programs));
      assert(Randomizer_ApplyPlacementPrograms(maps,count,&repeat));
      assert(!memcmp(again,programs,sizeof(programs)) && !memcmp(&summary,&repeat,sizeof(summary)));
      for(unsigned i=0;i<count;++i) {
        assert(ActionPlacements_Validate(&programs[i]) && programs[i].count==before[i].count);
        unsigned before_items[8]={0},after_items[8]={0},wave=0;
        for(size_t j=0;j<programs[i].count;++j) {
          const ActionPlacement *a=&before[i].rows[j],*b=&programs[i].rows[j];
          assert(a->id==b->id && a->kind==b->kind);
          if(a->kind!=kActionPlacement_Object || a->parameter==255 || (a->type&128 && a->type!=128))
            assert(!memcmp(a,b,sizeof(*a)));
          else if(a->type==128) {
            assert(a->type==b->type && a->parameter<8 && b->parameter<8);
            ++before_items[a->parameter];++after_items[b->parameter];
            unsigned matches_before=0,matches_after=0,other_wave=0;
            for(size_t k=0;k<before[i].count;++k) {
              const ActionPlacement *old=&before[i].rows[k],*now=&programs[i].rows[k];
              if(other_wave==wave && old->kind==kActionPlacement_Object && old->type==128) {
                matches_before+=old->x==b->x && old->y==b->y;
                matches_after+=now->x==b->x && now->y==b->y;
              }
              if(old->kind==kActionPlacement_Wave)++other_wave;
            }
            assert(matches_before==matches_after && matches_before);
          } else assert(a->x==b->x && a->y==b->y && a->parameter==b->parameter);
          if(a->kind==kActionPlacement_Wave)++wave;
        }
        assert(!memcmp(before_items,after_items,sizeof(before_items)));
      }
      assert(!memcmp(rom,rom_before,0x100000));
    }
  memcpy(before,programs,sizeof(before));
  for(unsigned bad=0;bad<6;++bad) {
    RandomizerSummary summary,unchanged;memset(&summary,0x55,sizeof(summary));unchanged=summary;
    const uint16_t scene=maps[1].scene;ActionPlacementProgram *ptr=maps[1].program;
    if(bad==0)maps[1].scene=maps[0].scene;
    if(bad==1)maps[1].program=maps[0].program;
    if(bad==2)maps[1].program=NULL;
    if(bad==3)g_settings.rando_enemy_scope=255;
    assert(!Randomizer_ApplyPlacementPrograms(bad==4?NULL:maps,bad==5?50:49,&summary));
    assert(!memcmp(programs,before,sizeof(before)) && !memcmp(&summary,&unchanged,sizeof(summary)));
    maps[1].scene=scene;maps[1].program=ptr;g_settings.rando_enemy_scope=0;
  }
  g_settings.rando_enable=false;RandomizerSummary disabled;
  assert(Randomizer_ApplyPlacementPrograms(maps,49,&disabled) && !disabled.applied);
  assert(!memcmp(before,programs,sizeof(before)));
  free(rom_before);
}
int main(int argc,char **argv) {
  uint8_t *rom=calloc(1,0x100000),*baseline=malloc(0x100000),*again=malloc(0x100000);
  assert(rom && baseline && again);
  if(argc==2) {
    FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(rom,1,0x100000,f)==0x100000 && fgetc(f)==EOF);fclose(f);
  } else {assert(argc==1);Fixture(rom);}
  memcpy(baseline,rom,0x100000);assert(Randomizer_Init(rom,0x100000));
  unsigned direct[256];const size_t direct_count=DirectRows(baseline,direct);assert(direct_count);
  uint64_t hash=UINT64_C(14695981039346656037);
  for(unsigned seed=0;seed<4;++seed)for(unsigned drops=0;drops<3;++drops)
    for(unsigned spots=0;spots<2;++spots)for(unsigned types=0;types<3;++types) {
      g_settings=(Settings){0};g_settings.rando_enable=true;g_settings.rando_seed=seed*1234567;
      g_settings.rando_enemy_hp=g_settings.rando_enemy_atk=100;
      g_settings.rando_statue_drops=drops;g_settings.rando_statue_spots=spots;
      g_settings.rando_enemy_types=types!=0;g_settings.rando_enemy_scope=types==2;
      Randomizer_Apply();
      for(size_t i=0;i<direct_count;++i)assert(!memcmp(rom+direct[i],baseline+direct[i],4));
      hash=Hash(hash,rom,0x100000);memcpy(again,rom,0x100000);
      Randomizer_Apply();assert(!memcmp(again,rom,0x100000));
      g_settings.rando_enable=false;Randomizer_Apply();assert(!memcmp(baseline,rom,0x100000));
    }
  printf("Randomizer baseline %s digest=%016llx\n",argc==2?"ROM":"synthetic",(unsigned long long)hash);
  assert(hash==(argc==2?UINT64_C(0x24d319948f8ea8c5):UINT64_C(0xa09369eed962638d)));
  Stats(baseline,rom);
  Programs(rom);
  free(again);free(baseline);free(rom);return 0;
}
