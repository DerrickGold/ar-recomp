#include "actraiser/actraiser_boss_rules.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "regional/regional_boss_rules.h"
#include "quintet_lzss.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t memory[65536];
static uint64_t snapshot;
static unsigned target,origin;
uint64_t ActRaiserRegional_BossSnapshot(void){return snapshot;}
int cpu_hle_tailcall_request(uint32_t pc,uint32_t from){target=pc;origin=from;return 1;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at){(void)cpu;assert(!bank);return memory[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at){return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value){(void)cpu;assert(!bank);memory[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value){cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
static void Write(unsigned at,uint16_t value){ByteOrder_WriteLe16(memory+at,value);}
static CpuState Setup(unsigned wizard,unsigned encounter) {
  memset(memory,0,sizeof(memory));memory[0x18]=encounter?7:wizard?2:1;
  Write(0x912,wizard?(encounter?0xf6e2:0xbdff):(encounter?0xf6ca:0xaf5d));
  Write(0x8f6,0x5000);memory[0x8f8]=0x7e;Write(0x8fa,wizard?11:1);
  CpuState cpu={.X=0x8e0,.Y=0x1aa2,.S=0x1ef0,.A=0x1234,.P=CPU_P_C|CPU_P_V|CPU_P_Z};
  cpu_p_to_mirrors(&cpu);return cpu;
}
static void CheckPrefixes(void) {
  assert(ArRegionalBoss_Value(UINT64_MAX,0)==UINT16_MAX);
  assert(ArRegionalBoss_Value(UINT64_C(3)<<10,0)==UINT16_MAX);
  assert(ArRegionalBoss_Value(0,kArRegionalBoss_Count)==UINT16_MAX);
  for(unsigned n=0;n<729;++n)for(unsigned wizard=0;wizard<2;++wizard)for(unsigned encounter=0;encounter<2;++encounter) {
    unsigned digits=n;ArRegionalBossPolicy policy={{0}};
    for(unsigned i=0;i<6;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalBoss_Resolve(&policy,&snapshot));
    CpuState cpu=Setup(wizard,encounter),expected=cpu;
    const bool changed=policy.source[wizard?kArRegionalBoss_WizardPause:kArRegionalBoss_MinoAxeOffset]==1;
    uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    assert((wizard?ActRaiser_WizardPauseEntry(&cpu):ActRaiser_MinotaurAxeOffsetEntry(&cpu))==changed);
    if(changed) {
      assert((wizard?ActRaiser_WizardPause(&cpu):ActRaiser_MinotaurAxeOffset(&cpu))==RECOMP_RETURN_TAILCALL);
      assert(target==(wizard?0xbe7e:0xafde) && origin==(wizard?0xbe78:0xafdb));
      if(!wizard){expected.A=(uint16_t)-48;ActRaiserCpuHle_SetNegativeZero16(&expected,expected.A);}
    }
    assert(!memcmp(&expected,&cpu,sizeof(cpu)) && !memcmp(before,memory,sizeof(memory)));
  }
  ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,1) && ArRegionalBoss_Resolve(&policy,&snapshot));
  for(unsigned wizard=0;wizard<2;++wizard)for(unsigned bad=0;bad<15;++bad) {
    CpuState cpu=Setup(wizard,0);
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x1aa0;break;case 10:memory[0x18]=7;break;
      case 11:Write(0x912,wizard?0xf6e2:0xf6ca);break; /* rematch source in original room */
      case 12:Write(0x8f6,0x4000);break;case 13:memory[0x8f8]=0x7f;break;
      case 14:Write(0x8fa,wizard?0x12:3);break; /* inherited-source projectile */
    }
    assert(!ActRaiser_WizardPauseEntry(&cpu) && !ActRaiser_MinotaurAxeOffsetEntry(&cpu));
  }
}
static int16_t Signed(uint8_t value){return value<128?value:(int16_t)((int)value-256);}
static CpuState Clock(void) {
  CpuState cpu=Setup(0,1);cpu.P|=CPU_P_M;cpu_p_to_mirrors(&cpu);
  Write(0x18,0x0807);Write(cpu.X,0x800);Write(cpu.X+0x32,0xf80f);
  Write(cpu.X+0x12,0xf8f5);Write(cpu.X+0x14,0xf8f5);Write(cpu.X+0x1a,0xff);
  Write(cpu.X+0x2c,0);Write(cpu.X+0x30,0x32);return cpu;
}
static void CheckClock(void) {
  for(unsigned region=0;region<3;++region)for(unsigned gate=0;gate<256;++gate) {
    ArRegionalBossPolicy policy={{0}};policy.source[kArRegionalBoss_TanzraClock]=region;
    assert(ArRegionalBoss_Resolve(&policy,&snapshot));CpuState cpu=Clock();
    Write(0xe5,0x973b);Write(0xe7,0xa500|(uint16_t)gate<<8);memory[0xe9]=0xb6;
    uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(memory));const CpuState expected=cpu;
    assert(ActRaiser_TanzraClockEntry(&cpu)==(region==1));
    if(region==1)assert(ActRaiser_TanzraClock(&cpu)==RECOMP_RETURN_TAILCALL && target==0xf8fe && origin==0xf8fc);
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(before,memory,sizeof(memory)));
  }
  snapshot=UINT64_C(1)<<(2*kArRegionalBoss_TanzraClock);
  for(unsigned bad=0;bad<21;++bad) {
    CpuState cpu=Clock();
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=0;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x1aa0;break;case 10:memory[0x19]=7;break;case 11:memory[0x18]=6;break;
      case 12:Write(0x912,0xf760);break;case 13:Write(0x8f6,0x4000);break;case 14:memory[0x8f8]=0x7f;break;
      case 15:Write(0x8fa,10);break;case 16:Write(cpu.X,0);break;
      case 17:Write(cpu.X+0x12,0xfd25);break;case 18:Write(cpu.X+0x14,0xfe89);break;
      case 19:Write(cpu.X+0x2c,1);break;case 20:Write(cpu.X+0x30,0x20);break;
    }
    assert(!ActRaiser_TanzraClockEntry(&cpu));
  }
  assert(!ActRaiser_TanzraClockEntry(NULL));
}
static void CheckOriginal(char **paths) {
  const unsigned offsets[5][2]={{0xcb017,0xcc778},{0xc8ffd,0xcaf2b},{0xcb01a,0xcc779},{0xca7fc,0xcbf5b},{0xca7fc,0xcbf5b}};
  const unsigned axe_pc[]={0xafdb,0xb06f,0xac85,0xac87,0xac8a};
  const unsigned wizard_pc[]={0xbe78,0xbf15,0xbb39,0xbb3b,0xbb3e};
  const unsigned sequence[]={0x8657,0x8646,0x856f,0x856f,0x856f};
  const unsigned delay[]={0x86fa,0,0x8612,0x8612,0x8612};
  const unsigned facing[]={0x8709,0x86f8,0x8621,0x8621,0x8621};
  const unsigned ice_offsets[5][2]={{0xc3137,0xc432c},{0xc091e,0xc23fc},{0xc26a5,0xc389a},{0xc1cb8,0xc2ead},{0xc1cb8,0xc2ead}};
  const unsigned final_offsets[]={0xc7727,0xc46c9,0xc74e8,0xc6afb,0xc6afb};
  static uint8_t final[5][4096];unsigned final_cases=0;
  static uint8_t rom[1048576],blobs[5][2][65536],ice[5][2][65536];unsigned cases=0,ice_cases=0;
  for(unsigned region=0;region<5;++region) {
    FILE *file=fopen(paths[region],"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
    const uint8_t *axe=rom+axe_pc[region]-0x8000,*wizard=rom+wizard_pc[region]-0x8000;
    ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,region>=2?2:region) && ArRegionalBoss_Resolve(&policy,&snapshot));
    const unsigned foff=final_offsets[region];const uint16_t fsize=ByteOrder_ReadLe16(rom+foff);
    assert(fsize<=sizeof(final[region]) && QuintetLzss_DecompressAsset(rom+foff,sizeof(rom)-foff,final[region],fsize,NULL));
    const uint8_t *fu=final[0],*fo=final[region];
    const unsigned fst[]={5,10,22,48};
    for(unsigned si=0;si<4;++si) {
      const unsigned state=fst[si],ub=ByteOrder_ReadLe16(fu+2+2*state),ob=ByteOrder_ReadLe16(fo+2+2*state);
      unsigned row=0,j=0,total=0;int dx_sum=0;
      while(fu[ub+4*row]!=255) {
        (void)ArRegionalBoss_TanzraMinionSkip(snapshot,state,row,&row);
        const unsigned a=ub+4*row,b=ob+4*j;uint16_t duration=fu[a+1];
        (void)ArRegionalBoss_TanzraRow(snapshot,state,row,&duration,Signed(fu[a+2]),Signed(fu[a+3]));
        assert(fo[b]!=255 && fu[a]==fo[b] && duration==fo[b+1] && fu[a+2]==fo[b+2] && fu[a+3]==fo[b+3]);
        const unsigned uc=ByteOrder_ReadLe16(fu+ByteOrder_ReadLe16(fu)+2*fu[a]);
        const unsigned oc=ByteOrder_ReadLe16(fo+ByteOrder_ReadLe16(fo)+2*fo[b]);
        assert(!memcmp(fu+uc,fo+oc,4));total+=duration+1;dx_sum+=Signed(fu[a+2])*(duration+1);
        ++row;++j;++final_cases;
      }
      assert(fo[ob+4*j]==255);
      if(state==5)assert(total==36);
      if(state==10)assert(total==ArRegionalBoss_Value(snapshot,kArRegionalBoss_TanzraClosing)+33u);
      if(state==22)assert(total==ArRegionalBoss_Value(snapshot,kArRegionalBoss_TanzraMinionTurn));
      if(state==48)assert(total==ArRegionalBoss_Value(snapshot,kArRegionalBoss_TanzraUpperTurn)+27u && dx_sum==(region==1?-72:-68));
    }
    const unsigned clock[]={0xf8f5,0xf974,0xf5ee,0xf5f0,0xf5f3};const uint8_t *pc=rom+clock[region]-0x8000;
    const uint8_t us_clock[]={0xe2,0x20,0x9c,0xed,0,0x64,0xf8,0x64,0xe8,0xc2,0x20};
    const uint8_t jp_clock[]={0xe2,0x20,0x9c,0xf0,0,0x64,0xfb,0xc2,0x20};
    const uint8_t pal_clock[]={0xe2,0x20,0x9c,0xee,0,0x64,0xf9,0x64,0xe9,0xc2,0x20};
    assert(!memcmp(pc,region==1?jp_clock:region?pal_clock:us_clock,region==1?sizeof(jp_clock):sizeof(us_clock)));
    assert(axe[0]==0xa9 && ByteOrder_ReadLe16(axe+1)==(uint16_t)-ArRegionalBoss_Value(snapshot,kArRegionalBoss_MinoAxeOffset));
    assert(axe[3]==0x20 && ByteOrder_ReadLe16(axe+4)==facing[region]);
    assert(wizard[-6]==0xa9 && ByteOrder_ReadLe16(wizard-5)==11 && wizard[-3]==0x20 && ByteOrder_ReadLe16(wizard-2)==sequence[region]);
    if(region==1)assert(wizard[0]==0x20 && ByteOrder_ReadLe16(wizard+1)==0x85bc && !ArRegionalBoss_Value(snapshot,kArRegionalBoss_WizardPause));
    else assert(wizard[0]==0xa9 && ByteOrder_ReadLe16(wizard+1)+1==ArRegionalBoss_Value(snapshot,kArRegionalBoss_WizardPause) &&
        wizard[3]==0x20 && ByteOrder_ReadLe16(wizard+4)==delay[region]);
    for(unsigned encounter=0;encounter<2;++encounter) {
      const unsigned offset=offsets[region][encounter],size=ByteOrder_ReadLe16(rom+offset);
      uint8_t *other=blobs[region][encounter];const uint8_t *us=blobs[0][encounter];
      assert(QuintetLzss_DecompressAsset(rom+offset,sizeof(rom)-offset,other,size,NULL));
      for(unsigned state=0;state<9;++state) {
        const unsigned base=ByteOrder_ReadLe16(us+2+2*state),target_row=ByteOrder_ReadLe16(other+2+2*state);
        for(unsigned row=0;;++row) {
          const unsigned a=base+4*row,b=target_row+4*row;
          if(us[a]==255){assert(other[b]==255);break;}
          uint16_t duration=us[a+1];
          if(!encounter)(void)ArRegionalBoss_MinoRow(snapshot,state,row,&duration,Signed(us[a+2]),Signed(us[a+3]));
          assert(duration==other[b+1] && us[a]==other[b] && us[a+2]==other[b+2] && us[a+3]==other[b+3]);
          const unsigned uc=ByteOrder_ReadLe16(us+ByteOrder_ReadLe16(us)+2*us[a]);
          const unsigned oc=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*other[b]);
          assert(!memcmp(us+uc,other+oc,4));++cases;
        }
      }
    }
    for(unsigned encounter=0;encounter<2;++encounter) {
      const unsigned offset=ice_offsets[region][encounter],size=ByteOrder_ReadLe16(rom+offset);
      uint8_t *other=ice[region][encounter];const uint8_t *us=ice[0][encounter];
      assert(QuintetLzss_DecompressAsset(rom+offset,sizeof(rom)-offset,other,size,NULL));
      const unsigned states[]={17,18,25,26};
      for(unsigned si=0;si<4;++si) {
        const unsigned state=states[si],base=ByteOrder_ReadLe16(us+2+2*state),target_row=ByteOrder_ReadLe16(other+2+2*state);
        unsigned frames=0;int dy=0;
        for(unsigned row=0,logical=0;;++row,++logical) {
          unsigned next;
          if(encounter && ArRegionalBoss_IceSkip(snapshot,state,row,&next))row=next;
          const unsigned a=base+4*row,b=target_row+4*logical;
          if(us[a]==255){assert(other[b]==255);break;}
          assert(!memcmp(us+a,other+b,4));
          const unsigned uc=ByteOrder_ReadLe16(us+ByteOrder_ReadLe16(us)+2*us[a]);
          const unsigned oc=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*other[b]);
          assert(!memcmp(us+uc,other+oc,4));++ice_cases;
          frames+=us[a+1]+1;dy+=Signed(us[a+3])*(us[a+1]+1);
        }
        if(si<2)assert(frames==(encounter?ArRegionalBoss_Value(snapshot,kArRegionalBoss_IceWindup):118) && dy==262);
        else assert(frames==60); /* Shared accelerated rematch projectiles are NOT a regional edit. */
      }
    }
  }
  printf("five-ROM Minotaur original/rematch: %u rows, velocities and collision headers; axe and Wizard prefixes verified\n",cases);
  printf("five-ROM Ice Dragon head/body and projectile timelines: %u rows verified\n",ice_cases);
  printf("five-ROM Tanzra opening/closing/turn/minion: %u rows, extents/displacement and phase-two clock prefixes verified\n",final_cases);
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);CheckPrefixes();CheckClock();if(argc==6)CheckOriginal(argv+1);
  puts("boss rules: native prefix and mixed-policy checks passed");return 0;
}
