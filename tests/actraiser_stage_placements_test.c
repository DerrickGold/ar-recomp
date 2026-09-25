#include "actraiser/actraiser_stage_placements.h"
#include "actraiser/actraiser_native_call.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "regional/action/regional_terrain.h"
#include "randomizer.h"
#include "byte_order.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

typedef struct PlacementRoot {uint16_t scene,first,wave,end;} PlacementRoot;
#include "actraiser/actraiser_stage_placement_roots.inc"
typedef struct EntryCase {uint16_t transition,location,first_slot;} EntryCase;
static const EntryCase kEntries[]={
  {0,1,0xae0}, {0x80,1,0xb20}, {0x80,7,0xae0}, {0,7,0xae0},
  {0x100,1,0xae0}, {1,0x107,0xb20},
};
static uint8_t ram[65536],rom[32768],expected[65536];
static unsigned target,origin,calls,expected_caller;
static bool fail_randomizer,alter_randomizer;
static const ActionPlacementProgram *replacement_program;
static RecompReturn leaf_result;
bool Randomizer_ApplyPlacementPrograms(const RandomizerPlacementMap *maps,size_t count,RandomizerSummary *out) {
  assert(maps && count==49 && !out);
  for(size_t i=0;i<count;++i)assert(ActionPlacements_Validate(maps[i].program));
  if(alter_randomizer)++maps[0].program->rows[0].x;
  if(replacement_program) {
    assert(ActionPlacements_Validate(replacement_program));
    *maps[0].program=*replacement_program;
  }
  return !fail_randomizer;
}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) {
  (void)cpu;assert(bank==0 || bank==10);
  if(bank==10){assert(at>=0x8000);return rom[at&0x7fff];}
  return ram[at];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) {(void)cpu;assert(!bank);ram[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) {
  cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);
}
int cpu_hle_tailcall_request(uint32_t at,uint32_t owner) {target=at;origin=owner;return 1;}
static void InitRecord(uint8_t *memory,unsigned x,unsigned type,unsigned param) {
  ByteOrder_WriteLe16(memory+x,param==255?0x0800:0);
  ByteOrder_WriteLe16(memory+x+0x12,(uint16_t)(0x2000|type));
  ByteOrder_WriteLe16(memory+x+0x30,0);
}
RecompReturn bank_00_9557_M0X0(CpuState *cpu) {
  ++calls;
  if(leaf_result!=RECOMP_RETURN_NORMAL)return leaf_result;
  assert(cpu->DB==10 && !cpu->PB && !cpu->m_flag && !cpu->x_flag && !cpu->D);
  InitRecord(ram,cpu->X,cpu->A,ByteOrder_ReadLe16(ram+cpu->X+0x38));
  cpu->A=0x1234;cpu->P|=CPU_P_C|CPU_P_V;cpu_p_to_mirrors(cpu);
  return RECOMP_RETURN_NORMAL;
}
RecompReturn ActRaiserNativeCall(CpuState *cpu,ActRaiserNativeLeaf leaf,uint8_t bank,uint16_t caller,bool long_call) {
  assert(!bank && !long_call && caller==expected_caller && leaf==bank_00_9557_M0X0);
  return leaf(cpu);
}
static void ObjectModel(unsigned x,const ActionPlacement *row) {
  ByteOrder_WriteLe16(expected+x+0x34,row->x*16);ByteOrder_WriteLe16(expected+x+0x36,row->y*16);
  ByteOrder_WriteLe16(expected+x+0x38,row->parameter);InitRecord(expected,x,row->type,row->parameter);
}
static CpuState Setup(const PlacementRoot *root) {
  memset(ram,0x5a,sizeof(ram));memset(ram+0x6a0,0,0x1400);
  ByteOrder_WriteLe16(ram+0x18,root->scene);ByteOrder_WriteLe16(ram+0x8d0,1);
  ByteOrder_WriteLe16(ram+0xfc,0);ByteOrder_WriteLe16(ram+0x341,1);
  memset(rom,0,sizeof(rom));rom[root->end&0x7fff]=255;
  if(root->wave)rom[(root->wave-5)&0x7fff]=254;
  CpuState cpu={.DB=10,.X=0xae0,.Y=root->first,.A=0xbeef,.S=0x1f00,.P=CPU_P_I|CPU_P_V};
  cpu_p_to_mirrors(&cpu);calls=target=origin=0;expected_caller=0x9461;leaf_result=RECOMP_RETURN_NORMAL;
  return cpu;
}
static void Run(const PlacementRoot *root,const ArRegionalPlacementPolicy *policy,bool mode,unsigned d,unsigned terrain,
                const EntryCase *entry) {
  assert(ActRaiserStagePlacements_Prepare(root->scene,policy,mode,d,terrain));
  CpuState cpu=Setup(root);
  /* $932E initializes either one player, or a statue plus the descending
   * light. $930B then reserves eight slots before entering $941C. Cover the
   * actual $FC-low/$0341-word predicate, including the Death Heim exception. */
  ByteOrder_WriteLe16(ram+0xfc,entry->transition);
  ByteOrder_WriteLe16(ram+0x341,entry->location);
  const unsigned first=entry->first_slot;cpu.X=first;
  if(!policy->enemies && !policy->pickups){assert(!ActRaiser_StagePlacementsEntry(&cpu));return;}
  ActionPlacementProgram program;assert(ArRegionalPlacements_Copy(policy,root->scene,mode,d,&program));
  const CpuState before=cpu;memcpy(expected,ram,sizeof(ram));
  assert(ActRaiser_StagePlacementsEntry(&cpu));
  assert(!memcmp(expected,ram,sizeof(ram)) && !memcmp(&before,&cpu,sizeof(cpu)));
  CpuState wrong=cpu;wrong.X=first==0xae0?0xb20:0xae0;
  assert(!ActRaiser_StagePlacementsEntry(&wrong));
  unsigned x=first,objects=0,gate=0;size_t after=0;
  for(size_t i=0;i<program.count;++i) {
    const ActionPlacement *row=&program.rows[i];
    if(row->kind==kActionPlacement_End)break;
    if(row->kind==kActionPlacement_Wave) {
      gate=x;after=i+1;
      ByteOrder_WriteLe16(expected+x,0x800);ByteOrder_WriteLe16(expected+x+0x12,0xa813);
      ByteOrder_WriteLe16(expected+x+0x24,0);ByteOrder_WriteLe16(expected+x+0x26,0);ByteOrder_WriteLe16(expected+x+0x30,0);
      ByteOrder_WriteLe16(expected+x+0x34,row->x*16);ByteOrder_WriteLe16(expected+x+0x36,row->y*16);
      ByteOrder_WriteLe16(expected+x+2,row->retry_x*16);
      ByteOrder_WriteLe16(expected+x+4,16*(root->scene==0x101?ArRegionalTerrain_FillmoreCheckpointY(terrain):row->retry_y));
      ByteOrder_WriteLe16(expected+x+0x38,root->wave);x+=64;break;
    }
    if(row->kind==kActionPlacement_Reserve)for(unsigned j=0;j<row->reserve;++j) {
      ByteOrder_WriteLe16(expected+x,0x4000);x+=64;
    } else {ObjectModel(x,row);++objects;x+=64;}
  }
  assert(ActRaiser_StagePlacements(&cpu)==RECOMP_RETURN_TAILCALL && target==0x946e && origin==0x941c);
  assert(calls==objects && cpu.X==x && cpu.Y==(gate?root->wave:root->end));
  assert(cpu.S==before.S && cpu.DB==before.DB && cpu.PB==before.PB && !cpu.m_flag && !cpu.x_flag);
  assert((cpu.P&(CPU_P_C|CPU_P_V|CPU_P_I))==(gate?CPU_P_I:CPU_P_I|CPU_P_C));
  assert(!memcmp(expected,ram,sizeof(ram)));
  if(!gate)return;
  /* Original epilogue places sentinel; native gate retirement keeps its own
   * slot and the player, while the wave loader reuses all other eligible slots. */
  ByteOrder_WriteLe16(ram+x,0x8000);ByteOrder_WriteLe16(ram+gate+0x12,0xa82d);
  cpu.X=0x8a0;cpu.Y=root->wave;cpu.S=0x1efc;ByteOrder_WriteLe16(ram+cpu.S+1,gate);
  memcpy(expected,ram,sizeof(ram));assert(ActRaiser_StageWaveEntry(&cpu));
  expected_caller=0x9540;calls=0;objects=0;x=0x8a0;
  for(size_t i=after;i<program.count;++i) {
    while((ByteOrder_ReadLe16(expected+x)&0x800) || (ByteOrder_ReadLe16(expected+x+0x30)&1))x+=64;
    assert(x<0x1aa0);
    if(program.rows[i].kind==kActionPlacement_End)break;
    ObjectModel(x,&program.rows[i]);++objects;x+=64;
  }
  assert(ActRaiser_StageWave(&cpu)==RECOMP_RETURN_TAILCALL && target==0x954d && origin==0x9500);
  assert(cpu.X==x && cpu.Y==root->end && cpu.S==0x1efc && calls==objects);
  assert(!memcmp(expected,ram,sizeof(ram)));
}
static jmp_buf fatal_escape;
static char fatal_message[256];
static void CaptureFatal(const char *message) {
  snprintf(fatal_message,sizeof(fatal_message),"%s",message);
  longjmp(fatal_escape,1);
}
static void ExpectCapacityFailure(CpuState *cpu,bool wave) {
  const CpuState before=*cpu;
  memcpy(expected,ram,sizeof(ram));
  calls=target=origin=0;
  ActRaiserHleFatal_RegisterHostEscape(CaptureFatal);
  if(setjmp(fatal_escape)==0) {
    if(wave)ActRaiser_StageWave(cpu);
    else ActRaiser_StagePlacements(cpu);
    assert(false);
  }
  ActRaiserHleFatal_RegisterHostEscape(NULL);
  assert(!calls && !target && !origin);
  assert(!memcmp(expected,ram,sizeof(ram)) && !memcmp(&before,cpu,sizeof(before)));
  assert(strstr(fatal_message,"insufficient free actor slots"));
}
static void Capacity(void) {
  const ArRegionalPlacementPolicy jp={1,1};
  /* The generic format can fit an ordinary entry while exceeding the statue
   * entry's pool by one. Count reserved slots and the wave gate, not just rows. */
  static const struct {unsigned objects,reserved;bool wave;} cases[]={
    {0,0,false},{61,0,false},{62,0,false},{0,61,false},{0,62,false},
    {1,60,false},{1,61,false},{60,0,true},{61,0,true},{0,60,true},{0,61,true},
  };
  for(unsigned n=0;n<sizeof(cases)/sizeof(cases[0]);++n) {
    ActionPlacementProgram program={0};
    if(cases[n].reserved)program.rows[program.count++]=(ActionPlacement){
      .kind=kActionPlacement_Reserve,.reserve=cases[n].reserved};
    for(unsigned i=0;i<cases[n].objects;++i)
      program.rows[program.count++]=(ActionPlacement){.kind=kActionPlacement_Object,.x=5,.y=27,.type=1};
    if(cases[n].wave)program.rows[program.count++]=(ActionPlacement){.kind=kActionPlacement_Wave};
    program.rows[program.count++]=(ActionPlacement){.kind=kActionPlacement_End};
    for(size_t i=0;i<program.count;++i)program.rows[i].id=(uint16_t)(i+1);
    replacement_program=&program;
    assert(ActRaiserStagePlacements_Prepare(kRoots[0].scene,&jp,false,0,1));
    replacement_program=NULL;
    for(unsigned e=0;e<sizeof(kEntries)/sizeof(kEntries[0]);++e) {
      CpuState cpu=Setup(&kRoots[0]);
      ByteOrder_WriteLe16(ram+0xfc,kEntries[e].transition);
      ByteOrder_WriteLe16(ram+0x341,kEntries[e].location);
      cpu.X=kEntries[e].first_slot;
      assert(ActRaiser_StagePlacementsEntry(&cpu));
      const unsigned used=cases[n].objects+cases[n].reserved+cases[n].wave;
      if(used+1>(0x1aa0u-cpu.X)/64) {
        ExpectCapacityFailure(&cpu,false);
      } else {
        assert(ActRaiser_StagePlacements(&cpu)==RECOMP_RETURN_TAILCALL);
        assert(cpu.X==kEntries[e].first_slot+used*64 && cpu.X<0x1aa0);
        assert(calls==cases[n].objects && target==0x946e && origin==0x941c);
        /* The native epilogue still has a valid slot for its sentinel. */
        cpu_write16(&cpu,0,cpu.X,0x8000);
        for(unsigned at=0x1aa0;at<0x1ae2;++at)assert(ram[at]==0x5a);
      }
    }
  }
  /* Later waves already preflight retained actors. Lock in failure-before-
   * mutation and the requirement for a sentinel even after the final object. */
  ActionPlacementProgram program={.count=3,.rows={
    {.id=1,.kind=kActionPlacement_Wave},
    {.id=2,.kind=kActionPlacement_Object,.x=5,.y=27,.type=1},
    {.id=3,.kind=kActionPlacement_End},
  }};
  replacement_program=&program;
  assert(ActRaiserStagePlacements_Prepare(kRoots[0].scene,&jp,false,0,1));
  replacement_program=NULL;
  for(unsigned free_slots=0;free_slots<=2;++free_slots) {
    CpuState cpu=Setup(&kRoots[0]);
    for(unsigned x=0x8a0;x<0x1aa0;x+=64)ByteOrder_WriteLe16(ram+x,0x800);
    ByteOrder_WriteLe16(ram+0xae0+0x12,0xa82d);
    ByteOrder_WriteLe16(ram+0xae0+0x38,kRoots[0].wave);
    for(unsigned i=0;i<free_slots;++i)ByteOrder_WriteLe16(ram+0x19e0+i*64,0);
    cpu.X=0x8a0;cpu.Y=kRoots[0].wave;cpu.S=0x1efc;
    ByteOrder_WriteLe16(ram+cpu.S+1,0xae0);
    assert(ActRaiser_StageWaveEntry(&cpu));
    if(free_slots<2)ExpectCapacityFailure(&cpu,true);
    else {
      expected_caller=0x9540;
      assert(ActRaiser_StageWave(&cpu)==RECOMP_RETURN_TAILCALL);
      assert(calls==1 && cpu.X==0x1a20 && target==0x954d && origin==0x9500);
    }
  }
}
int main(void) {
  uint8_t seed[32]={0},hash[32],copy[32];bool native=false;
  assert(ActRaiserStagePlacements_Fingerprint(seed,hash,&native) && native && !memcmp(hash,seed,32));
  assert(!ActRaiserStagePlacements_Fingerprint(NULL,hash,&native));
  assert(!ActRaiserStagePlacements_Fingerprint(seed,NULL,&native));
  assert(!ActRaiserStagePlacements_Fingerprint(seed,hash,NULL));
  for(unsigned e=0;e<3;++e)for(unsigned p=0;p<3;++p)for(unsigned mode=0;mode<2;++mode)
    for(unsigned d=0;d<3;++d)for(unsigned r=0;r<49;++r)
      for(unsigned entry=0;entry<sizeof(kEntries)/sizeof(kEntries[0]);++entry) {
        const ArRegionalPlacementPolicy policy={e,p};
        Run(&kRoots[r],&policy,mode,d,(e+p+d)%3,&kEntries[entry]);
      }
  const ArRegionalPlacementPolicy jp={1,1};
  assert(ActRaiserStagePlacements_Prepare(0x101,&jp,false,0,1));
  assert(ActRaiserStagePlacements_Fingerprint(seed,hash,&native) && !native && memcmp(hash,seed,32));
  assert(ActRaiserStagePlacements_Prepare(0x101,&jp,false,0,0));
  assert(ActRaiserStagePlacements_Fingerprint(seed,copy,&native) && memcmp(hash,copy,32));
  assert(ActRaiserStagePlacements_Prepare(0x101,&jp,false,0,1));
  assert(ActRaiserStagePlacements_Fingerprint(seed,copy,&native) && !memcmp(hash,copy,32));
  alter_randomizer=true;
  /* Changing settings alone never changes the captured program. */
  assert(ActRaiserStagePlacements_Fingerprint(seed,copy,&native) && !memcmp(hash,copy,32));
  assert(ActRaiserStagePlacements_Prepare(0x101,&jp,false,0,1));
  assert(ActRaiserStagePlacements_Fingerprint(seed,copy,&native) && memcmp(hash,copy,32));
  alter_randomizer=false;
  assert(ActRaiserStagePlacements_Prepare(0x101,&jp,false,0,1));
  for(unsigned bad=0;bad<10;++bad) {
    CpuState cpu=Setup(&kRoots[0]);
    switch(bad) {
      case 0:cpu.emulation=1;break;case 1:cpu.PB=1;break;case 2:cpu.DB=0;break;
      case 3:cpu.D=1;break;case 4:cpu.m_flag=1;break;case 5:cpu.x_flag=1;break;
      case 6:cpu.X+=64;break;case 7:++cpu.Y;break;case 8:cpu.P|=CPU_P_D;break;
      case 9:rom[kRoots[0].end&0x7fff]=0;break;
    }
    assert(!ActRaiser_StagePlacementsEntry(&cpu));
  }
  CpuState cpu=Setup(&kRoots[0]);
  fail_randomizer=true;assert(!ActRaiserStagePlacements_Prepare(0x201,&jp,false,0,1));fail_randomizer=false;
  assert(ActRaiserStagePlacements_Fingerprint(seed,copy,&native) && !memcmp(hash,copy,32));
  assert(ActRaiser_StagePlacementsEntry(&cpu));
  assert(!ActRaiserStagePlacements_Prepare(0x909,&jp,false,0,1) && ActRaiser_StagePlacementsEntry(&cpu));
  leaf_result=RECOMP_RETURN_SKIP_1;
  assert(ActRaiser_StagePlacements(&cpu)==RECOMP_RETURN_SKIP_1 && calls==1 && !target);
  Capacity();
  ActRaiserStagePlacements_Reset();assert(!ActRaiser_StagePlacementsEntry(&cpu) && !ActRaiser_StageWaveEntry(&cpu));
  memcpy(copy,seed,32);assert(ActRaiserStagePlacements_Fingerprint(copy,copy,&native) && native && !memcmp(copy,seed,32));
  puts("Native placement adapters: ordinary/statue entries, atomic capacity checks, pool footprint, waves, captures, cursor/return contracts passed");
  return 0;
}
