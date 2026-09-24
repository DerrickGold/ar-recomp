#include "actraiser/actraiser_stage_terrain.h"
#include "actraiser/actraiser_stage_placements.h"
#include "action/action_room_terrain.h"
#include "regional/regional_terrain.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t ram[65536], bank[65536], profile, presented;
static unsigned target, origin, publishes;
uint8_t ActRaiserRegional_TerrainSnapshot(void) { return profile; }
bool ActRaiserRegional_PlacementSnapshot(ArRegionalPlacementPolicy *policy,ArRegionalDifficulty *difficulty) {
  *policy=(ArRegionalPlacementPolicy){1,2};*difficulty=kArRegionalDifficulty_Expert;return true;
}
bool ActRaiserStagePlacements_Prepare(uint16_t scene,const ArRegionalPlacementPolicy *policy,
    bool mode,ArRegionalDifficulty difficulty,uint8_t terrain) {
  assert(scene==ByteOrder_ReadLe16(ram+0x18) && mode==(ram[0x349]!=0));
  assert(policy->enemies==1 && policy->pickups==2 && difficulty==kArRegionalDifficulty_Expert);
  assert(terrain==profile);return true;
}
uint8_t ActRaiserRegional_MosaicSnapshot(void) { return 2; }
void ActRaiserActionBg_BeginRoomVariants(uint8_t p,uint8_t mosaic) { assert(mosaic==2);presented=p;++publishes; }
int cpu_hle_tailcall_request(uint32_t pc,uint32_t site) { target=pc;origin=site;return 1; }
uint8 cpu_read8(CpuState *cpu,uint8 b,uint16 at) {
  (void)cpu;assert(b==0 || b==0x7e || b==0x0a);
  return b==0x0a && at>=0x8000?bank[at]:ram[at];
}
uint16 cpu_read16(CpuState *cpu,uint8 b,uint16 at) {
  return cpu_read8(cpu,b,at)|(uint16)cpu_read8(cpu,b,at+1)<<8;
}
void cpu_write8(CpuState *cpu,uint8 b,uint16 at,uint8 value) {
  (void)cpu;assert(!b || b==0x7e);ram[at]=value;
}
void cpu_write16(CpuState *cpu,uint8 b,uint16 at,uint16 value) {
  cpu_write8(cpu,b,at,value);cpu_write8(cpu,b,at+1,value>>8);
}
static void Anchors(void) {
  for(unsigned flags=0;flags<256;++flags) {
    if(flags&(CPU_P_M|CPU_P_X|CPU_P_D))continue;
    for(profile=0;profile<3;++profile) {
      memset(ram,0x5a,sizeof(ram));memset(bank,0,sizeof(bank));
      ByteOrder_WriteLe16(ram+0x18,0x0101);ByteOrder_WriteLe16(ram+0x8d4,80);
      bank[0xb200]=5;bank[0xb201]=34;bank[0xb202]=0;
      CpuState cpu={.DB=0x0a,.Y=0xb201,.X=0x8a0,.S=0x1efe,.P=flags,.A=80};
      cpu_p_to_mirrors(&cpu);CpuState before=cpu;
      uint8_t expected[65536];memcpy(expected,ram,sizeof(ram));
      assert(ActRaiser_TerrainStartEntry(&cpu)==(profile==1));
      if(profile==1) {
        assert(ActRaiser_TerrainStart(&cpu)==RECOMP_RETURN_TAILCALL);
        assert(target==0x9343 && origin==0x933c);
        before.A=27;before.Y++;before.P&=~(CPU_P_N|CPU_P_Z);cpu_p_to_mirrors(&before);
      }
      assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(ram,expected,sizeof(ram)));
      cpu.Y=0xb200;before=cpu;
      const uint8_t record[]={0xfe,155,0,156,23};memcpy(bank+0xb200,record,5);
      ByteOrder_WriteLe16(ram+0x8a2,156*16);memcpy(expected,ram,sizeof(ram));
      assert(ActRaiser_TerrainCheckpointEntry(&cpu)==(profile==1));
      if(profile==1) {
        assert(ActRaiser_TerrainCheckpoint(&cpu)==RECOMP_RETURN_TAILCALL);
        assert(target==0x94b7 && origin==0x94b1);
        before.A=25;before.P&=~(CPU_P_N|CPU_P_Z);cpu_p_to_mirrors(&before);
      }
      assert(!memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(ram,expected,sizeof(ram)));
    }
  }
  profile=1;
  for(unsigned bad=0;bad<13;++bad) {
    CpuState c={.DB=0x0a,.X=0x8a0,.Y=0xb200};
    ByteOrder_WriteLe16(ram+0x18,0x0101);ByteOrder_WriteLe16(ram+0x8a2,156*16);
    const uint8_t r[]={0xfe,155,0,156,23};memcpy(bank+0xb200,r,5);
    switch(bad) {
      case 0:c.DB=0;break;case 1:c.PB=2;break;case 2:c.D=2;break;
      case 3:c.emulation=1;break;case 4:c.m_flag=1;break;case 5:c.x_flag=1;break;
      case 6:c.P|=CPU_P_D;break;case 7:c._flag_D=1;break;case 8:++c.X;break;
      case 9:c.Y=0xfffe;break;case 10:++ram[0x18];break;
      case 11:++ram[0x8a2];break;case 12:++bank[0xb204];break;
    }
    assert(!ActRaiser_TerrainCheckpointEntry(&c));
  }
  assert(!ActRaiser_TerrainStartEntry(NULL) && !ActRaiser_StageTerrainEntry(NULL));
  assert(!ActRaiser_TerrainCheckpointEntry(NULL));
  uint8_t out=0xff;assert(!ArRegionalTerrain_Resolve(-1,&out) && out==0xff);
  assert(!ArRegionalTerrain_Resolve(3,&out) && !ArRegionalTerrain_Resolve(0,NULL));
  unsigned rooms=0;
  for(unsigned area=0;area<9;++area)for(unsigned room=0;room<10;++room)
    rooms+=ArRegionalTerrain_HasScene((uint16_t)(area|room<<8));
  assert(rooms==49);
  ActionRoomScene empty={0};assert(!ActionRoomTerrain_Project(&empty,0));
  assert(!ArRegionalTerrain_Project(0,0x0101,NULL));
}
static void Stage(const ActionRoomSceneBg *b) {
  memcpy(ram+0x8000,b->map,b->map_size);
  for(unsigned i=0;i<2048;++i)ram[0x2100+(i^1)]=b->metatiles[i];
  ByteOrder_WriteLe16(ram+0x2e,b->pages_wide<<8);
  ByteOrder_WriteLe16(ram+0x30,b->pages_high<<8);
}
static void CheckWords(const ActionRoomScene *actual,const ActionRoomScene *reference) {
  const ActionRoomSceneBg *a=&actual->bg[0],*b=&reference->bg[0];
  assert(a->pages_wide==b->pages_wide && a->pages_high==b->pages_high && a->map_size==b->map_size);
  for(size_t i=0;i<a->map_size;++i)
    assert(!memcmp(a->metatiles+a->map[i]*8,b->metatiles+b->map[i]*8,8));
}
static void RomCheck(char **paths) {
  uint8_t *rom[5];
  for(unsigned r=0;r<5;++r) {
    FILE *f=fopen(paths[r],"rb");assert(f);rom[r]=malloc(0x100000);assert(rom[r]);
    assert(fread(rom[r],1,0x100000,f)==0x100000 && fgetc(f)==EOF);fclose(f);
  }
  unsigned rooms=0;
  for(unsigned area=1;area<=7;++area)for(unsigned room=1;room<=8;++room) {
    const uint16_t key=(uint16_t)(area|room<<8);
    if(!ArRegionalTerrain_HasScene(key))continue;
    ++rooms;ActionRoomScene us,refs[3],p;
    assert(ActionRoomScene_Load(&us,rom[0],0x100000,area,room));
    for(unsigned r=0;r<3;++r)assert(ActionRoomScene_Load(&refs[r],rom[r],0x100000,area,room));
    for(unsigned r=3;r<5;++r) {
      assert(ActionRoomScene_Load(&p,rom[r],0x100000,area,room));
      assert(!memcmp(&refs[2].bg[0],&p.bg[0],sizeof(p.bg[0])));
    }
    for(unsigned old_map=0;old_map<3;++old_map)for(unsigned old_defs=0;old_defs<3;++old_defs)
    for(profile=0;profile<3;++profile) {
      p=us;assert(ActionRoomTerrain_Project(&p,old_map));
      ActionRoomScene definitions=us;assert(ActionRoomTerrain_Project(&definitions,old_defs));
      memcpy(p.bg[0].metatiles,definitions.bg[0].metatiles,2048);
      memset(ram,0x5a,sizeof(ram));Stage(&p.bg[0]);ByteOrder_WriteLe16(ram+0x18,key);
      ByteOrder_WriteLe16(ram+0x32c,1);ByteOrder_WriteLe16(ram+0x32e,156*16);
      ByteOrder_WriteLe16(ram+0x330,ArRegionalTerrain_FillmoreCheckpointY(old_map)*16);
      CpuState cpu={.A=0xdead,.X=0x1234,.Y=0x4567,.S=0x1efc,.P=CPU_P_M|CPU_P_C|CPU_P_N};
      cpu_p_to_mirrors(&cpu);CpuState expected_cpu=cpu;
      uint8_t expected[65536];memcpy(expected,ram,sizeof(ram));
      assert(ActionRoomTerrain_Project(&p,profile));CheckWords(&p,&refs[profile]);
      memcpy(expected+0x8000,p.bg[0].map,p.bg[0].map_size);
      for(unsigned i=0;i<2048;++i)expected[0x2100+(i^1)]=p.bg[0].metatiles[i];
      if(key==0x0101)ByteOrder_WriteLe16(expected+0x330,ArRegionalTerrain_FillmoreCheckpointY(profile)*16);
      assert(ActRaiser_StageTerrainEntry(&cpu));
      const unsigned count=publishes;assert(ActRaiser_StageTerrain(&cpu)==RECOMP_RETURN_TAILCALL);
      assert(publishes==count+1 && presented==profile && target==0x832c && origin==0x8329);
      expected_cpu.X=0;expected_cpu.P=(expected_cpu.P&~(CPU_P_N|CPU_P_Z))|CPU_P_Z;cpu_p_to_mirrors(&expected_cpu);
      assert(!memcmp(&cpu,&expected_cpu,sizeof(cpu)) && !memcmp(ram,expected,sizeof(ram)));
      ActionRoomScene again=p;assert(ActionRoomTerrain_Project(&p,profile));assert(!memcmp(&p,&again,sizeof(p)));
      assert(ActionRoomTerrain_Project(&p,0));assert(!memcmp(&p,&us,sizeof(p)));
      /* Unknown maps/definitions/dimensions are rejected without any writes. */
      for(unsigned corrupt=0;corrupt<4;++corrupt) {
        p=us;
        if(corrupt==0)p.bg[0].map[0]^=1;
        if(corrupt==1)p.bg[0].metatiles[0]^=1;
        if(corrupt==2)++p.bg[0].pages_wide;
        if(corrupt==3)p.group=0;
        again=p;assert(!ActionRoomTerrain_Project(&p,profile));assert(!memcmp(&p,&again,sizeof(p)));
      }
    }
  }
  assert(rooms==49);for(unsigned r=0;r<5;++r)free(rom[r]);
}
int main(int argc,char **argv) {
  Anchors();if(argc==6)RomCheck(argv+1);else assert(argc==1);
  puts("Regional terrain: native anchors and optional five-ROM reversible scene/CPU parity passed");
  return 0;
}
