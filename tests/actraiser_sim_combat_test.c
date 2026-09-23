#include "actraiser/actraiser_sim_combat_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[65536],town_ram[65536];
static ArRegionalSimActors actors;
static uint16_t requested;
static bool ready=true;
static unsigned captures,copies,calls,writes;
static RecompReturn token;
static uint32_t target,origin;
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 address) {
  (void)cpu;assert(bank==0 || bank==1 || bank==0x7f);return bank==0x7f?town_ram[address]:ram[address];
}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 address) { return cpu_read8(cpu,bank,address)|cpu_read8(cpu,bank,address+1)<<8; }
void cpu_write8(CpuState *cpu,uint8 bank,uint16 address,uint8 value) {
  (void)cpu;++writes;assert(bank<=1 || bank==0x7f);(bank==0x7f?town_ram:ram)[address]=value;
}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 address,uint16 value) {
  cpu_write8(cpu,bank,address,value);cpu_write8(cpu,bank,address+1,value>>8);
}
int cpu_hle_tailcall_request(uint32 pc,uint32 source) { target=pc;origin=source;return 1; }
bool ActRaiserRegional_SimActorsReady(void) { return ready; }
void ActRaiserRegional_SimActorCache(bool load,unsigned town) {
  ++copies;assert(load?ArRegionalSimActors_LoadTown(&actors,town):ArRegionalSimActors_SaveTown(&actors,town));
}
void ActRaiserRegional_SimActorBirth(unsigned town,unsigned slot) {
  ++captures;assert(ArRegionalSimActors_Birth(&actors,town,slot,(ArRegionalSimActorRules){.combat=requested}));
}
bool ActRaiserRegional_SimActorSnapshot(unsigned town,unsigned slot,uint16_t *snapshot) {
  ArRegionalSimActorRules rules;
  if (!ready || !ArRegionalSimActors_Read(&actors,town,slot,&rules)) return false;
  *snapshot=rules.combat;return true;
}
static RecompReturn Native(CpuState *cpu) {
  ++calls;assert(!ActRaiser_RegionalSimCacheEntry(cpu));return token;
}
RecompReturn bank_03_813F_M0X0(CpuState *cpu) { assert(!cpu->m_flag);return Native(cpu); }
RecompReturn bank_03_813F_M1X0(CpuState *cpu) { assert(cpu->m_flag);return Native(cpu); }
RecompReturn bank_03_8168_M0X0(CpuState *cpu) { assert(!cpu->m_flag);return Native(cpu); }
RecompReturn bank_03_8168_M1X0(CpuState *cpu) { assert(cpu->m_flag);return Native(cpu); }
static void Word(uint8_t *bytes,unsigned at,unsigned value) { bytes[at]=value;bytes[at+1]=value>>8; }
static void Actor(unsigned town,unsigned slot,unsigned species) {
  Word(town_ram,0x7bfb,town*2);Word(town_ram,0x9688+town*8+slot*2,0x0b30+slot*0x26);
  Word(town_ram,0x95f8+town*8+slot*2,0x12+species);Word(ram,0x0b3e +slot*0x26,0x12+species);
  Word(ram,0x0b40+slot*0x26,0x8000);
}
static void PolicyAndCache(void) {
  for(unsigned combination=0;combination<243;++combination) {
    ArRegionalSimCombatPolicy policy;unsigned n=combination,mask=0;
    for(unsigned i=0;i<5;++i) { policy.source[i]=(ArRegionalSource)(n%3);mask|=(n%3==1)<<i;n/=3; }
    uint16_t snapshot=0xffff;assert(ArRegionalSimCombat_Resolve(&policy,&snapshot) && snapshot==mask);
    ArRegionalSource source;const bool uniform=ArRegionalSimCombat_GroupSource(&policy,&source);
    assert(uniform==(!mask || mask==31));
    if(uniform) assert((source==kArRegionalSource_Japan)==(mask==31));
    for(unsigned species=0;species<4;++species) {
      uint8_t limit,contact;assert(ArRegionalSimCombat_Values(snapshot,species,&limit,&contact));
      const unsigned expected_limits[]={2-(mask&1),0,3-((mask>>1)&1),7};
      const unsigned expected_damage[]={3-((mask>>2)&1),1,6-3*((mask>>3)&1),8-4*((mask>>4)&1)};
      assert(limit==expected_limits[species] && contact==expected_damage[species]);
    }
  }
  for(unsigned town=0;town<6;++town) {
    assert(ArRegionalSimActors_LoadTown(&actors,town));
    for(unsigned slot=0;slot<4;++slot) assert(ArRegionalSimActors_Birth(&actors,town,slot,(ArRegionalSimActorRules){.combat=town*4+slot+1}));
    assert(!ArRegionalSimActors_SaveTown(&actors,(town+1)%6));
    assert(ArRegionalSimActors_SaveTown(&actors,town));
  }
  for(unsigned town=0;town<6;++town) {
    assert(ArRegionalSimActors_LoadTown(&actors,town));
    for(unsigned slot=0;slot<4;++slot) {
      ArRegionalSimActorRules snapshot;assert(ArRegionalSimActors_Read(&actors,town,slot,&snapshot) && snapshot.combat==town*4+slot+1);
      assert(ArRegionalSimActors_Birth(&actors,town,slot,(ArRegionalSimActorRules){.combat=31}));
    }
    assert(ArRegionalSimActors_LoadTown(&actors,town)); /* discard an unsaved generation, exactly like the native cache */
    assert(actors.active[0].combat==town*4+1);
  }
  uint8_t bytes[kArRegionalSimActorsEncodedBytes];ArRegionalSimActors decoded={0};
  assert(ArRegionalSimActors_Encode(&actors,bytes,sizeof(bytes)) && ArRegionalSimActors_Decode(bytes,sizeof(bytes),&decoded));
  assert(!memcmp(&actors,&decoded,sizeof(actors)));
  for(unsigned at=0;at<sizeof(bytes);++at) {
    uint8_t bad[sizeof(bytes)];memcpy(bad,bytes,sizeof(bytes));bad[at]|=0x80;
    decoded=actors;assert(!ArRegionalSimActors_Decode(bad,sizeof(bad),&decoded));assert(!memcmp(&actors,&decoded,sizeof(actors)));
  }
  ArRegionalSimActorRules untouched_rules={.combat=0xdead};assert(!ArRegionalSimActors_Read(&actors,0,0,&untouched_rules) && untouched_rules.combat==0xdead);
  uint16_t untouched=0xdead;
  ArRegionalSimCombatPolicy invalid={{0,0,0,0,3}};
  assert(!ArRegionalSimCombat_Resolve(&invalid,&untouched) && untouched==0xdead);
}
static void Arithmetic(void) {
  Actor(0,0,0);
  for(unsigned mask=0;mask<32;++mask)for(unsigned species=0;species<4;++species) {
    Actor(0,0,species);uint8_t threshold,damage;assert(ArRegionalSimCombat_Values(mask,species,&threshold,&damage));
    for(unsigned a=0;a<256;++a)for(unsigned flags=0;flags<16;++flags) {
      unsigned p=CPU_P_M|((flags&1)?CPU_P_C:0)|((flags&2)?CPU_P_N:0)|((flags&4)?CPU_P_Z:0)|((flags&8)?CPU_P_V:0);
      CpuState cpu={.PB=1,.DB=1,.S=0x1eed,.A=0xad00|a,.X=species,.Y=0x0b30,.P=p};cpu_p_to_mirrors(&cpu);
      CpuState expected=cpu;const unsigned sub=damage+!(flags&1);const uint8_t result=(uint8_t)(a-sub);
      expected.A=0xad00|result;
      expected.P=(p&~(CPU_P_N|CPU_P_Z|CPU_P_V|CPU_P_C))|(result&0x80?CPU_P_N:0)|(!result?CPU_P_Z:0)|
          (a>=sub?CPU_P_C:0)|(((a^damage)&(a^result)&0x80)?CPU_P_V:0);cpu_p_to_mirrors(&expected);
      assert(ActRaiserSimCombat_Contact(&cpu,mask) && !memcmp(&cpu,&expected,sizeof(cpu)));
      expected.A=0xad00|threshold;expected.P=(expected.P&~(CPU_P_N|CPU_P_Z))|(!threshold?CPU_P_Z:0);cpu_p_to_mirrors(&expected);
      assert(ActRaiserSimCombat_Threshold(&cpu,mask) && !memcmp(&cpu,&expected,sizeof(cpu)));
    }
  }
  assert(!writes);
}
int main(void) {
  PolicyAndCache();Arithmetic();actors=(ArRegionalSimActors){0};
  const RecompReturn results[]={RECOMP_RETURN_NORMAL,RECOMP_RETURN_SKIP_1,RECOMP_RETURN_SKIP_2,
      RECOMP_RETURN_SKIP_3,RECOMP_RETURN_TAILCALL,RECOMP_RETURN_PARKED_WAIT,RECOMP_RETURN_OWNED_UNWIND};
  for(unsigned town=0;town<6;++town)for(unsigned width=0;width<2;++width)for(unsigned i=0;i<7;++i) {
    Actor(town,0,0);token=results[i];CpuState cpu={.PB=3,.DB=0x7f,.m_flag=width};
    assert(ActRaiser_RegionalSimCacheEntry(&cpu));copies=calls=0;
    ArRegionalSimActors before=actors;const CpuState registers=cpu;
    assert(ActRaiser_RegionalSimCacheLoad(&cpu)==token && calls==1 && copies==(i==0));
    assert(!memcmp(&cpu,&registers,sizeof(cpu)));
    if (i) assert(!memcmp(&actors,&before,sizeof(actors)));
    before=actors;copies=calls=0;
    assert(ActRaiser_RegionalSimCacheSave(&cpu)==token && calls==1 && copies==(i==0));
    if(i) assert(!memcmp(&actors,&before,sizeof(actors)));
  }
  for(unsigned town=0;town<6;++town)for(unsigned slot=0;slot<4;++slot)for(unsigned species=0;species<4;++species) {
    Actor(town,slot,species);assert(ArRegionalSimActors_LoadTown(&actors,town));requested=31;
    CpuState cpu={.PB=3,.DB=0x7f,.S=0x1ed0,.X=0x0b30+slot*0x26,.Y=town*8+slot*2};
    assert(ActRaiser_RegionalSimBirthEntry(&cpu));captures=0;
    assert(ActRaiser_RegionalSimBirth(&cpu)==RECOMP_RETURN_TAILCALL && captures==1 && target==0x03b9f1 && origin==0x03b9ee);
    assert(cpu.A==0 && cpu._flag_Z && actors.active[slot].combat==31);
    requested=0;cpu=(CpuState){.PB=1,.DB=1,.X=species,.Y=0x0b30+slot*0x26,.P=CPU_P_M|CPU_P_C};cpu_p_to_mirrors(&cpu);
    assert(ActRaiser_RegionalSimCollisionEntry(&cpu));
    ram[cpu.Y+0x0f]=0x80; /* non-species actor state must not disable the adapter */
    assert(ActRaiser_RegionalSimCollisionEntry(&cpu));
    assert(ActRaiser_RegionalSimThreshold(&cpu)==RECOMP_RETURN_TAILCALL && target==0x01b01c && origin==0x01b018);
    assert(ActRaiser_RegionalSimContact(&cpu)==RECOMP_RETURN_TAILCALL && target==0x01b0d0 && origin==0x01b0cc);
    assert(captures==1 && actors.active[slot].combat==31); /* settings edits never repin */
    unsigned t,s;const CpuState valid=cpu;
    cpu.DB=0;assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));cpu=valid;
    cpu.X=4;assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));cpu=valid;
    cpu.Y++;assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));cpu=valid;
    cpu.D=1;assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));cpu=valid;
    cpu.m_flag=0;assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));cpu=valid;
    cpu.x_flag=1;assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));cpu=valid;
    cpu.P|=CPU_P_D;assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));cpu=valid;
    cpu._flag_D=1;assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));cpu=valid;
    cpu.emulation=1;assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));cpu=valid;
    Word(town_ram,0x7bfb,12);assert(!ActRaiserSimCombat_CollisionSlot(&cpu,&t,&s));
  }
  ready=false;CpuState cpu={.PB=3};assert(!ActRaiser_RegionalSimCacheEntry(&cpu));assert(!writes);
  puts("SIM combat: mixed rules, pinned live/cache generations, codec, native flags and guarded continuations passed");return 0;
}
