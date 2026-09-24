#include "actraiser/actraiser_actor_stats.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "regional/action/regional_actor_stats.h"
#include "regional/action/regional_platform_skull.h"
#include "regional/action/regional_cast_hold.h"
#include "actraiser/actraiser_cast_hold.h"
#include "actraiser/actraiser_difficulty.h"
#include "regional/action/regional_difficulty.h"
#include "randomizer.h"
#include "settings.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t memory[65536],skull;
static uint8_t cast_hold;
Settings g_settings;
const SettingDesc *Settings_Find(const char *key){(void)key;return NULL;}
SettingChangeResult Settings_SetLong(const SettingDesc *d,long value){(void)d;(void)value;return 0;}
static ArRegionalDifficultySnapshot difficulty;
ArRegionalDifficultySnapshot ActRaiserRegional_DifficultySnapshot(void){return difficulty;}
uint8_t ActRaiserRegional_CastHoldSnapshot(void){return cast_hold;}
static ArRegionalActorStatsSnapshot snapshot;
static unsigned target,origin;
bool ActRaiserRegional_ActorStatsEnabled(void){return snapshot.changed;}
uint16_t ActRaiserRegional_ActorChildStat(unsigned rule) {
  if(rule<kArRegionalActorStat_BaseCount || rule>=kArRegionalActorStat_Count)return UINT16_MAX;
  return snapshot.value[rule]?snapshot.value[rule]:UINT16_MAX;
}
bool ActRaiserRegional_ActorStats(uint16_t actor,uint16_t hp,uint16_t attack,uint16_t *out_hp,uint16_t *out_attack) {
  return ArRegionalActorStats_Apply(&snapshot,actor,hp,attack,out_hp,out_attack);
}
uint8_t ActRaiserRegional_PlatformSkullSnapshot(void){return skull;}
int cpu_hle_tailcall_request(uint32 pc,uint32 from){target=pc;origin=from;return 1;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at){(void)cpu;assert(!bank);return memory[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at){return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value){(void)cpu;assert(!bank);memory[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value){cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
static void Write(unsigned at,uint16_t value){ByteOrder_WriteLe16(memory+at,value);}
typedef struct Owner { uint16_t actor,source[5]; } Owner;
static const Owner kOwners[]={
  {0x0102,{0xa9da,0xaa6e,0xa684,0xa686,0xa689}},
  {0x0104,{0xad45,0xadd9,0xa9ef,0xa9f1,0xa9f4}},
  {0x010a,{0xaf5d,0xaff1,0xac07,0xac09,0xac0c}},
  {0x010e,{0xb0b4,0xb148,0xad5e,0xad60,0xad63}},
  {0x0110,{0xb155,0xb1e9,0xadff,0xae01,0xae04}},
  {0x0112,{0xb191,0xb225,0xae3b,0xae3d,0xae40}},
  {0x0113,{0xb1af,0xb243,0xae59,0xae5b,0xae5e}},
  {0x0117,{0xb379,0xb40d,0xb023,0xb025,0xb028}},
  {0x0204,{0xb61a,0xb6ae,0xb2d2,0xb2d4,0xb2d7}},
  {0x0223,{0xbba8,0xbc3c,0xb860,0xb862,0xb865}},
  {0x0225,{0xbd2a,0xbdbe,0xb9e2,0xb9e4,0xb9e7}},
  {0x0227,{0xbdff,0xbe9c,0xbac0,0xbac2,0xbac5}},
  {0x0304,{0xc66f,0xc6fe,0xc336,0xc338,0xc33b}},
  {0x0306,{0xc3a5,0xc43a,0xc06c,0xc06e,0xc071}},
  {0x0307,{0xc45f,0xc4ee,0xc126,0xc128,0xc12b}},
  {0x030b,{0xc1a2,0xc239,0xbe63,0xbe65,0xbe68}},
  {0x030c,{0xc80e,0xc89f,0xc4d5,0xc4d7,0xc4da}},
  {0x0310,{0xc961,0xc9ec,0xc628,0xc62a,0xc62d}},
  {0x0311,{0xc9be,0xca49,0xc685,0xc687,0xc68a}},
  {0x0312,{0xca8b,0xcb16,0xc752,0xc754,0xc757}},
  {0x0316,{0xcb7b,0xcbfd,0xc842,0xc844,0xc847}},
  {0x0405,{0xceec,0xcf71,0xcbb3,0xcbb5,0xcbb8}},
  {0x0406,{0xcf2e,0xcfb3,0xcbf5,0xcbf7,0xcbfa}},
  {0x0408,{0xcf9e,0xd01e,0xcc65,0xcc67,0xcc6a}},
  {0x0409,{0xd025,0xd0a5,0xccec,0xccee,0xccf1}},
  {0x040a,{0xd033,0xd0b3,0xccfa,0xccfc,0xccff}},
  {0x040b,{0xd2a2,0xd324,0xcf69,0xcf6b,0xcf6e}},
  {0x040c,{0xd382,0xd404,0xd049,0xd04b,0xd04e}},
  {0x040d,{0xd646,0xd6c8,0xd30d,0xd30f,0xd312}},
  {0x0414,{0xd4a1,0xd523,0xd168,0xd16a,0xd16d}},
  {0x0416,{0xd5b1,0xd633,0xd278,0xd27a,0xd27d}},
  {0x0417,{0xd5c0,0xd642,0xd287,0xd289,0xd28c}},
  {0x0507,{0xde08,0xde9e,0xdb07,0xdb09,0xdb0c}},
  {0x050b,{0xdc46,0xdcdc,0xd93b,0xd93d,0xd940}},
  {0x0512,{0xe0ba,0xe14b,0xddb9,0xddbb,0xddbe}},
  {0x0513,{0xe18e,0xe21f,0xde8d,0xde8f,0xde92}},
  {0x0514,{0xe254,0xe2d5,0xdf53,0xdf55,0xdf58}},
  {0x0515,{0xe292,0xe313,0xdf91,0xdf93,0xdf96}},
  {0x0516,{0xe2a6,0xe327,0xdfa5,0xdfa7,0xdfaa}},
  {0x0517,{0xe2bd,0xe33e,0xdfbc,0xdfbe,0xdfc1}},
  {0x0520,{0xe3a1,0xe422,0xe0a0,0xe0a2,0xe0a5}},
  {0x0608,{0xf161,0xf1e0,0xee66,0xee68,0xee6b}},
  {0x0609,{0xe952,0xe9d1,0xe657,0xe659,0xe65c}},
  {0x0616,{0xed4b,0xedca,0xea50,0xea52,0xea55}},
  {0x061a,{0xefc8,0xf047,0xeccd,0xeccf,0xecd2}},
  {0x061b,{0xefd6,0xf055,0xecdb,0xecdd,0xece0}},
  {0x061c,{0xf0e6,0xf165,0xedeb,0xeded,0xedf0}},
  {0x0700,{0xf6ca,0xf749,0xf3cf,0xf3d1,0xf3d4}},
  {0x0701,{0xf6e2,0xf761,0xf3e7,0xf3e9,0xf3ec}},
  {0x0702,{0xf6fa,0xf779,0xf3ff,0xf401,0xf404}},
  {0x0703,{0xf712,0xf791,0xf417,0xf419,0xf41c}},
  {0x0704,{0xf72a,0xf7a9,0xf42f,0xf431,0xf434}},
  {0x0705,{0xf760,0xf7df,0xf465,0xf467,0xf46a}},
  {0x0715,{0xf751,0xf7d0,0xf456,0xf458,0xf45b}},
};
static CpuState Setup(const Owner *owner,uint16_t hp,uint16_t attack) {
  memset(memory,0,0x8000);const unsigned x=0x8e0,source=owner->source[0];memory[0x18]=owner->actor>>8;
  Write(x+0x16,ByteOrder_ReadLe16(memory+source));memory[x+0x18]=memory[source+2];
  Write(x+0x1a,memory[source+6]);Write(x+0x30,ByteOrder_ReadLe16(memory+source+4));
  Write(x+0x2c,hp);Write(x+0x2a,attack);Write(x+0x2e,memory[source+9]);Write(x+0x32,0xbeef);
  CpuState cpu={.A=0xa5a5,.X=x,.Y=source,.S=0x1ef0,.P=CPU_P_V|CPU_P_C};cpu_p_to_mirrors(&cpu);return cpu;
}
static uint16_t ExpectedScale(unsigned value,unsigned percent) {
  if(!value)return 0;
  unsigned scaled=(value*percent+50)/100;
  return scaled<1?1:scaled>255?255:(uint16_t)scaled;
}
static unsigned RunOwnerScaled(const Owner *owner,const uint8_t *pristine) {
  unsigned cases=0;
  const unsigned source=owner->source[0];
  const uint16_t copied_hp=memory[source+8],copied_attack=memory[source+7];
  const uint16_t native_hp=pristine?pristine[source-0x8000+8]:copied_hp;
  const uint16_t native_attack=pristine?pristine[source-0x8000+7]:copied_attack;
  const RandomizerStatScale scale=Randomizer_AppliedStatScale();
  for(unsigned health=0;health<3;++health)for(unsigned contact=0;contact<3;++contact)for(unsigned armor=0;armor<4;++armor) {
    ArRegionalActorStatsPolicy policy={{0}};uint16_t hp=native_hp,attack=native_attack;
    for(unsigned i=0;i<kArRegionalActorStat_Count;++i) {
      const ArRegionalActorStatDescriptor *d=ArRegionalActorStats_Descriptor(i);
      if(d->actor!=owner->actor)continue;
      policy.source[i]=d->field==kArRegionalActorStat_HP?health:contact;
      if(d->field==kArRegionalActorStat_HP)hp=d->value[health];else attack=d->value[contact];
    }
    hp=ExpectedScale(hp,scale.hp_percent);attack=ExpectedScale(attack,scale.attack_percent);
    assert(ArRegionalActorStats_Resolve(&policy,&snapshot));skull=armor;
    CpuState cpu=Setup(owner,copied_hp,copied_attack),expected=cpu;
    const bool skull_changed=owner->actor==0x040c && armor;
    const bool changed=hp!=copied_hp || attack!=copied_attack || skull_changed;
    uint8_t wanted[65536];memcpy(wanted,memory,sizeof(memory));
    assert(ActRaiser_ActorStatsEntry(&cpu)==changed);
    if(changed) {
      ByteOrder_WriteLe16(wanted+cpu.X+0x2c,hp);ByteOrder_WriteLe16(wanted+cpu.X+0x2a,attack);
      if(skull_changed) {
        ByteOrder_WriteLe16(wanted+cpu.X+0x30,armor&1?0x800:0);
        ByteOrder_WriteLe16(wanted+cpu.X+0x2e,armor&2?0:0x20);
      }
      expected.A=ByteOrder_ReadLe16(wanted+cpu.X+0x30);ActRaiserCpuHle_SetNegativeZero16(&expected,expected.A);
      assert(ActRaiser_ActorStats(&cpu)==RECOMP_RETURN_TAILCALL && target==0x966f && origin==0x966c);
    }
    assert(!memcmp(wanted,memory,sizeof(memory)) && !memcmp(&cpu,&expected,sizeof(cpu)));++cases;
  }
  return cases;
}
static unsigned RunOwner(const Owner *owner){return RunOwnerScaled(owner,NULL);}
static void CheckSynthetic(void) {
  memset(memory,0,sizeof(memory));unsigned cases=0;
  for(unsigned o=0;o<sizeof(kOwners)/sizeof(kOwners[0]);++o) {
    const Owner *owner=&kOwners[o];const unsigned at=owner->source[0];
    Write(at,0x4000);memory[at+2]=0x7e;memory[at+6]=owner->actor==0x040c?47:20;
    memory[at+7]=7;memory[at+8]=7;memory[at+9]=0x20;
    for(unsigned i=0;i<kArRegionalActorStat_Count;++i) {
      const ArRegionalActorStatDescriptor *d=ArRegionalActorStats_Descriptor(i);
      if(d->actor==owner->actor)memory[at+(d->field==kArRegionalActorStat_HP?8:7)]=(uint8_t)d->value[0];
    }
    cases+=RunOwner(owner);
  }
  ArRegionalActorStatsPolicy policy;assert(ArRegionalActorStats_Init(&policy,2) && ArRegionalActorStats_Resolve(&policy,&snapshot));skull=0;
  for(unsigned bad=0;bad<17;++bad) {
    const Owner *owner=&kOwners[0];CpuState cpu=Setup(owner,1,1);
    assert(ActRaiser_ActorStatsEntry(&cpu));
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x1aa0;break;case 10:memory[0x18]=7;break;case 11:cpu.Y=0;break;
      case 12:Write(cpu.X+0x16,0x5000);break;case 13:memory[cpu.X+0x18]=0x7f;break;
      case 14:Write(cpu.X+0x1a,21);break;case 15:Write(cpu.X+0x2c,99);break;case 16:Write(cpu.X+0x2a,99);break;
    }
    assert(!ActRaiser_ActorStatsEntry(&cpu));
  }
  for(unsigned i=0;i<kArRegionalActorStat_Count;++i) {
    ArRegionalActorStatsPolicy invalid=policy;invalid.source[i]=3;
    const ArRegionalActorStatsSnapshot before=snapshot;
    assert(!ArRegionalActorStats_Resolve(&invalid,&snapshot) && !memcmp(&before,&snapshot,sizeof(snapshot)));
    if(i)assert(ArRegionalActorStats_Descriptor(i-1)->actor<=ArRegionalActorStats_Descriptor(i)->actor);
  }
  printf("base actor stats: %u mixed-field/skull-birth cases and guards passed\n",cases);
}
static CpuState Child(unsigned slot,bool projectile,unsigned inherited,unsigned flags) {
  memset(memory,0xa5,0x8000);const unsigned x=0x8a0+slot*0x40;
  Write(0x18,0x0807);Write(x,0);Write(x+0x30,projectile?0x20:0);
  Write(x+0x12,projectile?0xfd25:0xfc8d);Write(x+0x32,0xf80f);
  Write(x+0x16,0x5000);memory[x+0x18]=0x7e;
  Write(x+0x2a,inherited);Write(x+0x2c,inherited);Write(x+0x2e,inherited);
  CpuState cpu={.A=projectile?0x20:2,.X=x,.Y=0xf80f,.S=0x1ef0,.P=(uint8_t)flags};
  cpu_p_to_mirrors(&cpu);return cpu;
}
static void CheckChildren(void) {
  unsigned cases=0;
  const RandomizerStatScale scale=Randomizer_AppliedStatScale();
  const unsigned slots[]={0,19,71},inherited[]={0,2,0xff,0xffff},flags[]={0,CPU_P_C|CPU_P_V,CPU_P_Z,CPU_P_N};
  for(unsigned hp=0;hp<3;++hp)for(unsigned reward=0;reward<3;++reward)for(unsigned attack=0;attack<3;++attack) {
    ArRegionalActorStatsPolicy policy={{0}};
    policy.source[kArRegionalActorStat_TanzraMinionHp]=hp;
    policy.source[kArRegionalActorStat_TanzraMinionReward]=reward;
    policy.source[kArRegionalActorStat_TanzraProjectileAttack]=attack;
    assert(ArRegionalActorStats_Resolve(&policy,&snapshot) && !snapshot.changed);
    const uint16_t wanted_hp=ExpectedScale(hp==2?1:2,scale.hp_percent);
    const uint16_t wanted_attack=ExpectedScale(3+attack,scale.attack_percent);
    uint16_t ignored=0xaaaa;assert(!ArRegionalActorStats_Apply(&snapshot,0x0800,2,3,&ignored,&ignored) && ignored==0xaaaa);
    for(unsigned slot=0;slot<3;++slot)for(unsigned n=0;n<4;++n)for(unsigned f=0;f<4;++f) {
      CpuState cpu=Child(slots[slot],false,inherited[n],flags[f]),expected=cpu;
      uint8_t wanted[65536];memcpy(wanted,memory,sizeof(wanted));
      assert(ActRaiser_TanzraMinionHpEntry(&cpu)==(wanted_hp!=2));
      if(wanted_hp!=2)assert(ActRaiser_TanzraMinionHp(&cpu)==RECOMP_RETURN_TAILCALL && target==0xfc99 && origin==0xfc96);
      else Write(cpu.X+0x2c,cpu.A); /* unchanged native STA */
      assert(ActRaiser_TanzraMinionRewardEntry(&cpu)==(reward==2));
      if(reward==2)assert(ActRaiser_TanzraMinionReward(&cpu)==RECOMP_RETURN_TAILCALL && target==0xfc9c && origin==0xfc99);
      else Write(cpu.X+0x2e,cpu.A);
      ByteOrder_WriteLe16(wanted+cpu.X+0x2c,wanted_hp);
      ByteOrder_WriteLe16(wanted+cpu.X+0x2e,reward==2?1:2);
      assert(!memcmp(wanted,memory,sizeof(wanted)) && !memcmp(&cpu,&expected,sizeof(cpu)));++cases;
      cpu=Child(slots[slot],true,inherited[n],flags[f]);expected=cpu;memcpy(wanted,memory,sizeof(wanted));
      assert(ActRaiser_TanzraProjectileAttackEntry(&cpu)==(wanted_attack!=3));
      if(wanted_attack!=3)assert(ActRaiser_TanzraProjectileAttack(&cpu)==RECOMP_RETURN_TAILCALL && target==0xfd31 && origin==0xfd2e);
      else {cpu.A=3;ActRaiserCpuHle_SetNegativeZero16(&cpu,cpu.A);}
      expected.A=wanted_attack;ActRaiserCpuHle_SetNegativeZero16(&expected,expected.A);
      assert(!memcmp(wanted,memory,sizeof(wanted)) && !memcmp(&cpu,&expected,sizeof(cpu)));
      Write(cpu.X+0x2a,cpu.A);ByteOrder_WriteLe16(wanted+cpu.X+0x2a,wanted_attack);
      assert(!memcmp(wanted,memory,sizeof(wanted)));++cases;
    }
  }
  ArRegionalActorStatsPolicy policy;assert(ArRegionalActorStats_Init(&policy,2) && ArRegionalActorStats_Resolve(&policy,&snapshot));
  for(unsigned projectile=0;projectile<2;++projectile)for(unsigned bad=0;bad<20;++bad) {
    CpuState cpu=Child(0,projectile,0,CPU_P_C|CPU_P_V);
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;case 8:cpu.X++;break;
      case 9:cpu.X=0x1aa2;break;case 10:cpu.X=0x660;break;case 11:Write(0x18,0x0707);break;
      case 12:Write(cpu.X+0x32,0xf760);break;case 13:Write(cpu.X+0x16,0x4000);break;
      case 14:memory[cpu.X+0x18]=0x7f;break;case 15:Write(cpu.X+0x12,0xfcd9);break;
      case 16:Write(cpu.X,0x4000);break;case 17:Write(cpu.X+0x30,0x400);break;
      case 18:cpu.A=99;break;case 19:snapshot=(ArRegionalActorStatsSnapshot){0};break;
    }
    const CpuState before=cpu;uint8_t wanted[65536];memcpy(wanted,memory,sizeof(wanted));
    assert(!ActRaiser_TanzraMinionHpEntry(&cpu) && !ActRaiser_TanzraMinionRewardEntry(&cpu) && !ActRaiser_TanzraProjectileAttackEntry(&cpu));
    assert(!memcmp(&before,&cpu,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(wanted)));
    assert(ArRegionalActorStats_Resolve(&policy,&snapshot));
  }
  assert(!ActRaiser_TanzraMinionHpEntry(NULL) && !ActRaiser_TanzraMinionRewardEntry(NULL) && !ActRaiser_TanzraProjectileAttackEntry(NULL));
  printf("child stats: %u mixed/inherited-value cases and 40 invalid-owner/CPU cases passed\n",cases);
}
static CpuState Prop(unsigned rule,unsigned slot,unsigned flags,unsigned stale) {
  static const uint16_t records[]={0xac02,0xac32,0xac5e};
  memset(memory,0,0x8000);const unsigned x=0x6a0+slot*0x40;
  memory[0x18]=1;Write(x+0x16,0x4000);memory[x+0x18]=0x7e;
  Write(x+0x1a,38+rule);Write(x+0x30,0x32);Write(x+0x32,stale);
  CpuState cpu={.A=0xa5a5,.X=x,.Y=records[rule],.S=0x1ef0,.P=(uint8_t)flags};
  cpu_p_to_mirrors(&cpu);return cpu;
}
static void CheckCastHold(void) {
  snapshot=(ArRegionalActorStatsSnapshot){0};skull=0;unsigned cases=0;
  const unsigned flags[]={0,CPU_P_C|CPU_P_V,CPU_P_Z,CPU_P_N};
  const unsigned slots[]={0,31,79},stale[]={0,0xac02,0xf80f,0xffff};
  for(unsigned n=0;n<27;++n) {
    ArRegionalCastHoldPolicy policy;unsigned digits=n;
    for(unsigned i=0;i<3;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalCastHold_Resolve(&policy,&cast_hold));
    for(unsigned rule=0;rule<3;++rule)for(unsigned s=0;s<3;++s)
      for(unsigned f=0;f<4;++f)for(unsigned old=0;old<4;++old) {
        CpuState cpu=Prop(rule,slots[s],flags[f],stale[old]),expected=cpu;
        uint8_t wanted[65536];memcpy(wanted,memory,sizeof(wanted));
        const bool changed=policy.source[rule]==2;
        assert(ActRaiser_ActorStatsEntry(&cpu)==changed);
        if(changed) {
          ByteOrder_WriteLe16(wanted+cpu.X+0x30,0x8032);
          expected.A=0x8032;ActRaiserCpuHle_SetNegativeZero16(&expected,expected.A);
          assert(ActRaiser_ActorStats(&cpu)==RECOMP_RETURN_TAILCALL && target==0x966f && origin==0x966c);
        }
        assert(!memcmp(wanted,memory,sizeof(wanted)) && !memcmp(&cpu,&expected,sizeof(cpu)));++cases;
      }
  }
  for(unsigned bad=0;bad<22;++bad) {
    CpuState cpu=Prop(0,0,0,0xffff);cast_hold=7;
    switch(bad) {
      case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
      case 3:cpu.m_flag=1;break;case 4:cpu.x_flag=1;break;case 5:cpu.emulation=1;break;
      case 6:cpu._flag_D=1;break;case 7:cpu.P|=CPU_P_D;break;
      case 8:cpu.X++;break;case 9:cpu.X=0x660;break;case 10:cpu.X=0x1aa0;break;
      case 11:cpu.Y++;break;case 12:memory[0x18]=2;break;
      case 13:Write(cpu.X+0x16,0x5000);break;case 14:memory[cpu.X+0x18]=0x7f;break;
      case 15:Write(cpu.X+0x1a,39);break;case 16:Write(cpu.X+0x30,0x8032);break;
      case 17:Write(cpu.X+0x2a,1);break;case 18:Write(cpu.X+0x2c,1);break;
      case 19:Write(cpu.X+0x2e,1);break;case 20:cast_hold=0;break;case 21:cast_hold=8;break;
    }
    const CpuState before=cpu;uint8_t wanted[65536];memcpy(wanted,memory,sizeof(wanted));
    assert(!ActRaiser_CastHoldSpawnEntry(&cpu) && !ActRaiser_ActorStatsEntry(&cpu));
    assert(!memcmp(wanted,memory,sizeof(wanted)) && !memcmp(&cpu,&before,sizeof(cpu)));
  }
  assert(!ActRaiser_CastHoldSpawnEntry(NULL));cast_hold=0;
  printf("linked-prop cast hold: %u mixed-policy/reused-slot cases and 22 guards passed\n",cases);
}
static void CheckRoms(char **paths) {
  static uint8_t rom[5][1048576];unsigned cases=0;
  for(unsigned region=0;region<5;++region) {
    FILE *file=fopen(paths[region],"rb");assert(file && fread(rom[region],1,sizeof(rom[region]),file)==sizeof(rom[region]) && !fclose(file));
    const unsigned minion[]={0xfc8d,0xfd0a,0xf986,0xf988,0xf98b},projectile[]={0xfd25,0xfda2,0xfa1e,0xfa20,0xfa23};
    uint8_t minion_prefix[]={0x9e,0,0,0x9e,0x30,0,0xa9,2,0,0x9d,0x2c,0,0x9d,0x2e,0};
    uint8_t projectile_prefix[]={0x9e,0,0,0xa9,0x20,0,0x9d,0x30,0,0xa9,3,0,0x9d,0x2a,0,0xa9,6,0};
    const unsigned source=region>=2?2:region;
    const unsigned prop[5][3]={{0xac02,0xac32,0xac5e},{0xac96,0xacc6,0xacf2},
      {0xa8ac,0xa8dc,0xa908},{0xa8ae,0xa8de,0xa90a},{0xa8b1,0xa8e1,0xa90d}};
    for(unsigned i=0;i<3;++i) {
      const uint8_t *p=rom[region]+prop[region][i]-0x8000;
      assert(ByteOrder_ReadLe16(p)==0x4000 && p[2]==0x7e && p[6]==38+i);
      assert(ByteOrder_ReadLe16(p+4)==(source==2?0x8032:0x32) && !p[7] && !p[8] && !p[9]);
    }
    minion_prefix[7]=(uint8_t)ArRegionalActorStats_Descriptor(kArRegionalActorStat_TanzraMinionHp)->value[source];
    assert(minion_prefix[7]==ArRegionalActorStats_Descriptor(kArRegionalActorStat_TanzraMinionReward)->value[source]);
    projectile_prefix[10]=(uint8_t)ArRegionalActorStats_Descriptor(kArRegionalActorStat_TanzraProjectileAttack)->value[source];
    assert(!memcmp(rom[region]+minion[region]-0x8000,minion_prefix,sizeof(minion_prefix)));
    assert(!memcmp(rom[region]+projectile[region]-0x8000,projectile_prefix,sizeof(projectile_prefix)));
  }
  memcpy(memory+0x8000,rom[0],0x8000);
  for(unsigned o=0;o<sizeof(kOwners)/sizeof(kOwners[0]);++o) {
    const Owner *owner=&kOwners[o];cases+=RunOwner(owner);
    for(unsigned i=0;i<kArRegionalActorStat_Count;++i) {
      const ArRegionalActorStatDescriptor *d=ArRegionalActorStats_Descriptor(i);if(d->actor!=owner->actor)continue;
      for(unsigned region=0;region<5;++region) {
        const uint8_t *record=rom[region]+owner->source[region]-0x8000;
        assert(record[d->field==kArRegionalActorStat_HP?8:7]==d->value[region>=2?2:region]);
      }
    }
  }
  printf("five-ROM stat bytes: 63 base fields and 3 child overrides x5 releases; %u actual-record adapter combinations passed\n",cases);
}
static void CheckDifficultyOrder(const Owner *owner) {
  ArRegionalActorStatsPolicy policy;
  assert(ArRegionalActorStats_Init(&policy,2) && ArRegionalActorStats_Resolve(&policy,&snapshot));
  skull=cast_hold=0;
  const unsigned source=owner->source[0];
  for(unsigned mode=1;mode<=3;++mode) {
    difficulty=(ArRegionalDifficultySnapshot){.spawn_hp=mode};
    CpuState cpu=Setup(owner,memory[source+8],memory[source+7]);
    if(ActRaiser_ActorStatsEntry(&cpu))assert(ActRaiser_ActorStats(&cpu)==RECOMP_RETURN_TAILCALL);
    else {cpu.A=cpu_read16(&cpu,0,cpu.X+0x30);ActRaiserCpuHle_SetNegativeZero16(&cpu,cpu.A);}
    const uint16_t hp=cpu_read16(&cpu,0,cpu.X+0x2c);
    const bool eligible=!(cpu.A&0x8231);
    const uint16_t wanted=eligible && mode==2 && hp==2?1:eligible && mode==3 && hp==1?2:hp;
    assert(ActRaiser_DifficultySpawnEntry(&cpu));
    assert(ActRaiser_DifficultySpawn(&cpu)==RECOMP_RETURN_TAILCALL && target==0x968f && origin==0x966f);
    assert(cpu_read16(&cpu,0,cpu.X+0x2c)==wanted);
  }
  difficulty=(ArRegionalDifficultySnapshot){0};
}
static void CheckComposition(const char *path) {
  static uint8_t rom[0x100000],pristine[0x100000];
  if(path) {
    FILE *file=fopen(path,"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
  } else {
    memcpy(rom,memory+0x8000,0x8000);
    static const unsigned tables[]={0x96af,0xa8f6,0xb449,0xc11e,0xcd9b,0xd928,0xe722,0xf39a};
    for(unsigned o=0;o<sizeof(kOwners)/sizeof(kOwners[0]);++o) {
      const Owner *owner=&kOwners[o];
      ByteOrder_WriteLe16(rom+(tables[owner->actor>>8]&0x7fff)+2*(owner->actor&255),owner->source[0]);
    }
  }
  memcpy(pristine,rom,sizeof(rom));assert(Randomizer_Init(rom,sizeof(rom)));
  const unsigned scales[][2]={{100,100},{200,100},{100,200},{200,200},{10,1000},{1000,10},{50,125},{125,50},{1000,1000}};
  unsigned cases=0;
  for(unsigned s=0;s<sizeof(scales)/sizeof(scales[0]);++s) {
    g_settings=(Settings){.rando_enable=true,.rando_enemy_hp=scales[s][0],.rando_enemy_atk=scales[s][1]};
    Randomizer_Apply();memcpy(memory+0x8000,rom,0x8000);
    for(unsigned o=0;o<sizeof(kOwners)/sizeof(kOwners[0]);++o) {
      const Owner *owner=&kOwners[o];const unsigned offset=owner->source[0]&0x7fff;
      assert(rom[offset+8]==ExpectedScale(pristine[offset+8],scales[s][0]));
      assert(rom[offset+7]==ExpectedScale(pristine[offset+7],scales[s][1]));
      cases+=RunOwnerScaled(owner,pristine);
      CheckDifficultyOrder(owner);
    }
    CheckChildren();
    Randomizer_Apply();assert(!memcmp(memory+0x8000,rom,0x8000)); /* no compounding */
  }
  /* Neither edits that have not been applied nor a damaged/stale actor may
   * silently supply a different base or percentage at the fresh-spawn seam. */
  g_settings.rando_enable=false;
  assert(Randomizer_AppliedStatScale().hp_percent==1000);
  const Owner *owner=&kOwners[0];const unsigned at=owner->source[0];
  ArRegionalActorStatsPolicy policy;
  assert(ArRegionalActorStats_Init(&policy,2) && ArRegionalActorStats_Resolve(&policy,&snapshot));
  CpuState cpu=Setup(owner,memory[at+8]+1,memory[at+7]);
  assert(!ActRaiser_ActorStatsEntry(&cpu));
  cpu=Setup(owner,memory[at+8],memory[at+7]+1);assert(!ActRaiser_ActorStatsEntry(&cpu));
  Randomizer_Apply();assert(!memcmp(rom,pristine,sizeof(rom)));
  assert(Randomizer_AppliedStatScale().hp_percent==100 && Randomizer_AppliedStatScale().attack_percent==100);
  memcpy(memory+0x8000,rom,0x8000);skull=0;
  for(unsigned o=0;o<sizeof(kOwners)/sizeof(kOwners[0]);++o)cases+=RunOwner(&kOwners[o]);
  CheckChildren();
  printf("regional/randomizer composition: %u full CPU/memory cases plus child, difficulty-order and reapply checks (%s)\n",cases,path?"US ROM":"synthetic");
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);CheckSynthetic();CheckChildren();CheckCastHold();if(argc==6)CheckRoms(argv+1);
  CheckComposition(argc==6?argv[1]:NULL);return 0;
}
