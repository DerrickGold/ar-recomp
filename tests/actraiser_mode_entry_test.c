#include "actraiser/actraiser_mode_entry.h"
#include "actraiser/actraiser_native_call.h"
#include "regional/regional_mode_entry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t ram[65536],town[65536],rules;
static unsigned target,owner,waits,clears,returns;
static RecompReturn wait_result;
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) {(void)cpu;assert(!bank || bank==0x7f);return bank?town[at]:ram[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) {(void)cpu;assert(!bank || bank==0x7f);(bank?town:ram)[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) {cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
int cpu_hle_tailcall_request(uint32 pc,uint32 site) {target=pc;owner=site;return true;}
int cpu_begin_reset_tail(CpuState *cpu,uint32 pc,uint32 site) {
  assert(pc==0x008024 && site==0x02ab04 && cpu->S==0x1ff);
  assert(cpu_read16(cpu,0,0x1fd)==0x8023 && !ram[0x1ff]);cpu->PB=0;return true;
}
bool ActRaiserRegional_ModeEntry(bool activate,uint8_t *snapshot) {(void)activate;*snapshot=rules;return true;}
bool ActRaiserRegional_ReturnToTitle(void) {++returns;return true;}
RecompReturn ActRaiser_WaitForVblank(CpuState *cpu) {(void)cpu;++waits;return wait_result;}
RecompReturn ActRaiserNativeCall(CpuState *cpu,ActRaiserNativeLeaf leaf,uint8_t bank,uint16_t caller,bool long_call) {
  assert(leaf==ActRaiser_WaitForVblank && bank==2 && caller==0xaaf1 && !long_call);return leaf(cpu);
}
RecompReturn bank_02_ABC4_M1X0(CpuState *cpu) {
  ++clears;for(unsigned i=0;i<0x700;i+=2)cpu_write16(cpu,0x7f,0xb000+i,0x2000);
  return RECOMP_RETURN_NORMAL;
}
int cpu_invoke_rts_leaf(CpuState *cpu,RecompReturn (*leaf)(CpuState *),uint32 site) {
  assert(leaf==bank_02_ABC4_M1X0 && site==0x02abc4);return leaf(cpu)==RECOMP_RETURN_NORMAL;
}
static CpuState State(unsigned p) {
  CpuState cpu={.PB=2,.DB=2,.A=0xabcc,.X=0x1234,.Y=0x5678,.S=0x1fb,.P=p};cpu_p_to_mirrors(&cpu);return cpu;
}
static void Prefixes(void) {
  for(unsigned r=0;r<4;++r)for(unsigned p=0;p<256;++p) {
    if(!(p&CPU_P_M) || p&CPU_P_X)continue;
    CpuState cpu=State(p),before=cpu;rules=r;
    assert(ActRaiser_ModeTitleGateEntry(&cpu));
    assert(ActRaiser_ModeTitleGate(&cpu)==RECOMP_RETURN_TAILCALL);
    assert(owner==0x02a70d && target==(!(p&CPU_P_C) || r&1?0x02a72f:0x02a70f));
    assert(!memcmp(&cpu,&before,sizeof(cpu)));
    assert(ActRaiser_ModeInitialChoiceEntry(&cpu)==((r&1)!=0));
    if(!(r&1))continue;
    assert(ActRaiser_ModeInitialChoice(&cpu)==RECOMP_RETURN_TAILCALL && target==0x02a756);
    assert(ram[0x336]==!(p&CPU_P_C) && cpu.A==(0xab00|!(p&CPU_P_C)) && cpu.S==before.S);
    for(unsigned choice=0;choice<3;++choice) {
      ram[0x336]=choice;cpu=before;
      assert(ActRaiser_ModeNextChoice(&cpu)==RECOMP_RETURN_TAILCALL && target==0x02a813);
      const unsigned expected=choice==0?2:choice==2 && !(p&CPU_P_C)?1:0;
      assert(cpu.A==(0xab00|expected) && cpu.X==expected && cpu.Y==before.Y && cpu.S==before.S);
      assert((cpu.P&~(CPU_P_N|CPU_P_Z))==(p&~(CPU_P_N|CPU_P_Z)));
    }
    cpu=before;cpu.P&=~CPU_P_M;cpu_p_to_mirrors(&cpu);
    assert(ActRaiser_ModeInitialLabelEntry(&cpu)==((p&CPU_P_C)!=0));
    if(p&CPU_P_C)assert(ActRaiser_ModeInitialLabel(&cpu)==RECOMP_RETURN_TAILCALL && cpu.Y==0xaa4a && target==0x02a74b);
  }
  rules=2;
  /* Choices can be enabled after the checksum gate, while Start is already
   * on screen, without enabling Continue or touching the SRAM marker. */
  CpuState late=State(CPU_P_M|CPU_P_C);rules=0;
  assert(ActRaiser_ModeTitleGate(&late)==RECOMP_RETURN_TAILCALL && target==0x02a70f);
  assert(!ActRaiser_ModeLateMenuEntry(&late));rules=3;
  assert(ActRaiser_ModeLateMenuEntry(&late));
  assert(ActRaiser_ModeLateMenu(&late)==RECOMP_RETURN_TAILCALL && target==0x02a72f);
  assert(ActRaiser_ModeInitialChoice(&late)==RECOMP_RETURN_TAILCALL && !ram[0x336]);
  ram[0x336]=0;assert(ActRaiser_ModeNextChoice(&late)==RECOMP_RETURN_TAILCALL && late.X==2);
  late=State(CPU_P_M);rules=0;
  assert(ActRaiser_ModeTitleGate(&late)==RECOMP_RETURN_TAILCALL && target==0x02a72f);
  rules=3;assert(ActRaiser_ModeNextChoiceEntry(&late));ram[0x336]=0;
  assert(ActRaiser_ModeNextChoice(&late)==RECOMP_RETURN_TAILCALL && late.X==2);
  rules=2;
  for(unsigned p=0;p<256;++p) {
    if(!(p&CPU_P_M) || p&CPU_P_X)continue;
    memset(ram,0x5a,sizeof(ram));memset(town,0x5a,sizeof(town));ram[0x349]=1;
    CpuState cpu=State(p);cpu.DB=0;cpu.S=0x1ff;
    assert(ActRaiser_ModeGameOverEntry(&cpu));
    unsigned prior=returns;
    assert(ActRaiser_ModeGameOver(&cpu)==RECOMP_RETURN_TAILCALL && target==0x02aafd && owner==0x02aaf9);
    assert(returns==prior+1 && waits==clears && cpu.S==0x1ff && cpu.PB==2);
    assert(cpu.X==14 && !(uint8_t)cpu.A && cpu._flag_C && cpu._flag_Z && !cpu._flag_N);
    assert(ram[0x4200]==1 && ram[0x2100]==0x80 && !ram[0x349] && !ram[0x347] && !ram[0xe4]);
    for(unsigned i=0;i<65536;++i) {
      const uint8_t expected=i>=0xb000 && i<0xb700?(i&1?0x20:0):i>=0x6b18 && i<0x6b26?0:0x5a;
      assert(town[i]==expected);
    }
    cpu.S=0x1fe;
    assert(ActRaiser_ModeReturnTargetEntry(&cpu));
    assert(ActRaiser_ModeReturnTarget(&cpu)==RECOMP_RETURN_TAILCALL && target==0x02ab03 && cpu.X==0x8023);
    ram[0x1ff]=0; /* original AAFD PHA */
    assert(ActRaiser_ModeReturnRootEntry(&cpu));
    assert(ActRaiser_ModeReturnRoot(&cpu)==RECOMP_RETURN_OWNED_UNWIND && cpu.PB==0 && cpu.S==0x1ff);
    assert(!ActRaiser_ModeReturnTargetEntry(&cpu));
  }
  for(unsigned r=0;r<4;++r)for(unsigned progress=0;progress<16;++progress) {
    rules=r;ram[0x349]=progress;CpuState cpu=State(CPU_P_M);cpu.DB=0;cpu.S=0x1ff;
    assert(ActRaiser_ModeGameOverEntry(&cpu)==((r&2) && progress>0 && progress<14));
  }
  const RecompReturn escapes[]={RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_TAILCALL,RECOMP_RETURN_OWNED_UNWIND,RECOMP_RETURN_SKIP_1};
  for(unsigned i=0;i<4;++i) {
    rules=2;ram[0x349]=1;CpuState cpu=State(CPU_P_M);cpu.DB=0;cpu.S=0x1ff;
    uint8_t before[65536];memcpy(before,ram,sizeof(ram));unsigned prior=returns;
    wait_result=escapes[i];assert(ActRaiser_ModeGameOver(&cpu)==escapes[i]);
    assert(returns==prior && !memcmp(before,ram,sizeof(ram)));
  }
  for(unsigned bad=0;bad<6;++bad) {
    CpuState cpu=State(CPU_P_M);
    switch(bad) {case 0:cpu.PB=0;break;case 1:cpu.DB=0;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=0;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;}
    assert(!ActRaiser_ModeTitleGateEntry(&cpu));
  }
  assert(!ActRaiser_ModeTitleGateEntry(NULL) && !ActRaiser_ModeGameOverEntry(NULL));
}
static void Policies(void) {
  for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b) {
    const ArRegionalModePolicy policy={{a,b}};uint8_t result=99;ArRegionalSource source;
    assert(ArRegionalMode_Resolve(&policy,&result) && result==((a==2)|((b==2)<<1)));
    assert(ArRegionalMode_GroupSource(&policy,&source)==(result==0 || result==3));
  }
  uint8_t result=99;ArRegionalModePolicy invalid={{3,0}};
  assert(!ArRegionalMode_Resolve(&invalid,&result) && result==99);
  assert(!ArRegionalMode_Init(&invalid,3) && !ArRegionalMode_Descriptor(2));
}
static void Roms(char **paths) {
  uint8_t rom[1048576];
  const unsigned gates[]={0xa7e9,0xa55f},overs[]={0xaae9,0xa82f,0xab5f,0xab68,0xab51};
  const unsigned clears_at[]={0xabc4,0xa90a,0xac5d,0xac66,0xac4f};
  for(unsigned r=0;r<5;++r) {
    FILE *file=fopen(paths[r],"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
    const uint8_t *over=rom+0x10000+overs[r]-0x8000;
    assert(!memcmp(rom+0x10000+clears_at[r]-0x8000,
        (uint8_t[]){8,0xc2,0x30,0xa9,0,0x20,0xa2,0,0,0x9f,0,0xb0,0x7f,0xe8,0xe8,0xe0,0,7,0xd0,0xf5,0x28,0x60},22));
    if(r<2) {
      const uint8_t *gate=rom+0x10000+gates[r]-0x8000;
      assert(!memcmp(gate,(uint8_t[]){0xaf,0xf0,0x1f,0x70,0xc9,0x41,0xd0,0x26,
          0xaf,0xf1,0x1f,0x70,0xc9,0x43,0xd0,0x1e,0xaf,0xf2,0x1f,0x70,0xc9,0x54,0xd0,0x16},24));
      assert(over[16]==0x22 && over[19]==2); /* new-run JSL before pushed main loop */
    } else {
      assert(!memcmp(over+16,(uint8_t[]){0x20,r==4?0xbe:0xd4,0xa8,0xa9,1,0x8d,0,0x42,0xa9,0x80,0x8d,0,0x21},13));
      assert(over[29]==0x20 && over[30]==(uint8_t)clears_at[r] && over[31]==(clears_at[r]>>8));
      assert(!memcmp(over+32,(uint8_t[]){0x9c,0x4b,3,0x9c,0x49,3,0x9c,0xe5,0,
          0xa2,0,0,0x8a,0x9f,0x18,0x6b,0x7f,0xe8,0xe0,0x0e,0,0xd0,0xf6,
          0xa9,0,0x48,0xa2,0x23,0x80,0xda,0x6b},31));
    }
  }
  puts("five-ROM unlock and Game Over reset/return signatures passed");
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);Policies();Prefixes();if(argc==6)Roms(argv+1);
  puts("mode entry: title eligibility, bounded reset and native return contracts passed");return 0;
}
