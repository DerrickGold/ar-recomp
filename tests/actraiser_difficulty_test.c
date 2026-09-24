#include "actraiser/actraiser_difficulty.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "regional/action/regional_difficulty.h"
#include "byte_order.h"
#include "quintet_lzss.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t memory[65536];
static ArRegionalDifficultySnapshot snapshot;
static unsigned target,origin;
ArRegionalDifficultySnapshot ActRaiserRegional_DifficultySnapshot(void) {return snapshot;}
int cpu_hle_tailcall_request(uint32_t pc,uint32_t from) {target=pc;origin=from;return 1;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at) {(void)cpu;assert(!bank);return memory[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at) {return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value) {(void)cpu;assert(!bank);memory[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value) {cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
static void Write(unsigned at,uint16_t value) {ByteOrder_WriteLe16(memory+at,value);}
static CpuState Setup(unsigned narrow) {
  memset(memory,0,sizeof(memory));target=origin=0;
  CpuState cpu={.X=0x8e0,.Y=0xa934,.S=0x1e0,.A=0,.P=(uint8_t)(CPU_P_C|CPU_P_V|(narrow?CPU_P_M:0))};
  cpu_p_to_mirrors(&cpu);
  Write(cpu.Y,0x4000);memory[cpu.Y+2]=0x7e;memory[cpu.Y+6]=9;
  Write(cpu.X+0x16,0x4000);memory[cpu.X+0x18]=0x7e;Write(cpu.X+0x1a,9);
  return cpu;
}
static void Policies(void) {
  for(unsigned n=0;n<243;++n)for(unsigned level=0;level<3;++level) {
    ArRegionalDifficultyPolicy p={.level=(ArRegionalDifficulty)level};unsigned digits=n;
    for(unsigned i=0;i<5;++i) {p.source[i]=digits%3;digits/=3;}
    assert(ArRegionalDifficulty_Resolve(&p,&snapshot));
    assert(snapshot.spawn_hp==(p.source[0]==2?1+level:0));
    assert(snapshot.contact_extra==(p.source[1]==2 && level==2));
    assert(snapshot.timer_reload==(p.source[2]==2?(level==1?71:level==2?47:59):59));
    assert(snapshot.skip_dragon_attack==(p.source[3]==2 && level==1));
    assert(snapshot.single_tendril_bob==(p.source[4]==2 && level==1));
    assert(!(ArRegionalDifficulty_Identity(&snapshot)&128));
    for(unsigned flags=0;flags<65536;++flags)for(unsigned hp=0;hp<4;++hp) {
      const unsigned expected=p.source[0]!=2 || (flags&0x8231)?hp:
          level==1 && hp==2?1:level==2 && hp==1?2:hp;
      assert(ArRegionalDifficulty_SpawnHp(&snapshot,(uint16_t)flags,(uint16_t)hp)==expected);
    }
  }
  ArRegionalDifficultyPolicy p={0};ArRegionalDifficultySnapshot before=snapshot;
  p.level=kArRegionalDifficulty_Count;assert(!ArRegionalDifficulty_Resolve(&p,&snapshot));
  assert(!memcmp(&before,&snapshot,sizeof(snapshot)));p.level=0;p.source[0]=3;
  assert(!ArRegionalDifficulty_Resolve(&p,&snapshot));
  assert(!ArRegionalDifficulty_Init(NULL,0,0) && !ArRegionalDifficulty_Init(&p,3,0) &&
      !ArRegionalDifficulty_Init(&p,0,3) && !ArRegionalDifficulty_Descriptor(5));
}
static void Spawn(void) {
  for(unsigned region=0;region<3;++region)for(unsigned level=0;level<3;++level)
  for(unsigned action=0;action<2;++action)for(unsigned mask=0;mask<8;++mask)for(unsigned hp=0;hp<25;++hp) {
    const uint16_t flags[]={0,1,0x10,0x20,0x200,0x8000,0x8231,0x400};
    ArRegionalDifficultyPolicy p;assert(ArRegionalDifficulty_Init(&p,region,level));
    assert(ArRegionalDifficulty_Resolve(&p,&snapshot));CpuState cpu=Setup(0);
    cpu.A=flags[mask];Write(cpu.X+0x30,cpu.A);Write(cpu.X+0x2c,hp);Write(cpu.X+0x2a,1);Write(0x349,action);
    uint8_t before[65536];memcpy(before,memory,sizeof(before));
    assert(ActRaiser_DifficultySpawnEntry(&cpu)==(region==2));
    if(region==2) {
      assert(ActRaiser_DifficultySpawn(&cpu)==RECOMP_RETURN_TAILCALL && target==0x968f && origin==0x966f);
      assert(cpu_read16(&cpu,0,cpu.X+0x2a)==1);
      ByteOrder_WriteLe16(before+cpu.X+0x2c,ArRegionalDifficulty_SpawnHp(&snapshot,flags[mask],hp));
    }
    assert(!memcmp(before,memory,sizeof(before)) && cpu.S==0x1e0 && cpu.X==0x8e0 && cpu.Y==0xa934);
  }
}
static void Contacts(void) {
  snapshot=(ArRegionalDifficultySnapshot){.contact_extra=1};CpuState cpu=Setup(1);
  cpu.Y=0x8a0;Write(0x8a,cpu.Y);Write(cpu.Y+0x30,8);
  for(unsigned hp=0;hp<256;++hp)for(unsigned attack=0;attack<256;++attack) {
    cpu.A=0xa500|hp;cpu.P=CPU_P_C|CPU_P_M;cpu_p_to_mirrors(&cpu);memory[cpu.X+0x2a]=attack;
    assert(ActRaiser_DifficultyContactEntry(&cpu));
    uint8_t before[65536];memcpy(before,memory,sizeof(before));
    assert(ActRaiser_DifficultyContact(&cpu)==RECOMP_RETURN_TAILCALL && target==0x8a27 && origin==0x8a24);
    const unsigned damage=(attack+1)&255,value=(hp-damage)&255;
    assert(cpu.A==(0xa500|value) && cpu._flag_C==(hp>=damage) && cpu._flag_N==((value&128)!=0) &&
        cpu._flag_Z==(value==0) && cpu._flag_V==(((hp^damage)&(hp^value)&128)!=0));
    assert(!memcmp(memory,before,sizeof(memory)));
  }
}
static void TimersAndDragon(void) {
  for(unsigned level=0;level<3;++level) {
    ArRegionalDifficultyPolicy p;assert(ArRegionalDifficulty_Init(&p,2,level) && ArRegionalDifficulty_Resolve(&p,&snapshot));
    for(unsigned gate=0;gate<2;++gate)for(unsigned divider=0;divider<256;++divider) {
      CpuState cpu=Setup(1);cpu.PB=2;cpu.A=0xa57f;memory[0xe8]=gate;memory[0xe5]=divider;
      uint8_t before[65536];memcpy(before,memory,sizeof(before));
      const bool expected=level!=0 && !gate && (divider&128);
      assert(ActRaiser_DifficultyTimerEntry(&cpu)==expected);
      if(expected)assert(ActRaiser_DifficultyTimer(&cpu)==RECOMP_RETURN_TAILCALL && target==0x2bc8c &&
          origin==0x2bc8a && cpu.A==(0xa500|snapshot.timer_reload));
      assert(!memcmp(before,memory,sizeof(before)));
    }
    CpuState cpu=Setup(0);Write(0x18,0x0304);Write(cpu.X+0x32,0xd646);
    Write(cpu.X+0x16,0x5000);Write(cpu.X+0x1a,7);
    const CpuState before=cpu;uint8_t ram[65536];memcpy(ram,memory,sizeof(ram));
    assert(ActRaiser_DifficultyDragonEntry(&cpu)==(level==1));
    if(level==1)assert(ActRaiser_DifficultyDragon(&cpu)==RECOMP_RETURN_TAILCALL && target==0xd76b && origin==0xd766);
    assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(ram,memory,sizeof(ram)));
  }
}
static void Guards(void) {
  typedef bool (*Entry)(CpuState *);
  const Entry entries[]={ActRaiser_DifficultySpawnEntry,ActRaiser_DifficultyContactEntry,
      ActRaiser_DifficultyTimerEntry,ActRaiser_DifficultyDragonEntry};
  for(unsigned entry=0;entry<4;++entry)for(unsigned mutation=0;mutation<17;++mutation) {
    snapshot=(ArRegionalDifficultySnapshot){.spawn_hp=2,.contact_extra=1,
        .timer_reload=71,.skip_dragon_attack=true};
    CpuState cpu=Setup(entry==1 || entry==2);
    if(entry==1) {cpu.Y=0x8a0;Write(0x8a,cpu.Y);Write(cpu.Y+0x30,8);}
    if(entry==2) {cpu.PB=2;memory[0xe5]=255;}
    if(entry==3) {Write(0x18,0x0304);Write(cpu.X+0x32,0xd646);
      Write(cpu.X+0x16,0x5000);Write(cpu.X+0x1a,7);}
    assert(entries[entry](&cpu));
    switch(mutation) {
      case 0: cpu.PB^=1;break;
      case 1: cpu.DB=1;break;
      case 2: cpu.D=1;break;
      case 3: cpu.m_flag^=1;break;
      case 4: cpu.x_flag=1;break;
      case 5: cpu.emulation=1;break;
      case 6: cpu._flag_D=1;break;
      case 7: cpu.P|=CPU_P_D;break;
      default:
        if(entry==0) switch(mutation) {
          case 8: cpu.X++;break;case 9: cpu.X=0;break;
          case 10: cpu.Y=0x7fff;break;case 11: cpu.Y=0xfff5;break;
          case 12: cpu.A=1;break;case 13: Write(cpu.X+0x16,0);break;
          case 14: memory[cpu.X+0x18]=0;break;case 15: Write(cpu.X+0x1a,0);break;
          case 16: snapshot.spawn_hp=4;break;
        }
        else if(entry==1) switch(mutation) {
          case 8: cpu.X++;break;case 9: cpu.Y++;break;case 10: cpu.Y=cpu.X;break;
          case 11: Write(0x8a,0);break;case 12: cpu._flag_C=0;break;
          case 13: Write(cpu.Y+0x30,0);break;case 14: Write(cpu.X+0x30,0x200);break;
          default: snapshot.contact_extra=0;break;
        }
        else if(entry==2) switch(mutation) {
          case 8: memory[0xe8]=1;break;case 9: memory[0xe5]=127;break;
          default: snapshot.timer_reload=59;break;
        }
        else switch(mutation) {
          case 8: cpu.X++;break;case 9: Write(0x18,0x0305);break;
          case 10: Write(cpu.X+0x32,0xd647);break;case 11: Write(cpu.X+0x16,0x4000);break;
          case 12: memory[cpu.X+0x18]=0;break;case 13: Write(cpu.X+0x1a,8);break;
          case 14: Write(cpu.X+0x3a,0x8a0);break;
          default: snapshot.skip_dragon_attack=false;break;
        }
    }
    const CpuState before=cpu;uint8_t ram[65536];memcpy(ram,memory,sizeof(ram));
    assert(!entries[entry](&cpu) && !entries[entry](NULL));
    assert(!memcmp(&before,&cpu,sizeof(cpu)) && !memcmp(ram,memory,sizeof(ram)));
  }
}
static void Roms(char **paths) {
  static uint8_t rom[1048576],blobs[5][8192];
  const unsigned hp[]={0x166f,0x1697,0x116e,0x116e,0x116e};
  const unsigned timers[]={0x13c82,0x20c7d,0x1427a,0x14283,0x1426c};
  for(unsigned r=0;r<5;++r) {
    FILE *file=fopen(paths[r],"rb");assert(file && fread(rom,1,sizeof(rom),file)==sizeof(rom) && !fclose(file));
    assert(!memcmp(rom+hp[r],r<2?(uint8_t[]){0x89,1,2}:(uint8_t[]){0x89,0x31,0x82},3));
    assert(!memcmp(rom+timers[r],r<2?(uint8_t[]){0xa5,0xe8,0xd0,0x17,0xc6,0xe5}:
        (uint8_t[]){0xa5,0xe9,0xd0,0x18,0xc6,0xe6},6));
    assert(!memcmp(rom+timers[r]+8,r<2?(uint8_t[]){0xa9,59}:(uint8_t[]){0xad,0x4d},2));
    if(r>=2) {
      assert(!memcmp(rom+(r==4?0x1287d:0x12893),(uint8_t[]){71,59,47},3));
      const unsigned delta=r==3?2:r==4?5:0;
      assert(!memcmp(rom+0x5378+delta,(uint8_t[]){0xad,5,2,0xc9,1,0,0xf0,3,0x20},9));
      assert(!memcmp(rom+0x57c2+delta,(uint8_t[]){0xad,5,2,0xc9,1,0,0xf0,8,0xa9,2,16},11));
      assert(!memcmp(rom+0x57d2+delta,(uint8_t[]){0xa9,24,0,0x20,0x6f,0x85},6));
    }
    const unsigned offset=r==1?0xbdaf9:0xa749d,length=ByteOrder_ReadLe16(rom+offset);
    assert(length<=8192 && QuintetLzss_DecompressAsset(rom+offset,sizeof(rom)-offset,blobs[r],length,NULL));
    const unsigned sequence=ByteOrder_ReadLe16(blobs[r]+2+2*(r<2?16:24));
    const unsigned rows=r<2?4:6;
    const uint8_t expected[][4]={{27,3,255,255},{27,3,255,0},{27,15,255,0},{27,3,255,1},{27,3,255,0},{27,15,255,0}};
    if(r>=2)assert(!memcmp(blobs[r]+sequence,expected,sizeof(expected)));
    else assert(!memcmp(blobs[r]+sequence,(uint8_t[]){27,3,255,255,27,7,255,0,27,3,255,1,27,7,255,0},16));
    assert(blobs[r][sequence+4*rows]==255);
    const unsigned pose=ByteOrder_ReadLe16(blobs[r]+ByteOrder_ReadLe16(blobs[r])+54);
    const unsigned us_pose=ByteOrder_ReadLe16(blobs[0]+ByteOrder_ReadLe16(blobs[0])+54);
    assert(!memcmp(blobs[r]+pose,blobs[0]+us_pose,5+7*blobs[0][us_pose+4]));
  }
  puts("five-ROM difficulty instructions, reload tables and complete tendril pose/program verified");
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);
  Policies();Spawn();Contacts();TimersAndDragon();Guards();
  if(argc==6)Roms(argv+1);
  assert(!ActRaiser_DifficultySpawnEntry(NULL) && !ActRaiser_DifficultyContactEntry(NULL) &&
      !ActRaiser_DifficultyTimerEntry(NULL) && !ActRaiser_DifficultyDragonEntry(NULL));
  puts("regional difficulty policy and native prefixes passed");return 0;
}
