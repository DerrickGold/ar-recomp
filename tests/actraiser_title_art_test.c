#include "actraiser/regional/actraiser_title_art.h"
#include "actraiser/regional/actraiser_regional_media.h"
#include "regional/presentation/regional_artwork.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[8192],native[33024],donor[33024],vram[65536],palette[256];
static uint16_t destination;
static unsigned colors,writes,captures;
static uint8_t requested,captured,vmain;
uint8_t ActRaiserRegional_TitleArtworkSnapshot(void) {return captured;}
bool ActRaiserRegional_BeginTitleArtwork(uint8_t *mask) {++captures;*mask=captured=requested;return true;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu;
  if(!bank && address<sizeof(ram))return ram[address];
  if(bank==0x0b && address>=0x8300 && address<0xc300)return native[address-0x8300];
  if(bank==0x7e && address>=0xc000)return native[0x4000+address-0xc000];
  if(bank==0x1c && address>=0xba93 && address<0xbb93)return native[0x8000+address-0xba93];
  assert(0);return 0;
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) {
  return (uint16)(cpu_read8(cpu,bank,address)|(uint16)cpu_read8(cpu,bank,address+1)<<8);
}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu;assert(!bank);
  if(address<sizeof(ram)) {ram[address]=value;return;}
  switch(address) {
    case 0x2115:vmain=value;break;
    case 0x2116:destination=(destination&0xff00)|value;break;
    case 0x2117:destination=(destination&255)|(uint16_t)value<<8;break;
    case 0x2118:vram[destination*2]=value;++writes;if(!(vmain&128))++destination;break;
    case 0x2119:vram[destination*2+1]=value;++writes;if(vmain&128)++destination;break;
    case 0x2122:assert(colors<256);palette[colors++]=value;break;
    default:assert(0);
  }
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(cpu,bank,address,(uint8_t)value);cpu_write8(cpu,bank,address+1,(uint8_t)(value>>8));
}
static CpuState Prepare(unsigned kind,uint8_t status) {
  memset(ram,0x79,sizeof(ram));memset(vram,0xa9,sizeof(vram));colors=writes=0;
  CpuState cpu={0};cpu.PB=2;cpu.S=0x1dc;cpu.Y=0x1234;cpu.X=kind==2?0:0xa5;cpu.A=0x6700;
  cpu.P=CPU_P_C|(kind==1?0:CPU_P_M);cpu_p_to_mirrors(&cpu);
  cpu_write16(&cpu,0,0,0);cpu_write16(&cpu,0,2,kind?0x4000:256);
  cpu_write16(&cpu,0,0x18,0);
  const uint16_t addresses[]={0xba93,0x8300,0x8e7f};
  const uint8_t banks[]={0x1c,0x0b,5};
  cpu_write16(&cpu,0,0xa5,addresses[kind]);ram[0xa7]=banks[kind];
  if(kind==2) {ram[cpu.S+1]=0x34;ram[cpu.S+2]=0x12;ram[cpu.S+3]=status;}
  else ram[cpu.S+1]=status;
  destination=0;vmain=kind==2?0:0x80;
  return cpu;
}
int main(void) {
  for(unsigned i=0;i<sizeof(native);++i) {native[i]=(uint8_t)(i*13);donor[i]=(uint8_t)(i*7+31);}
  const ArRegionalMediaView view={.release=kArRegionalMediaRelease_Japan,.count=1,
      .entries={{kArRegionalMedia_TitleBackground,{donor,sizeof(donor)}}}};
  bool (*guards[])(CpuState *)={ActRaiser_TitlePaletteEntry,ActRaiser_TitleCharactersEntry,ActRaiser_TitleMapEntry};
  RecompReturn (*uploads[])(CpuState *)={ActRaiser_LoadTitlePalette,ActRaiser_LoadTitleCharacters,ActRaiser_LoadTitleMap};
  for(unsigned available=0;available<2;++available)for(unsigned enabled=0;enabled<2;++enabled) {
    ActRaiserRegionalMedia_ClearDonors();if(available)assert(ActRaiserRegionalMedia_AddDonor(&view));
    assert(ActRaiserRegionalMedia_AvailableArtwork()==(available?kArRegionalArtwork_TitleMask:0));
    for(unsigned status=0;status<256;++status)for(unsigned kind=0;kind<3;++kind) {
      requested=captured=enabled?kArRegionalArtwork_TitleMask:0;
      CpuState cpu=Prepare(kind,(uint8_t)status),before=cpu;
      uint8_t before_ram[sizeof(ram)];memcpy(before_ram,ram,sizeof(ram));
      assert(guards[kind](&cpu));assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(ram,before_ram,sizeof(ram)));
      unsigned prior=captures;assert(uploads[kind](&cpu)==RECOMP_RETURN_NORMAL);
      assert(captures==prior+(kind==0));
      assert(cpu.S==before.S+(kind==2?5:3) && cpu.P==status && cpu.Y==(status&CPU_P_X?0x34:0x1234) && cpu.DB==0 && cpu.D==0 && cpu.PB==2);
      unsigned expected_x=kind==0?before.X:kind==1?0:0x4000;
      assert(cpu.X==(status&CPU_P_X?expected_x&255:expected_x));
      const uint8_t *pixels=available&&enabled?donor:native;
      if(kind==0) {
        assert(colors==256 && writes==0 && !memcmp(palette,pixels+0x8000,256));
        assert(cpu.A==(0x6700|native[33023]));
      } else {
        assert(writes==0x4000 && colors==0 && destination==0x4000 && vmain==0x80);
        for(unsigned i=0;i<0x8000;++i) {
          unsigned lane=kind==1?1:0;
          uint8_t expected=i/2<0x4000 && (i&1)==lane?pixels[(kind==2?0x4000:0)+i/2]:0xa9;
          assert(vram[i]==expected);
        }
        for(unsigned i=0x8000;i<sizeof(vram);++i)assert(vram[i]==0xa9);
        assert(cpu.A==(kind==2?0x6780:(0x6700|native[0x3fff])));
      }
      if(kind!=2) {before_ram[before.S]=0x12;before_ram[before.S-1]=0x34;}
      assert(!memcmp(before_ram,ram,sizeof(ram)));
    }
  }
  for(unsigned kind=0;kind<3;++kind) {
    assert(!guards[kind](NULL));
    for(unsigned bad=0;bad<10;++bad) {
      CpuState cpu=Prepare(kind,0);
      switch(bad) {
        case 0:cpu.emulation=1;break;case 1:cpu.D=1;break;case 2:cpu.DB=1;break;
        case 3:cpu.PB=1;break;case 4:cpu.m_flag=!cpu.m_flag;break;case 5:cpu.x_flag=1;break;
        case 6:ram[0x18]=1;break;case 7:ram[0x19]=9;break;case 8:ram[0xa7]^=1;break;case 9:ram[0xa5]^=1;break;
      }
      assert(!guards[kind](&cpu));
    }
  }
  // A request after the palette must not mix planes from different regions.
  requested=0;captured=kArRegionalArtwork_TitleMask;
  CpuState cpu=Prepare(1,0);assert(ActRaiser_LoadTitleCharacters(&cpu)==RECOMP_RETURN_NORMAL);
  assert(vram[1]==donor[0] && captured==kArRegionalArtwork_TitleMask);
  ActRaiserRegionalMedia_ClearDonors();ArRegionalMediaView short_view=view;--short_view.entries[0].bytes.size;
  assert(ActRaiserRegionalMedia_AddDonor(&short_view));
  assert(!ActRaiserRegionalMedia_Title(true).characters.data && !ActRaiserRegionalMedia_AvailableArtwork());
  puts("title artwork: coherent planes, native CPU/stack/PPU, pending edits, fallback and guards passed");
  return 0;
}
