#include "actraiser/actraiser_town_art.h"
#include "actraiser/actraiser_regional_media.h"
#include "regional/regional_artwork.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[8192],characters[3][0x4000],vram[65536];
static uint8_t followers[256],lairs[128],pyramid[32];
static uint16_t destination;
static uint8_t requested,captured;
static unsigned captures,writes;

uint8_t ActRaiserRegional_TownArtworkSnapshot(uint16_t scene) {(void)scene;return captured;}
bool ActRaiserRegional_BeginTownArtwork(uint16_t scene,uint8_t *mask) {
  assert(scene>=0x100 && scene<=0x600 && !(scene&255));
  ++captures;captured=requested;*mask=captured;return true;
}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu;
  if(bank==0 && address<sizeof(ram))return ram[address];
  if(bank==12 && address>=0x8000)return characters[address>=0xc000][address&0x3fff];
  if(bank==13 && address>=0x8000 && address<0xc000)return characters[2][address&0x3fff];
  assert(0);return 0;
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) {
  return (uint16)(cpu_read8(cpu,bank,address)|(uint16)cpu_read8(cpu,bank,address+1)<<8);
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu;assert(!bank);
  if(address<sizeof(ram)) {ram[address]=value;return;}
  if(address==0x2118) {vram[(uint16_t)(destination*2)]=value;++writes;return;}
  if(address==0x2119) {vram[(uint16_t)(destination*2+1)]=value;++destination;++writes;return;}
  assert(0);
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(cpu,bank,address,(uint8)value);cpu_write8(cpu,bank,address+1,(uint8)(value>>8));
}
static CpuState Prepare(unsigned bank,unsigned town,uint8_t status) {
  memset(ram,0x79,sizeof(ram));memset(vram,0xa9,sizeof(vram));writes=0;
  CpuState cpu={0};cpu.PB=2;cpu.S=0x1dc;cpu.Y=0x1234;cpu.X=0xc000;cpu.A=0xfff1;
  cpu.P=CPU_P_C;cpu_p_to_mirrors(&cpu);
  cpu_write16(&cpu,0,0,0);cpu_write16(&cpu,0,2,0x4000);
  cpu_write16(&cpu,0,0x18,(uint16_t)(town<<8));
  cpu_write16(&cpu,0,0xa5,bank==1?0xc000:0x8000);ram[0xa7]=bank==2?13:12;
  ram[cpu.S+1]=status;
  destination=bank==2?0x2000:0;
  return cpu;
}
static void CheckBank(unsigned bank,unsigned town,uint8_t mask,uint8_t status) {
  requested=mask;captured=mask;
  CpuState cpu=Prepare(bank,town,status),before=cpu;
  uint8_t before_ram[sizeof(ram)];memcpy(before_ram,ram,sizeof(ram));
  assert(ActRaiser_TownArtEntry(&cpu));
  assert(!memcmp(before_ram,ram,sizeof(ram)) && !memcmp(&before,&cpu,sizeof(cpu)));
  const unsigned previous=captures;
  assert(ActRaiser_LoadTownArt(&cpu)==RECOMP_RETURN_NORMAL);
  assert(captures==previous+(bank!=2));
  assert(cpu.Y==before.Y && cpu.X==before.X && cpu.DB==before.DB && cpu.PB==before.PB && cpu.D==before.D);
  assert(cpu.P==status && cpu.S==before.S+3);
  assert(cpu.A==(uint16_t)(characters[bank][0x3ffe]|(uint16_t)characters[bank][0x3fff]<<8));
  before_ram[before.S]=(uint8_t)(before.Y>>8);before_ram[before.S-1]=(uint8_t)before.Y;
  assert(!memcmp(before_ram,ram,sizeof(ram)) && writes==0x4000);
  uint8_t expected[0x4000];memcpy(expected,characters[bank],sizeof(expected));
  ActRaiserTownArtSpan spans[4];unsigned count=ActRaiserRegionalMedia_TownSpans(mask,bank,spans);
  for(unsigned i=0;i<count;++i) {
    assert(spans[i].offset+spans[i].bytes.size<=sizeof(expected));
    if(i)assert(spans[i].offset>=spans[i-1].offset+spans[i-1].bytes.size);
    memcpy(expected+spans[i].offset,spans[i].bytes.data,spans[i].bytes.size);
  }
  const unsigned base=bank==2?0x4000:0;
  assert(!memcmp(vram+base,expected,sizeof(expected)));
  for(unsigned i=0;i<sizeof(vram);++i)if(i<base || i>=base+0x4000)assert(vram[i]==0xa9);
  assert(destination==(bank==2?0x4000:0x2000));
}
int main(void) {
  for(unsigned bank=0;bank<3;++bank)for(unsigned i=0;i<0x4000;++i)characters[bank][i]=(uint8_t)(i*13+bank*29);
  for(unsigned i=0;i<sizeof(followers);++i)followers[i]=(uint8_t)(i*3+i/64);
  for(unsigned i=0;i<sizeof(lairs);++i)lairs[i]=(uint8_t)(i*5+i/64);
  memset(pyramid,0xc3,sizeof(pyramid));
  const ArRegionalMediaView donor={.release=kArRegionalMediaRelease_Japan,.count=3,.entries={
    {kArRegionalMedia_TownFollowerSymbols,{followers,sizeof(followers)}},
    {kArRegionalMedia_TownLairSymbols,{lairs,sizeof(lairs)}},
    {kArRegionalMedia_TownPyramidDetail,{pyramid,sizeof(pyramid)}}}};
  for(unsigned available=0;available<2;++available) {
    ActRaiserRegionalMedia_ClearDonors();
    if(available)assert(ActRaiserRegionalMedia_AddDonor(&donor));
    assert(ActRaiserRegionalMedia_AvailableArtwork()==(available?kArRegionalArtwork_TownMask:0));
    for(unsigned mask=0;mask<32;++mask)for(unsigned bank=0;bank<3;++bank)for(unsigned town=1;town<=6;++town)
      CheckBank(bank,town,mask,(uint8_t)(CPU_P_M|(town&1?CPU_P_D:0)|(mask&CPU_P_C)));
  }
  ActRaiserTownArtSpan spans[4];
  assert(ActRaiserRegionalMedia_TownSpans(0xff,kActRaiserTownArt_Late,spans)==1 && spans[0].offset==0x2260);
  assert(ActRaiserRegionalMedia_TownSpans(0xff,kActRaiserTownArt_Early,spans)==3);
  assert(spans[1].offset==0x39c0 && spans[2].offset==0x3bc0);
  assert(ActRaiserRegionalMedia_TownSpans(0xff,kActRaiserTownArt_Objects,spans)==4);
  assert(spans[0].offset==0x3440 && spans[0].bytes.data==followers && spans[0].bytes.size==64);
  assert(spans[1].offset==0x3500 && spans[1].bytes.data==followers+128 && spans[1].bytes.size==64);
  assert(spans[2].offset==0x3640 && spans[2].bytes.data==followers+64 && spans[2].bytes.size==64);
  assert(spans[3].offset==0x3700 && spans[3].bytes.data==followers+192 && spans[3].bytes.size==64);
  assert(!ActRaiserRegionalMedia_TownSpans(0xff,3,spans));
  assert(!ActRaiserRegionalMedia_TownSpans(0xff,0,NULL));
  assert(!ActRaiser_TownArtEntry(NULL));
  for(unsigned i=0;i<12;++i) {
    CpuState cpu=Prepare(0,1,0);
    switch(i) {
      case 0:cpu.m_flag=1;break;case 1:cpu.x_flag=1;break;case 2:cpu.emulation=1;break;
      case 3:cpu.DB=1;break;case 4:cpu.D=1;break;case 5:cpu.PB=1;break;
      case 6:ram[0x18]=1;break;case 7:ram[0x19]=0;break;case 8:ram[0x19]=7;break;
      case 9:ram[0]=1;break;case 10:ram[3]=0x20;break;case 11:ram[0xa7]=14;break;
    }
    assert(!ActRaiser_TownArtEntry(&cpu));
  }
  // A request between BG and OBJ does not switch the current town's symbols.
  requested=0;captured=4;CpuState cpu=Prepare(2,1,CPU_P_M);unsigned previous=captures;
  assert(ActRaiser_LoadTownArt(&cpu)==RECOMP_RETURN_NORMAL && captured==4 && captures==previous);
  assert(!memcmp(vram+0x4000+0x1a2*32,followers,64));
  ActRaiserRegionalMedia_ClearDonors();
  puts("town artwork: sparse bytes, guards, bank scope, fallback and native CPU/PPU contract passed");
  return 0;
}
