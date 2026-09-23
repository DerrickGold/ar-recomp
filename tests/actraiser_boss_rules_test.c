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
static CpuState Antlion(unsigned slot,unsigned state,unsigned value,unsigned flags) {
  memset(memory,0xa5,sizeof(memory));const unsigned x=0x6a0+slot*64;
  Write(0x18,0x0203);Write(x+0x32,0xc66f);Write(x+0x16,0x5000);
  memory[x+0x18]=0x7e;Write(x+0x1a,state);
  CpuState cpu={.A=value,.X=x,.Y=0x1aa2,.S=0x1ef0,.P=(uint8_t)flags};
  cpu_p_to_mirrors(&cpu);return cpu;
}
static void ExpectCompare(CpuState *cpu,unsigned value) {
  cpu->_flag_C=cpu->A>=value;
  cpu->P=(uint8_t)((cpu->P&~CPU_P_C)|(cpu->_flag_C?CPU_P_C:0));
  ActRaiserCpuHle_SetNegativeZero16(cpu,(uint16_t)(cpu->A-value));
}
static void CheckAntlion(void) {
  const unsigned slots[]={0,32,79},flags[]={0,CPU_P_V|CPU_P_C,CPU_P_Z,CPU_P_N};
  const unsigned values[]={0,63,64,65,2303,2304,2431,2432,65535};unsigned cases=0;
  for(unsigned trigger=0;trigger<3;++trigger)for(unsigned strategy=0;strategy<3;++strategy) {
    ArRegionalBossPolicy policy={{0}};policy.source[kArRegionalBoss_AntlionTrigger]=trigger;
    policy.source[kArRegionalBoss_AntlionStrategy]=strategy;assert(ArRegionalBoss_Resolve(&policy,&snapshot));
    for(unsigned slot=0;slot<3;++slot)for(unsigned f=0;f<4;++f)for(unsigned v=0;v<9;++v) {
      CpuState cpu=Antlion(slots[slot],0,values[v],flags[f]),expected=cpu;
      uint8_t wanted[sizeof(memory)];memcpy(wanted,memory,sizeof(wanted));
      assert(ActRaiser_AntlionTriggerEntry(&cpu)==(trigger==1));
      if(trigger==1) {
        ExpectCompare(&expected,2304);
        assert(ActRaiser_AntlionTrigger(&cpu)==RECOMP_RETURN_TAILCALL && target==0xc680 && origin==0xc67d);
      }
      assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(memory)));++cases;
      cpu=Antlion(slots[slot],4,values[v],flags[f]);expected=cpu;memcpy(wanted,memory,sizeof(wanted));
      assert(ActRaiser_AntlionVolleyEntry(&cpu)==(strategy==1));
      assert(ActRaiser_AntlionDecisionEntry(&cpu)==(strategy==1));
      if(strategy==1) {
        assert(ActRaiser_AntlionVolley(&cpu)==RECOMP_RETURN_TAILCALL && target==0xc71e && origin==0xc718);
        assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(memory)));
        ExpectCompare(&expected,64);
        const RecompReturn result=ActRaiser_AntlionDecision(&cpu);
        if(values[v]<64)assert(result==RECOMP_RETURN_TAILCALL && target==0xc726 && origin==0xc721);
        else {
          assert(result==RECOMP_RETURN_TAILCALL && target==0xc682 && origin==0xc721);
          ByteOrder_WriteLe16(wanted+cpu.X+6,0);ByteOrder_WriteLe16(wanted+cpu.X+8,0);
          ByteOrder_WriteLe16(wanted+cpu.X+0x24,60);ByteOrder_WriteLe16(wanted+cpu.X+0x12,0xc6e3);
          expected.A=0xc6e3;ActRaiserCpuHle_SetNegativeZero16(&expected,expected.A);
        }
      }
      assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(memory)));++cases;
    }
  }
  ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,1) && ArRegionalBoss_Resolve(&policy,&snapshot));
  for(unsigned decision=0;decision<2;++decision)for(unsigned bad=0;bad<16;++bad) {
    CpuState cpu=Antlion(0,decision?4:0,2432,0);
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x660;break;case 10:cpu.X=0x1aa0;break;case 11:Write(0x18,0x0103);break;
      case 12:Write(cpu.X+0x32,0xc80e);break;case 13:Write(cpu.X+0x16,0x4000);break;
      case 14:memory[cpu.X+0x18]=0x7f;break;case 15:Write(cpu.X+0x1a,12);break;
    }
    assert(!ActRaiser_AntlionTriggerEntry(&cpu) && !ActRaiser_AntlionVolleyEntry(&cpu) && !ActRaiser_AntlionDecisionEntry(&cpu));
  }
  printf("Antlion: %u mixed-policy/flag/slot decisions and 32 invalid-owner cases passed\n",cases);
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
    const unsigned trigger[]={0xc67d,0xc70c,0xc344,0xc346,0xc349};
    assert(rom[trigger[region]-0x8000]==0xc9 && ByteOrder_ReadLe16(rom+trigger[region]-0x7fff)==(region==1?2304:2432));
    const unsigned post[]={0xc718,0xc7a7,0xc3df,0xc3e1,0xc3e4};
    const uint8_t western[]={0xa9,12,0,0x20,0x57,0x86,0x20,0xbe,0x85,0xc9,64,0,0xb0,0xbd};
    const uint8_t japanese[]={0x20,0xad,0x85,0xc9,64,0,0x90,8,0xa9,60,0,0x20,0xe9,0x86,0x80,0xbb};
    uint8_t expected[sizeof(western)];memcpy(expected,western,sizeof(expected));
    if(region>=2){expected[4]=0x6f;expected[5]=0x85;expected[7]=0xd6;expected[8]=0x84;}
    assert(!memcmp(rom+post[region]-0x8000,region==1?japanese:expected,region==1?sizeof(japanese):sizeof(expected)));
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
static CpuState Dragon(unsigned state,unsigned remaining,bool initial) {
  CpuState cpu=Setup(0,0);Write(0x18,0x0304);Write(cpu.X+0x32,0xd646);
  Write(cpu.X+0x1a,initial?0:state);Write(cpu.X+0x38,state|(remaining<<8));
  Write(cpu.X+0x12,initial?0xa655:0x8661);Write(cpu.X+0x1e,0xa65d);Write(cpu.X,0x2000);
  return cpu;
}
static void CheckDragon(void) {
  unsigned cases=0;
  for(unsigned row=0;row<3;++row)for(unsigned flight=0;flight<3;++flight)
  for(unsigned state=1;state<=2;++state)for(unsigned count=0;count<6;++count)for(unsigned flags=0;flags<16;++flags) {
    ArRegionalBossPolicy policy={{0}};policy.source[kArRegionalBoss_DragonProjectileDelay]=row;
    policy.source[kArRegionalBoss_DragonProjectileFlight]=flight;assert(ArRegionalBoss_Resolve(&policy,&snapshot));
    CpuState cpu=Dragon(state,count,false);
    cpu.P=(uint8_t)((flags&1)|((flags&2))|((flags&4)<<4)|((flags&8)<<4));cpu_p_to_mirrors(&cpu);
    CpuState expected=cpu;uint8_t wanted[sizeof(memory)];memcpy(wanted,memory,sizeof(memory));
    const bool change=count>=1 && count<=4; /* Active actor survives a missing debug cache. */
    assert(ActRaiser_DragonFlightRepeatEntry(&cpu)==change);
    if(change) {
      expected.A=state;ActRaiserCpuHle_SetNegativeZero16(&expected,state);
      ByteOrder_WriteLe16(wanted+cpu.X+0x38,(uint16_t)(state|((count-1)<<8)));
      assert(ActRaiser_DragonFlightRepeat(&cpu)==RECOMP_RETURN_TAILCALL && target==0xa65b && origin==0xa65e);
    }
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(memory,wanted,sizeof(memory)));
    cpu=Dragon(state,0,true);expected=cpu;memcpy(wanted,memory,sizeof(memory));
    assert(ActRaiser_DragonFlightBeginEntry(&cpu)==(flight==2));
    if(flight==2) {
      expected.A=state;ActRaiserCpuHle_SetNegativeZero16(&expected,state);
      ByteOrder_WriteLe16(wanted+cpu.X,0);ByteOrder_WriteLe16(wanted+cpu.X+0x38,(uint16_t)(state|0x400));
      assert(ActRaiser_DragonFlightBegin(&cpu)==RECOMP_RETURN_TAILCALL && target==0xa65b && origin==0xa655);
    }
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(memory,wanted,sizeof(memory)));++cases;
  }
  ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,2) && ArRegionalBoss_Resolve(&policy,&snapshot));
  for(unsigned initial=0;initial<2;++initial)for(unsigned bad=0;bad<19;++bad) {
    CpuState cpu=Dragon(1,initial?0:4,initial);
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x1aa0;break;case 10:Write(0x18,0x0404);break;
      case 11:Write(cpu.X+0x32,0xd974);break;case 12:Write(cpu.X+0x16,0x4000);break;
      case 13:memory[cpu.X+0x18]=0x7f;break;case 14:Write(cpu.X+0x1a,3);break;
      case 15:Write(cpu.X+0x38,0);break;case 16:Write(cpu.X+0x38,0x501);break;
      case 17:Write(cpu.X+(initial?0x12:0x1e),0);break;case 18:Write(cpu.X+0x38,0x403);break;
    }
    assert(!(initial?ActRaiser_DragonFlightBeginEntry(&cpu):ActRaiser_DragonFlightRepeatEntry(&cpu)));
  }
  printf("dragon projectile: %u mixed-policy/phase/flag cases and 38 invalid contexts passed\n",cases);
}
static void CheckDragonRoms(char **paths) {
  static uint8_t rom[1048576],blobs[5][4096];
  for(unsigned r=0;r<5;++r) {
    FILE *f=fopen(paths[r],"rb");assert(f && fread(rom,1,sizeof(rom),f)==sizeof(rom) && !fclose(f));
    const unsigned off=248868,size=ByteOrder_ReadLe16(rom+off);
    assert(size<=4096 && QuintetLzss_DecompressAsset(rom+off,sizeof(rom)-off,blobs[r],size,NULL));
    ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,r>=2?2:r) && ArRegionalBoss_Resolve(&policy,&snapshot));
    for(unsigned state=1;state<=2;++state) {
      const uint8_t *us=blobs[0],*other=blobs[r];
      const unsigned a=ByteOrder_ReadLe16(us+2+2*state),b=ByteOrder_ReadLe16(other+2+2*state);
      uint16_t duration=us[a+1];
      assert(ArRegionalBoss_DragonRow(snapshot,state,0,us[a],&duration,Signed(us[a+2]),Signed(us[a+3]))==(r>=2));
      assert(duration==other[b+1] && us[a]==other[b] && !memcmp(us+a+2,other+b+2,3));
      const unsigned uc=ByteOrder_ReadLe16(us+ByteOrder_ReadLe16(us)+2*us[a]);
      const unsigned oc=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*other[b]);
      assert(!memcmp(us+uc,other+oc,5+7*us[uc+4]));
    }
    const unsigned entry=r>=2?0xa212:r?0xa614:0xa655,seq=r>=2?0x856f:r?0x8646:0x8657;
    assert(!memcmp(rom+entry-0x8000,(uint8_t[]){0x9e,0,0},3));
    const unsigned repeats=ArRegionalBoss_Value(snapshot,kArRegionalBoss_DragonProjectileFlight);
    for(unsigned i=0;i<repeats;++i) {
      const uint8_t *p=rom+entry-0x8000+3+6*i;
      assert(!memcmp(p,(uint8_t[]){0xbd,0x38,0,0x20},4) && ByteOrder_ReadLe16(p+4)==seq);
    }
    assert(!memcmp(rom+entry-0x8000+3+6*repeats,(uint8_t[]){0xbd,0x30,0,0x89,0,4,0xf0,0xf2},8));
  }
  puts("five-ROM dragon projectile rows, compositions and first offscreen-check contracts passed");
}
static CpuState Viper(unsigned encounter) {
  CpuState cpu=Setup(0,0);Write(0x18,encounter?0x0607:0x0805);Write(cpu.X+0x32,encounter?0xf72a:0xe483);
  Write(cpu.X+0x1a,10);return cpu;
}
static void CheckViper(void) {
  unsigned cases=0;
  for(unsigned region=0;region<3;++region)for(unsigned encounter=0;encounter<2;++encounter)
  for(unsigned value=0;value<260;++value)for(unsigned flags=0;flags<4;++flags) {
    const unsigned upper[]={0x100,0x101,0x8000,0xffff};
    const unsigned byte=value<256?value:upper[value-256];
    ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,region) && ArRegionalBoss_Resolve(&policy,&snapshot));
    CpuState cpu=Viper(encounter);cpu.A=byte;cpu.P=(uint8_t)(CPU_P_V|flags);cpu_p_to_mirrors(&cpu);
    CpuState expected=cpu;uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    assert(ActRaiser_ViperChoiceEntry(&cpu)==(region!=0));
    if(region!=0) {
      if(region==1) {
        expected._flag_C=byte&1;expected.P=(uint8_t)((expected.P&~CPU_P_C)|(byte&1));
        expected.A>>=1;
      } else expected.A&=1;
      ActRaiserCpuHle_SetNegativeZero16(&expected,expected.A);
      assert(ActRaiser_ViperChoice(&cpu)==RECOMP_RETURN_TAILCALL && origin==0xe4db && target==((byte&1)?0xe4f7:0xe4e0));
    }
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(memory,before,sizeof(memory)));++cases;
  }
  ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,1) && ArRegionalBoss_Resolve(&policy,&snapshot));
  for(unsigned encounter=0;encounter<2;++encounter)for(unsigned bad=0;bad<17;++bad) {
    CpuState cpu=Viper(encounter);
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x1aa0;break;case 10:memory[0x18]=4;break;case 11:memory[0x19]=1;break;
      case 12:Write(cpu.X+0x32,0xe5cf);break;case 13:Write(cpu.X+0x16,0x4000);break;
      case 14:memory[cpu.X+0x18]=0x7f;break;case 15:Write(cpu.X+0x1a,4);break;
      case 16:Write(cpu.X+0x32,encounter?0xe483:0xf72a);break;
    }
    assert(!ActRaiser_ViperChoiceEntry(&cpu));
  }
  printf("Viper: %u decision/flag/encounter cases and 34 invalid-owner checks passed\n",cases);
}
static void CheckViperRoms(char **paths) {
  const unsigned offsets[5][2]={{0xc8000,0xca81a},{0xc5f6d,0xc8800},{0xc8000,0xca81e},{0xc7332,0xca000},{0xc7332,0xca000}};
  const unsigned choices[]={0xe4db,0xe55c,0xe1da,0xe1dc,0xe1df};
  static uint8_t rom[1048576],blobs[5][2][8192];unsigned cases=0;
  for(unsigned r=0;r<5;++r) {
    FILE *f=fopen(paths[r],"rb");assert(f && fread(rom,1,sizeof(rom),f)==sizeof(rom) && !fclose(f));
    const uint8_t *choice=rom+choices[r]-0x8000;
    assert(!memcmp(choice,r==1?(uint8_t[]){0x4a,0xb0,0x17}:(uint8_t[]){0x29,r>=2?1:3,0,0xd0,0x17},r==1?3:5));
    ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,r>=2?2:r) && ArRegionalBoss_Resolve(&policy,&snapshot));
    for(unsigned encounter=0;encounter<2;++encounter) {
      const unsigned offset=offsets[r][encounter],size=ByteOrder_ReadLe16(rom+offset);
      assert(size<=sizeof(blobs[r][encounter]) && QuintetLzss_DecompressAsset(rom+offset,sizeof(rom)-offset,blobs[r][encounter],size,NULL));
      const uint8_t *us=blobs[0][encounter],*other=blobs[r][encounter];
      for(unsigned state=4;state<=9;++state) {
        const unsigned a=ByteOrder_ReadLe16(us+2+2*state),b=ByteOrder_ReadLe16(other+2+2*state);
        unsigned sum_us=0,sum_other=0;int displacement_us=0,displacement_other=0;
        for(unsigned row=0;us[a+4*row]!=255;++row) {
          const uint8_t *u=us+a+4*row,*o=other+b+4*row;uint16_t duration=u[1];int16_t dy=Signed(u[3]);
          (void)ArRegionalBoss_ViperRow(snapshot,encounter,state,row,u[0],&duration,Signed(u[2]),&dy);
          assert(u[0]==o[0] && duration==o[1] && u[2]==o[2] && dy==Signed(o[3]));
          const unsigned uc=ByteOrder_ReadLe16(us+ByteOrder_ReadLe16(us)+2*u[0]);
          const unsigned oc=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*o[0]);
          assert(!memcmp(us+uc,other+oc,5+7*us[uc+4]));++cases;
          if((state==8 || state==9) && row>=6 && row<=9) {
            sum_us+=u[1]+1;sum_other+=duration+1;
            displacement_us+=(u[1]+1)*Signed(u[3]);displacement_other+=(duration+1)*dy;
          }
        }
        if(!encounter && (state==8 || state==9))assert(sum_us==22 && sum_other==(r>=2?15:22) && displacement_us==110 && displacement_other==110);
      }
    }
  }
  printf("Viper: %u five-ROM rows/compositions, floor displacement and RNG prefix signatures verified\n",cases);
}
static CpuState Pharaoh(unsigned encounter,unsigned state) {
  CpuState cpu=Setup(0,0);Write(0x18,encounter?0x0407:0x0603);
  Write(cpu.X+0x32,encounter?0xf6fa:0xc1a2);Write(cpu.X+0x1a,state);
  Write(cpu.X+0x1e,state==3?0xc2d0:0xc2a6);return cpu;
}
static void CheckPharaoh(void) {
  unsigned cases=0;
  for(unsigned mix=0;mix<27;++mix)for(unsigned encounter=0;encounter<2;++encounter)
  for(unsigned phase=0;phase<2;++phase)for(unsigned flags=0;flags<32;++flags) {
    ArRegionalBossPolicy policy={{0}};unsigned digits=mix;
    for(unsigned i=19;i<22;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalBoss_Resolve(&policy,&snapshot));
    CpuState cpu=Pharaoh(encounter,phase?3:2);cpu.P=(uint8_t)((flags&7)|((flags&24)<<3));cpu_p_to_mirrors(&cpu);
    cpu.Y=flags&1?0x1aa0:0x920; /* Full pool or real child, never reallocated. */
    CpuState expected=cpu;uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    const bool changed=phase || policy.source[kArRegionalBoss_PharaohHeads]==1;
    assert((phase?ActRaiser_PharaohHeadRepeatEntry(&cpu):ActRaiser_PharaohHeadIdleEntry(&cpu))==changed);
    if(changed) {
      if(!phase){expected.A=3;ActRaiserCpuHle_SetNegativeZero16(&expected,3);}
      assert((phase?ActRaiser_PharaohHeadRepeat(&cpu):ActRaiser_PharaohHeadIdle(&cpu))==RECOMP_RETURN_TAILCALL);
      assert(target==(phase?0xc2a1:0xc2ce) && origin==(phase?0xc2d1:0xc2cb));
    }
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(memory,before,sizeof(memory)));++cases;
  }
  snapshot=UINT64_C(1)<<(2*kArRegionalBoss_PharaohHeads);
  for(unsigned encounter=0;encounter<2;++encounter)for(unsigned phase=0;phase<2;++phase)
  for(unsigned bad=0;bad<18;++bad) {
    CpuState cpu=Pharaoh(encounter,phase?3:2);
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x1aa0;break;case 10:memory[0x18]=2;break;case 11:memory[0x19]=1;break;
      case 12:Write(cpu.X+0x32,0xc8c9);break;case 13:Write(cpu.X+0x16,0x4000);break;
      case 14:memory[cpu.X+0x18]=0x7f;break;case 15:Write(cpu.X+0x1a,phase?27:4);break;
      case 16:Write(cpu.X+0x32,encounter?0xc1a2:0xf6fa);break;
      case 17:if(phase)Write(cpu.X+0x1e,0xc2a6);else Write(cpu.X+0x1a,11);break;
    }
    assert(!(phase?ActRaiser_PharaohHeadRepeatEntry(&cpu):ActRaiser_PharaohHeadIdleEntry(&cpu)));
  }
  assert(!ActRaiser_PharaohHeadIdleEntry(NULL) && !ActRaiser_PharaohHeadRepeatEntry(NULL));
  CpuState cpu=Pharaoh(0,3);snapshot=UINT64_MAX;
  assert(ActRaiser_PharaohHeadRepeatEntry(&cpu)); /* In-flight return needs no cache. */
  cpu=Pharaoh(0,2);assert(!ActRaiser_PharaohHeadIdleEntry(&cpu));
  printf("Pharaoh: %u mixed-policy/phase/flag/encounter cases, 72 invalid contexts and lost-cache completion passed\n",cases);
}
static void CheckPharaohRoms(char **paths) {
  const unsigned offsets[5][2]={{0xdd27a,0xdce6a},{0xdd132,0xdcd45},{0xdc8e9,0xdc4d9},{0xdc43c,0xdc02c},{0xdc43c,0xdc02c}};
  const unsigned heads[]={0xc2cb,0xc362,0xbf92,0xbf94,0xbf97};
  static uint8_t rom[1048576],blobs[5][2][4096];unsigned cases=0;
  for(unsigned region=0;region<5;++region) {
    FILE *f=fopen(paths[region],"rb");assert(f && fread(rom,1,sizeof(rom),f)==sizeof(rom) && !fclose(f));
    const uint8_t *head=rom+heads[region]-0x8000;
    assert(head[0]==0xa9 && head[1]==(region==1?3:27) && !head[2] && head[3]==0x20);
    assert(ByteOrder_ReadLe16(head+4)==(region==1?0x8646:region?0x856f:0x8657));
    if(region==1)assert(!memcmp(head+6,(uint8_t[]){0x80,0xce},2));
    else assert(!memcmp(head+6,(uint8_t[]){0x20,region?0xcf:0xb7,region?0x84:0x85,0x60},4));
    ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,region==1?1:region?2:0) && ArRegionalBoss_Resolve(&policy,&snapshot));
    for(unsigned encounter=0;encounter<2;++encounter) {
      const unsigned at=offsets[region][encounter],size=ByteOrder_ReadLe16(rom+at);
      assert(size<=4096 && QuintetLzss_DecompressAsset(rom+at,sizeof(rom)-at,blobs[region][encounter],size,NULL));
      const uint8_t *us=blobs[0][encounter],*other=blobs[region][encounter];
      const unsigned states[]={2,3,4,10,11,13,14,15,25};
      for(unsigned j=0;j<sizeof(states)/sizeof(states[0]);++j) {
        const unsigned state=states[j],a=ByteOrder_ReadLe16(us+2+state*2),b=ByteOrder_ReadLe16(other+2+state*2);
        unsigned total=0,row=0,native_row=0;
        while(true) {
          unsigned next;if(ArRegionalBoss_PharaohSkip(snapshot,encounter,state,row,&next))row=next;
          const uint8_t *u=us+a+row*4,*o=other+b+native_row*4;
          assert(u[0]==o[0]);if(u[0]==255)break;
          assert(!memcmp(u,o,4));total+=u[1]+1;
          const unsigned uc=ByteOrder_ReadLe16(us+ByteOrder_ReadLe16(us)+2*u[0]);
          const unsigned oc=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*o[0]);
          assert(!memcmp(us+uc,other+oc,5+7*us[uc+4]));++cases;++row;++native_row;
        }
        if(state==11)assert(total==ArRegionalBoss_Value(snapshot,encounter?kArRegionalBoss_PharaohRematchLanding:kArRegionalBoss_PharaohLanding));
      }
    }
  }
  printf("Pharaoh: %u five-ROM rows/compositions and repeat/retirement prefixes verified\n",cases);
}
static CpuState Plant(unsigned previous) {
  CpuState cpu=Setup(0,0);Write(0x18,0x0305);Write(cpu.X+0x32,0xd974);
  Write(cpu.X+0x1a,previous);return cpu;
}
static void CheckPlant(void) {
  const unsigned previous[]={0,1,2,3,20},next[]={1,2,3,20,1},repeats[]={1,5,1,1,1};unsigned cases=0;
  for(unsigned mix=0;mix<81;++mix)for(unsigned phase=0;phase<5;++phase)
  for(unsigned flags=0;flags<32;++flags)for(unsigned protection=0;protection<2;++protection) {
    ArRegionalBossPolicy policy={{0}};unsigned digits=mix;
    for(unsigned i=22;i<26;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalBoss_Resolve(&policy,&snapshot));
    CpuState cpu=Plant(previous[phase]);cpu.P=(uint8_t)((flags&7)|((flags&24)<<3));cpu_p_to_mirrors(&cpu);
    Write(cpu.X+0x30,0x9531u&~(protection?0u:0x20u));
    const bool changed=policy.source[22]!=0 || (previous[phase]==20 && protection);
    uint8_t before[sizeof(memory)],wanted[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    CpuState expected=cpu;
    assert(ActRaiser_PlantPhaseEntry(&cpu)==changed);
    if(changed) {
      const unsigned st=policy.source[22]?next[phase]:2,count=policy.source[22]?repeats[phase]:99;
      expected.A=(uint16_t)(st<<8|count);ActRaiserCpuHle_SetNegativeZero16(&expected,expected.A);
      unsigned bits=ByteOrder_ReadLe16(memory+cpu.X+0x30);
      if(st==20)bits|=0x20;else if(previous[phase]==20)bits&=~0x20;
      Write(cpu.X+0x30,(uint16_t)bits);
    }
    memcpy(wanted,memory,sizeof(memory));memcpy(memory,before,sizeof(memory));
    if(changed)assert(ActRaiser_PlantPhase(&cpu)==RECOMP_RETURN_TAILCALL && target==0xd9de && origin==0xd9db);
    assert(!memcmp(memory,wanted,sizeof(memory)) && !memcmp(&cpu,&expected,sizeof(cpu)));++cases;
  }
  snapshot=UINT64_C(1)<<(2*kArRegionalBoss_PlantCycle);
  for(unsigned bad=0;bad<19;++bad) {
    CpuState cpu=Plant(0);
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x1aa0;break;case 10:memory[0x18]=4;break;case 11:memory[0x19]=1;break;
      case 12:Write(cpu.X+0x32,0xe483);break;case 13:Write(cpu.X+0x16,0x4000);break;
      case 14:memory[cpu.X+0x18]=0x7f;break;case 15:Write(cpu.X+0x1a,4);break;
      case 16:Write(cpu.X+0x3a,0x920);break;case 17:Write(cpu.X+0x1a,0xff);break;
      case 18:Write(cpu.X+0x1a,6);break;
    }
    assert(!ActRaiser_PlantPhaseEntry(&cpu));
  }
  assert(!ActRaiser_PlantPhaseEntry(NULL));
  CpuState cpu=Plant(20);Write(cpu.X+0x30,0x20);snapshot=UINT64_MAX;
  assert(ActRaiser_PlantPhaseEntry(&cpu) && ActRaiser_PlantPhase(&cpu)==RECOMP_RETURN_TAILCALL);
  assert(!ByteOrder_ReadLe16(memory+cpu.X+0x30) && cpu.A==0x263);
  printf("Plant phases: %u mixed-policy/flag/protection cases, 19 invalid contexts and cacheless closed recovery passed\n",cases);
}
static void CheckPlantRoms(char **paths) {
  static uint8_t rom[1048576],blobs[5][8192];unsigned cases=0;
  const unsigned starts[]={0xd9db,0xda53,0xd69c,0xd69e,0xd6a1};
  for(unsigned region=0;region<5;++region) {
    FILE *f=fopen(paths[region],"rb");assert(f && fread(rom,1,sizeof(rom),f)==sizeof(rom) && !fclose(f));
    const unsigned offset=region==1?0xbdaf9:0xa749d,size=ByteOrder_ReadLe16(rom+offset);
    assert(size<=sizeof(blobs[region]) && QuintetLzss_DecompressAsset(rom+offset,sizeof(rom)-offset,blobs[region],size,NULL));
    const uint8_t *code=rom+starts[region]-0x8000;
    if(!region)assert(!memcmp(code,(uint8_t[]){0xa9,99,2,0x20,0x69,0x86,0x80,0xf8},8));
    else {
      assert(code[0]==0xa9 && ByteOrder_ReadLe16(code+1)==1 && code[3]==0x20);
      assert(code[6]==0xa9 && ByteOrder_ReadLe16(code+7)==0x205 && code[9]==0x20);
      assert(code[12]==0xa9 && ByteOrder_ReadLe16(code+13)==3 && code[15]==0x20);
      assert(!memcmp(code+18,(uint8_t[]){0xbd,0x30,0,9,0x20,0,0x9d,0x30,0,0xa9,20,0},12));
      assert(!memcmp(code+33,(uint8_t[]){0xbd,0x30,0,0x29,0xdf,0xff,0x9d,0x30,0,0x80,0xd4},11));
    }
    ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,region==1?1:region?2:0) && ArRegionalBoss_Resolve(&policy,&snapshot));
    const uint8_t *us=blobs[0],*other=blobs[region];
    const unsigned states[]={1,2,3,4,6,20};
    for(unsigned j=0;j<6;++j) {
      const unsigned state=states[j],a=ByteOrder_ReadLe16(us+2+state*2),b=ByteOrder_ReadLe16(other+2+state*2);
      unsigned total=0;
      for(unsigned row=0;;++row) {
        unsigned urow=row;uint8_t visual=us[a+row*4];
        if(state==2)(void)ArRegionalBoss_PlantOpenRow(snapshot,row,&urow,&visual);
        const uint8_t *u=us+a+urow*4,*o=other+b+row*4;
        assert(visual==o[0]);if(visual==255)break;
        uint16_t duration=u[1];(void)ArRegionalBoss_PlantWindup(snapshot,state,row,visual,&duration,Signed(u[2]),Signed(u[3]));
        assert(duration==o[1] && u[2]==o[2] && u[3]==o[3]);total+=duration+1;
        /* JP body/closed artwork differs; that geometry remains a media leaf. */
        if(state<=3) {
          const unsigned uc=ByteOrder_ReadLe16(us+ByteOrder_ReadLe16(us)+2*visual);
          const unsigned oc=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*visual);
          assert(!memcmp(us+uc,other+oc,5+7*us[uc+4]));
        }
        ++cases;
      }
      if(state==2)assert(total==(region>=2?16:8));
      else if(state==4 || state==6)assert(total==(region>=2?25:9));
      else assert(total==(state==20?90:12));
    }
  }
  printf("Plant: %u five-ROM rows and exposure-program signatures verified; body artwork explicitly separate\n",cases);
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);CheckPrefixes();CheckClock();CheckAntlion();CheckDragon();
  CheckViper();CheckPharaoh();CheckPlant();if(argc==6){CheckOriginal(argv+1);CheckDragonRoms(argv+1);CheckViperRoms(argv+1);CheckPharaohRoms(argv+1);CheckPlantRoms(argv+1);}
  puts("boss rules: native prefix and mixed-policy checks passed");return 0;
}
