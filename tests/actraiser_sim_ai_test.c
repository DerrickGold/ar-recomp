#include "actraiser/actraiser_sim_ai_runtime.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "regional/regional_sim_actors.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

CpuReturnScope *g_cpu_return_scope,*g_cpu_owned_unwind_scope;
int g_recomp_stack_top;
int cpu_finish_owned_unwind(CpuReturnScope *scope,CpuState *cpu) { (void)scope;(void)cpu;return 0; }
static uint8_t ram[65536],town_ram[65536],jp[65536];
static ArRegionalSimActors actors;
static unsigned draws,lookups,animations,randoms[2],escape_call;
static uint16_t callers[2];
static uint32_t target,origin;
static RecompReturn escape;
static bool ready=true;
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu;assert(bank==0 || bank==1 || bank==0x7f);return bank==0x7f?town_ram[address]:ram[address];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) { return cpu_read8(cpu,bank,address)|cpu_read8(cpu,bank,address+1)<<8; }
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu;assert(bank==0 || bank==1 || bank==0x7f);(bank==0x7f?town_ram:ram)[address]=value;
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(cpu,bank,address,value);cpu_write8(cpu,bank,address+1,value>>8);
}
int cpu_hle_tailcall_request(uint32 pc,uint32 source) { target=pc;origin=source;return 1; }
bool ActRaiserRegional_SimActorAiSnapshot(unsigned town,unsigned slot,uint16_t *snapshot) {
  ArRegionalSimActorRules rules;
  if (!ready || !ArRegionalSimActors_Read(&actors,town,slot,&rules)) return false;
  *snapshot=rules.ai;return true;
}
static void Word(uint8_t *bytes,unsigned at,unsigned value) { bytes[at]=value;bytes[at+1]=value>>8; }
static void A8(CpuState *cpu,uint8_t value) { cpu_write_a8(cpu,value);ActRaiserCpuHle_SetNegativeZero8(cpu,value); }
static void A16(CpuState *cpu,uint16_t value) { cpu->A=value;ActRaiserCpuHle_SetNegativeZero16(cpu,value); }
static void Carry(CpuState *cpu,bool value) { cpu->_flag_C=value;cpu->P=(cpu->P&~CPU_P_C)|(value?CPU_P_C:0); }
static RecompReturn Escape(CpuState *cpu) { cpu->S=0x1234;cpu->PB=0x42;return escape; }
RecompReturn bank_03_AF65_M1X0(CpuState *cpu) {
  assert(draws<2 && cpu->PB==3 && cpu->DB==1 && cpu->m_flag && !cpu->x_flag && (uint8_t)cpu->A==32);
  assert(cpu_read16(cpu,0,cpu->S+1)==callers[draws] && ram[cpu->S+3]==1);
  ++draws;if(escape && draws==escape_call)return Escape(cpu);
  A8(cpu,randoms[draws-1]);cpu->S+=3;return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_01_D072_M0X0(CpuState *cpu) {
  ++animations;assert(cpu->PB==1 && !cpu->m_flag && cpu_read16(cpu,0,cpu->S+1)==0xba69);
  if(escape)return Escape(cpu);
  A16(cpu,0x3456);cpu->S+=2;return RECOMP_RETURN_NORMAL;
}
RecompReturn bank_03_BDE1_M1X0(CpuState *cpu) {
  ++lookups;assert(cpu->PB==3 && cpu->DB==1 && cpu->m_flag && cpu_read16(cpu,0,cpu->S+1)==0xbd49 && ram[cpu->S+3]==1);
  if(escape)return Escape(cpu);
  cpu->X=0x8200;cpu->Y=81;Carry(cpu,false);A8(cpu,1);cpu->S+=3;return RECOMP_RETURN_NORMAL;
}
static CpuState Setup(unsigned town,unsigned slot,unsigned species,unsigned flags) {
  memset(ram,0xa5,sizeof(ram));memset(town_ram,0x5a,sizeof(town_ram));
  draws=lookups=animations=0;escape=0;target=origin=0;ready=true;actors=(ArRegionalSimActors){0};
  const unsigned actor=0x0b30+slot*0x26;
  Word(town_ram,0x7bfb,town*2);Word(town_ram,0x9688+town*8+slot*2,actor);
  Word(town_ram,0x95f8+town*8+slot*2,species);Word(ram,actor+0x0e,0x8000|species);
  assert(ArRegionalSimActors_LoadTown(&actors,town));
  assert(ArRegionalSimActors_Birth(&actors,town,slot,(ArRegionalSimActorRules){.ai=63}));
  CpuState cpu={.PB=1,.DB=1,.A=0xabcd,.X=actor,.Y=0x1234,.S=0x1ff0,.P=flags};cpu_p_to_mirrors(&cpu);return cpu;
}
static void Policies(void) {
  for(unsigned n=0;n<729;++n) {
    ArRegionalSimAiPolicy policy;unsigned digits=n,mask=0;
    for(unsigned i=0;i<6;++i) { policy.source[i]=digits%3;mask|=(digits%3==1)<<i;digits/=3; }
    uint16_t snapshot=0xffff;assert(ArRegionalSimAi_Resolve(&policy,&snapshot) && snapshot==mask);
    ArRegionalSource source;assert(ArRegionalSimAi_GroupSource(&policy,&source)==(!mask || mask==63));
    for(unsigned i=0;i<6;++i) {
      uint16_t value;assert(ArRegionalSimAi_Value(snapshot,i,&value));
      assert(value==ArRegionalSimAi_Descriptor(i)->value[policy.source[i]]);
    }
  }
  ArRegionalSimAiPolicy invalid={{0,0,0,0,0,3}},before=invalid;uint16_t sentinel=0xdead;
  assert(!ArRegionalSimAi_Init(&invalid,3) && !memcmp(&invalid,&before,sizeof(invalid)));
  assert(!ArRegionalSimAi_Resolve(&invalid,&sentinel) && sentinel==0xdead);
  assert(!ArRegionalSimAi_Value(64,0,&sentinel) && sentinel==0xdead);
  assert(!ArRegionalSimAi_Descriptor(kArRegionalSimAi_Count));
}
/* Bounded ROM oracle for the JP candidate prefix only. Its RNG remains a
 * shared call contract, not a second random implementation. Production keeps
 * the US native generator and structure lookup. */
static void NativeCandidate(CpuState *cpu) {
  uint16_t pc=0xbc6b;
  for(unsigned step=0;step<48;++step) {
    if(pc==0xbccf)return;
    uint8_t op=jp[pc++];unsigned value=0;
    if(op==0x18) { Carry(cpu,false);continue; }
    if(op==0x3a) { A8(cpu,(uint8_t)(cpu->A-1));continue; }
    value=jp[pc++];
    if(op==0x80 || op==0xd0 || op==0xf0) {
      if(op==0x80 || (op==0xd0 && !cpu->_flag_Z) || (op==0xf0 && cpu->_flag_Z))pc+=(int8_t)value;
    } else if(op==0xe2) { assert(value==32);cpu->m_flag=1;cpu->P|=CPU_P_M; }
    else if(op==0x29) A8(cpu,(uint8_t)cpu->A&value);
    else if(op==0x69) {
      const unsigned a=(uint8_t)cpu->A,sum=a+value+cpu->_flag_C;
      cpu->_flag_V=(~(a^value)&(a^sum)&0x80)!=0;
      cpu->P=(cpu->P&~CPU_P_V)|(cpu->_flag_V?CPU_P_V:0);Carry(cpu,sum>255);A8(cpu,sum);
    } else if(op==0xa9) { assert(cpu->m_flag);A8(cpu,value); }
    else if(op==0xc9) { assert(!cpu->m_flag);value|=jp[pc++]<<8;Carry(cpu,cpu->A>=value);ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A-value); }
    else {
      value|=jp[pc++]<<8;
      if(op==0xad) { assert(value==0x0aee);A8(cpu,ram[value]); }
      else if(op==0xaf) { assert(jp[pc++]==0x7f && !cpu->m_flag);A16(cpu,cpu_read16(cpu,0x7f,value)); }
      else if(op==0x8f) { assert(jp[pc++]==0x7f);town_ram[value]=(uint8_t)cpu->A; }
      else { assert(op==0x22 && value==0xad2d && jp[pc++]==3 && draws<2 && (uint8_t)cpu->A==32);cpu->host_return_valid=1;A8(cpu,randoms[draws++]); }
    }
  }
  assert(!"candidate exceeded its bounded prefix");
}
static void Candidates(bool oracle) {
  for(unsigned cls=0;cls<3;++cls)for(unsigned axis=0;axis<2;++axis)
    for(unsigned r0=0;r0<32;++r0)for(unsigned r1=0;r1<32;++r1)for(unsigned f=0;f<4;++f) {
      CpuState cpu=Setup(2,1,0x12,(f&1?CPU_P_C:0)|(f&2?CPU_P_V:0));
      const unsigned kind=cls==0?0:cls==1?2:255;
      Word(town_ram,0x7c05,kind);ram[0x0aee]=axis*2;randoms[0]=r0;randoms[1]=r1;
      callers[0]=kind==2?0xbccc:axis?0xbcef:0xbd08;callers[1]=kind==2?0xbcd8:axis?0xbcfc:0xbd12;
      CpuState native=cpu;uint32_t tail=0;
      assert(ActRaiserSimAi_Run(&cpu,kActRaiserSimAi_Candidate,&tail)==RECOMP_RETURN_NORMAL && draws==2 && tail==0x01bd45);
      const unsigned x=kind==2?(r0&28):axis?((r0&28)+3):r0;
      const unsigned y=kind==2?(r1&28):axis?r1:((r1&28)+3);
      assert(town_ram[0x7c11]==x && town_ram[0x7c13]==y && town_ram[0x7c12]==0x5a && town_ram[0x7c14]==0x5a);
      assert(cpu.S==native.S && cpu.PB==native.PB && cpu.X==native.X && cpu.Y==native.Y && cpu.A==y);
      if(oracle) { draws=0;NativeCandidate(&native);assert(draws==2 && !memcmp(&cpu,&native,sizeof(cpu)));assert(town_ram[0x7c11]==x && town_ram[0x7c13]==y); }
    }
}
static void Prefixes(void) {
  for(unsigned town=0;town<6;++town)for(unsigned slot=0;slot<4;++slot) {
    CpuState cpu=Setup(town,slot,0x12,CPU_P_C|CPU_P_V);uint32_t tail=0;
    Word(ram,cpu.X+0x14,0xffff);
    assert(ActRaiserSimAi_Run(&cpu,kActRaiserSimAi_DragonReset,&tail)==0 && tail==0x01ba6a && animations==1);
    assert(!cpu_read16(&cpu,1,cpu.X+0x14) && cpu.A==0x3456);
    for(unsigned count=1;count<=16;++count) {
      assert(ActRaiserSimAi_Run(&cpu,kActRaiserSimAi_DragonGate,&tail)==0 && tail==(count%8?0x01ba79u:0x01ba6du));
      assert(cpu_read16(&cpu,1,cpu.X+0x14)==count%8 && cpu.A==count%8);
    }
    /* Recursion is owned by the stacked parent, not B778's effect actor. */
    Word(ram,cpu.S+1,cpu.X);cpu.X=0x1700;CpuState before=cpu;
    assert(ActRaiser_RegionalSimDragonRecursionEntry(&cpu));
    assert(ActRaiser_RegionalSimDragonRecursion(&cpu)==RECOMP_RETURN_TAILCALL && target==0x01bb60 && origin==0x01bb5c);
    assert(!memcmp(&before,&cpu,sizeof(cpu)) && !draws);
    actors.active[slot].ai=0;assert(!ActRaiser_RegionalSimDragonRecursionEntry(&cpu));
    for(unsigned species=0x12;species<=0x15;++species) {
      cpu=Setup(town,slot,species,CPU_P_M);before=cpu;
      assert(ActRaiser_RegionalSimPool(&cpu)==RECOMP_RETURN_TAILCALL && target==0x01bd4a && origin==0x01bd46);
      assert(lookups==1 && !draws && cpu.S==before.S && cpu.X==0x8200 && cpu.Y==81 && !cpu._flag_C);
    }
    cpu=Setup(town,slot,0x13,CPU_P_V);before=cpu;
    assert(ActRaiser_RegionalSimBatWait(&cpu)==RECOMP_RETURN_TAILCALL && target==0x01bf75 && cpu.A==60);
    before.A=60;ActRaiserCpuHle_SetNegativeZero16(&before,60);assert(!memcmp(&cpu,&before,sizeof(cpu)));
  }
  for(unsigned a=0;a<256;++a)for(unsigned flags=0;flags<4;++flags) {
    CpuState cpu=Setup(0,0,0x13,CPU_P_M|(flags&1?CPU_P_C:0)|(flags&2?CPU_P_V:0));cpu.A=0xbe00|a;
    CpuState expected=cpu;Carry(&expected,a>=250);ActRaiserCpuHle_SetNegativeZero8(&expected,(uint8_t)(a-250));
    assert(ActRaiser_RegionalSimBatChance(&cpu)==RECOMP_RETURN_TAILCALL && target==0x01bee5 && !memcmp(&cpu,&expected,sizeof(cpu)));
  }
}
static void Escapes(void) {
  for(unsigned seam=0;seam<kActRaiserSimAi_Count;++seam) {
    if(seam!=kActRaiserSimAi_DragonReset && seam!=kActRaiserSimAi_Candidate && seam!=kActRaiserSimAi_Pool)continue;
    for(unsigned token=1;token<=RECOMP_RETURN_OWNED_UNWIND;++token)for(unsigned call=1;call<=(seam==kActRaiserSimAi_Candidate?2u:1u);++call) {
      CpuState cpu=Setup(0,0,0x12,seam==kActRaiserSimAi_Pool?CPU_P_M:0);
      Word(town_ram,0x7c05,2);callers[0]=0xbccc;callers[1]=0xbcd8;randoms[0]=12;randoms[1]=20;
      escape=token;escape_call=call;uint32_t tail=0xdeadbeef;
      assert(ActRaiserSimAi_Run(&cpu,seam,&tail)==token && tail==0xdeadbeef && cpu.S==0x1234);
      assert(cpu.PB==((seam==kActRaiserSimAi_DragonReset || token>=RECOMP_RETURN_PARKED_WAIT)?0x42:1));
      if(seam==kActRaiserSimAi_Candidate)assert(draws==call && town_ram[0x7c13]==0x5a && town_ram[0x7c11]==(call==1?0x5a:12));
      if(seam==kActRaiserSimAi_DragonReset)assert(cpu_read16(&cpu,1,cpu.X+0x14)==0xa5a5);
    }
  }
  for(unsigned token=1;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
    CpuState cpu=Setup(0,0,0x12,CPU_P_M);escape=token;
    assert(ActRaiser_RegionalSimPool(&cpu)==(token>=RECOMP_RETURN_TAILCALL?token:token-1) && !target && !origin);
  }
}
static void Codec(void) {
  uint8_t bytes[kArRegionalSimActorsEncodedBytes],old[kArRegionalSimActorsV1EncodedBytes];
  for(unsigned combat=0;combat<32;++combat)for(unsigned ai=0;ai<64;++ai) {
    ArRegionalSimActors original={0},decoded={0};
    assert(ArRegionalSimActors_LoadTown(&original,3));
    assert(ArRegionalSimActors_Birth(&original,3,2,(ArRegionalSimActorRules){combat,ai}));
    assert(ArRegionalSimActors_SaveTown(&original,3));
    assert(ArRegionalSimActors_Birth(&original,3,2,(ArRegionalSimActorRules){31-combat,63-ai}));
    assert(ArRegionalSimActors_Encode(&original,bytes,sizeof(bytes)));
    assert(ArRegionalSimActors_Decode(bytes,sizeof(bytes),&decoded) && !memcmp(&original,&decoded,sizeof(original)));
    memset(old,0xa5,sizeof(old));assert(!ArRegionalSimActors_EncodeVersion(&original,old,sizeof(old),1));
    for(unsigned i=0;i<sizeof(old);++i)assert(old[i]==0xa5); /* no silent AI loss or partial writes */
    bytes[14+4*14]|=0x40;decoded=original;
    assert(!ArRegionalSimActors_Decode(bytes,sizeof(bytes),&decoded) && !memcmp(&original,&decoded,sizeof(original)));
  }
}
static void Guards(void) {
  CpuState cpu=Setup(0,0,0x13,0);unsigned town,slot;assert(ActRaiser_RegionalSimBatWaitEntry(&cpu));
  const CpuState valid=cpu;
  cpu.PB=3;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));cpu=valid;
  cpu.DB=0;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));cpu=valid;
  cpu.D=1;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));cpu=valid;
  cpu.m_flag=1;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));cpu=valid;
  cpu.x_flag=1;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));cpu=valid;
  cpu._flag_D=1;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));cpu=valid;
  cpu.P|=CPU_P_D;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));cpu=valid;
  cpu.emulation=1;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));cpu=valid;
  cpu.X++;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));cpu=valid;
  assert(!ActRaiserSimAi_Entry(&cpu,kActRaiserSimAi_Count,&town,&slot));
  assert(!ActRaiser_RegionalSimDragonResetEntry(&cpu));
  ready=false;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));ready=true;
  actors.active_town_tag=2;assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));actors.active_town_tag=1;
  Word(town_ram,0x9688,0x0b31);assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));Word(town_ram,0x9688,0x0b30);
  Word(town_ram,0x95f8,0x12);assert(!ActRaiser_RegionalSimBatWaitEntry(&cpu));Word(town_ram,0x95f8,0x13);
  for(unsigned mask=0;mask<64;++mask) {
    actors.active[0].ai=mask;
    assert(ActRaiser_RegionalSimBatWaitEntry(&cpu)==((mask&32)!=0));
    assert(ActRaiser_RegionalSimCandidateEntry(&cpu)==((mask&4)!=0));
  }
}
int main(int argc,char **argv) {
  if(argc>1) { assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f && !fseek(f,0x8000,SEEK_SET));assert(fread(jp+0x8000,1,0x8000,f)==0x8000 && !fclose(f)); }
  Policies();Candidates(argc>1);Prefixes();Escapes();Codec();Guards();
  puts("SIM AI: mixed policies, 24576 candidate cases, native helpers/flags, actor ownership, search cadence and escaping returns passed");return 0;
}
