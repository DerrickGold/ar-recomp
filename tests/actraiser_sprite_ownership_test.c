#include "actraiser/actraiser_ar2_master.h"
#include "actraiser/actraiser_sprite_ownership.h"
#include "actraiser_game.h"
#include "snesrecomp/game/cpu.h"
#include <stdio.h>
#include <string.h>

uint8 g_ram[kActRaiserWramSize];
static unsigned failures, builds, uploads, ar2_uploads;
static RecompReturn native_result = RECOMP_RETURN_NORMAL;
static CpuState native_input;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                      \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)
extern bool ActRaiser_SimSpriteBuildEntry(CpuState *);
extern bool ActRaiser_SpriteUploadEntry(CpuState *);
extern RecompReturn ActRaiser_SimSpriteBuild(CpuState *);
extern RecompReturn ActRaiser_SpriteUpload(CpuState *);

static void Expect(uint8_t group, uint8_t map, ActRaiserSpriteRole role, unsigned first,
                   unsigned count) {
  const ActRaiserSpriteOwnership owner = ActRaiserSpriteOwnership_Presented(group, map);
  uint8_t actual_first = 255, actual_count = 255;
  CHECK(ActRaiserSpriteOwnership_Range(&owner, role, &actual_first, &actual_count) == (count != 0));
  CHECK(actual_first == first && actual_count == count);
}

RecompReturn bank_01_ACD9_M0X0(CpuState *cpu) {
  ++builds;
  CHECK(!ActRaiser_SimSpriteBuildEntry(cpu));
  CHECK(!memcmp(cpu, &native_input, sizeof(*cpu)));
  ActRaiserSpriteOwnership_RecordSim(0x083e, 0x25, 24, 40);
  cpu->S += 3;
  return native_result;
}
RecompReturn bank_01_ACD9_M1X0(CpuState *cpu) { return bank_01_ACD9_M0X0(cpu); }
/* The AR2 master streams its tiles in the same vblank as a completed upload. */
void ActRaiserAr2Master_UploadVideo(CpuState *cpu) {
  CHECK(cpu != NULL);
  ++ar2_uploads;
}
RecompReturn bank_02_ACA3_M1X0(CpuState *cpu) {
  ++uploads;
  CHECK(!ActRaiser_SpriteUploadEntry(cpu));
  CHECK(!memcmp(cpu, &native_input, sizeof(*cpu)));
  Expect(0, 7, kActRaiserSprite_HudIcon, 0, 0); /* Not published early. */
  cpu->S += 2;
  return native_result;
}
static void TestNativeWrappers(void) {
  for (unsigned m = 0; m < 2; ++m) {
    ActRaiserSpriteOwnership_Reset();
    g_ram[0x18] = 0;
    g_ram[0x19] = 7;
    CpuState cpu = {.PB = 1, .DB = 0x7f, .m_flag = m, .S = 0x1e0};
    native_input = cpu;
    CHECK(ActRaiser_SimSpriteBuildEntry(&cpu));
    CHECK(ActRaiser_SimSpriteBuild(&cpu) == RECOMP_RETURN_NORMAL);
    CHECK(cpu.S == 0x1e3 && builds == m + 1);
    Expect(0, 7, kActRaiserSprite_HudIcon, 0, 0);
    cpu = (CpuState){.PB = 2, .m_flag = 1, .S = 0x1e0};
    native_input = cpu;
    CHECK(ActRaiser_SpriteUploadEntry(&cpu));
    CHECK(ActRaiser_SpriteUpload(&cpu) == RECOMP_RETURN_NORMAL);
    CHECK(cpu.S == 0x1e2 && uploads == m + 1 && ar2_uploads == m + 1);
    Expect(0, 7, kActRaiserSprite_HudIcon, 6, 4);
  }
  CHECK(!ActRaiser_SpriteUploadEntry(NULL));
  CpuState cpu = {.PB = 2, .DB = 0x7f, .m_flag = 1};
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.DB = 0;
  cpu.D = 0x100;
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.D = 0;
  cpu.x_flag = 1;
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.x_flag = 0;
  cpu.emulation = 1;
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.emulation = 0;
  cpu.m_flag = 0;
  CHECK(!ActRaiser_SpriteUploadEntry(&cpu));
  cpu.PB = 1;
  cpu.emulation = 1;
  CHECK(!ActRaiser_SimSpriteBuildEntry(&cpu));
  cpu.emulation = 0;
  cpu.S = 0x1e0;
  native_input = cpu;
  native_result = RECOMP_RETURN_TAILCALL;
  CHECK(ActRaiser_SimSpriteBuild(&cpu) == native_result);
  Expect(0, 7, kActRaiserSprite_HudIcon, 0, 0);
  cpu = (CpuState){.PB = 2, .m_flag = 1, .S = 0x1e0};
  native_input = cpu;
  CHECK(ActRaiser_SpriteUpload(&cpu) == native_result);
  CHECK(ar2_uploads == 2); /* an unfinished upload streams nothing */
  native_result = RECOMP_RETURN_NORMAL;
}

static void TestPublication(void) {
  uint8_t shadow[kActRaiserSpriteShadowBytes];
  memset(shadow, 0x5a, sizeof(shadow)); /* No recognizable sprite art. */
  ActRaiserSpriteOwnership_Reset();
  ActRaiserSpriteOwnership_Begin(0, 1, 0);
  ActRaiserSpriteOwnership_RecordSim(0x083e, 2, 44, 60);
  ActRaiserSpriteOwnership_Complete(shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 0, 0);
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 11, 4);
  /* Pause: repeated upload retains ownership. Beginning another frame alone
   * cannot change what is already visible. */
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  ActRaiserSpriteOwnership_Begin(0, 1, 0);
  ActRaiserSpriteOwnership_RecordSim(0x083e, 4, 0, 12);
  Expect(0, 1, kActRaiserSprite_HudIcon, 11, 4);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 0, 3);
  Expect(0, 7, kActRaiserSprite_HudIcon, 0, 0);
  shadow[543] ^= 1; /* Unobserved writer, including high OAM. */
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 0, 0);
  ActRaiserSpriteOwnership_Begin(0, 1, 0);
  ActRaiserSpriteOwnership_RecordSim(0x0a00, 2, 0, 16);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(0, 1, shadow);
  Expect(0, 1, kActRaiserSprite_HudIcon, 0, 0); /* World record is not HUD. */
  ActRaiserSpriteOwnership_Begin(0, 8, 1);
  ActRaiserSpriteOwnership_RecordSim(0x083e, 3, 4, 8);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(0, 8, shadow);
  Expect(0, 8, kActRaiserSprite_HudIcon, 1, 1);
  ActRaiserSpriteOwnership_Begin(0, 9, 3);
  ActRaiserSpriteOwnership_RecordSim(0x06a0, 0x33, 0, 28);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(0, 9, shadow);
  ActRaiserSpriteOwnership_Begin(0, 9, 4);
  ActRaiserSpriteOwnership owner = ActRaiserSpriteOwnership_Presented(0, 9);
  CHECK(owner.valid && owner.location == 3);
  ActRaiserSpriteOwnership_Complete(shadow); /* All-hidden animation. */
  ActRaiserSpriteOwnership_Upload(0, 9, shadow);
  owner = ActRaiserSpriteOwnership_Presented(0, 9);
  CHECK(owner.valid && owner.location == 4);
  Expect(0, 9, kActRaiserSprite_WorldLabel, 0, 0);
}

static void TestArtIndependentRoles(void) {
  uint8_t shadow[kActRaiserSpriteShadowBytes] = {0};
  const uint16_t eyes[] = {0xf3fa, 0xf408, 0xf430, 0xf458, 0xf486, 0xf494, 0xf4b2};
  for (unsigned parts = 1; parts <= 6; ++parts) {
    ActRaiserSpriteOwnership_Begin(7, 1, 0);
    for (unsigned i = 0; i < 7; ++i)
      ActRaiserSpriteOwnership_RecordAction(eyes[i], (5 + i * parts) * 4,
                                            (5 + (i + 1) * parts) * 4);
    ActRaiserSpriteOwnership_Complete(shadow);
    ActRaiserSpriteOwnership_Upload(7, 1, shadow);
    Expect(7, 1, kActRaiserSprite_StatueEyes, 5, 7 * parts);
    Expect(7, 2, kActRaiserSprite_StatueEyes, 0, 0);
  }
  for (unsigned spell = 0; spell < 4; ++spell) {
    ActRaiserSpriteOwnership_Begin(0, 7, 0);
    ActRaiserSpriteOwnership_RecordSim(0x081a, 0x25 + spell, 0, 4); /* Menu copy. */
    ActRaiserSpriteOwnership_RecordSim(0x083e, 0x25 + spell, 4, spell ? 8 : 20);
    ActRaiserSpriteOwnership_Complete(shadow);
    ActRaiserSpriteOwnership_Upload(0, 7, shadow);
    Expect(0, 7, kActRaiserSprite_HudIcon, 1, spell ? 1 : 4);
  }
  ActRaiserSpriteOwnership_Begin(1, 1, 0);
  ActRaiserSpriteOwnership_Record(kActRaiserSprite_HudIcon, 0, 4);
  ActRaiserSpriteOwnership_Record(kActRaiserSprite_HudIcon, 8, 12);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(1, 1, shadow);
  Expect(1, 1, kActRaiserSprite_HudIcon, 0, 0); /* Never capture a gap. */
  ActRaiserSpriteOwnership_Begin(1, 1, 0);
  ActRaiserSpriteOwnership_Record(kActRaiserSprite_HudIcon, 0, 516);
  ActRaiserSpriteOwnership_Complete(shadow);
  ActRaiserSpriteOwnership_Upload(1, 1, shadow);
  CHECK(!ActRaiserSpriteOwnership_Presented(1, 1).valid);
  ActRaiserSpriteOwnership_Reset();
}
static void TestActionReceivers(void) {
  uint8_t shadow[kActRaiserSpriteShadowBytes]={0};
  ActRaiserSpriteOwnership_Begin(1,2,0);
  ActRaiserSpriteOwnership_RecordActionObject(0,0x08a0,100,200,0,12);
  ActRaiserSpriteOwnership_RecordActionObject(0,0x08e0,300,400,12,20);
  ActRaiserSpriteOwnership_RecordActionObject(0,0x06a0,500,600,20,24);
  ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(1,2,shadow);
  ActRaiserSpriteOwnership owner=ActRaiserSpriteOwnership_Presented(1,2);
  CHECK(owner.valid&&owner.slots[0]==kActRaiserSprite_Player&&owner.slots[2]==kActRaiserSprite_Player);
  CHECK(owner.slots[3]==kActRaiserSprite_Enemy&&owner.slots[5]==kActRaiserSprite_Unowned);
  CHECK(owner.world_x[2]==100&&owner.world_y[3]==400);
  /* Every emitted dynamic object participates, including inactive attackers,
   * snakes, pickups and props. Combat flags no longer enter this API. */
  const unsigned end=kActRaiserWram_ActionObjectTable+kActRaiserActionObjectStride*kActRaiserActionObjectCount;
  for(unsigned record=kActRaiserWram_PlayerObject+kActRaiserActionObjectStride;record<end;record+=kActRaiserActionObjectStride)
    CHECK(ActRaiserSpriteOwnership_ActionRole(record)==kActRaiserSprite_Enemy);
  CHECK(ActRaiserSpriteOwnership_ActionRole(0x08e1)==kActRaiserSprite_Unowned);
  CHECK(ActRaiserSpriteOwnership_ActionRole(end)==kActRaiserSprite_Unowned);
  CHECK(ActRaiserSpriteOwnership_ActionRole(kActRaiserWram_MagicController)==kActRaiserSprite_Unowned);
  shadow[0]^=1;ActRaiserSpriteOwnership_Upload(1,2,shadow);CHECK(!ActRaiserSpriteOwnership_Presented(1,2).valid);
  ActRaiserSpriteOwnership_Begin(7,1,0);
  ActRaiserSpriteOwnership_RecordActionObject(0xf3fa,0x08e0,0,0,0,4);
  ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(7,1,shadow);
  CHECK(ActRaiserSpriteOwnership_Presented(7,1).slots[0]==kActRaiserSprite_StatueEyes);
  ActRaiserSpriteOwnership_Begin(1,2,0);
  ActRaiserSpriteOwnership_RecordActionObject(0,0x08a0,0,0,0,516);
  ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(1,2,shadow);
  CHECK(!ActRaiserSpriteOwnership_Presented(1,2).valid);
}
static void TestVoxelProps(void) {
  uint8_t shadow[544]={0};
  ActRaiserSpriteOwnership_Reset();
  ActRaiserSpriteOwnership_Begin(4,5,0);
  ActRaiserSpriteOwnership_RecordActionObject(0xd601,0xee0,192,212,40,52);
  ActRaiserSpriteOwnership_RecordActionVisual(0xd601,0xee0,0x4000,0x7e,0x4750,20,80,40,52);
  CHECK(!ActRaiserSpriteOwnership_Presented(4,5).valid);
  ActRaiserSpriteOwnership_Complete(shadow);
  CHECK(!ActRaiserSpriteOwnership_Presented(4,5).valid);
  ActRaiserSpriteOwnership_Upload(4,5,shadow);
  ActRaiserSpriteOwnership o=ActRaiserSpriteOwnership_Presented(4,5);
  CHECK(o.valid && o.prop_shape[10]==kActRaiserSpriteProp_RockPlatform && o.prop_owner[12]==0xee0);
  CHECK(!o.prop_owner[9] && !o.prop_owner[13]);
  CHECK(ActRaiserSpriteOwnership_PropTag(&o,0xee0)==11);
  CHECK(!ActRaiserSpriteOwnership_PropTag(&o,0));
  CHECK(!ActRaiserSpriteOwnership_PropTag(&o,0xe80));
  const ActRaiserSpriteOwnership invalid={.prop_owner={[127]=0xee0}};
  CHECK(!ActRaiserSpriteOwnership_PropTag(&invalid,0xee0));
  const ActRaiserSpriteOwnership last={.valid=true,.prop_owner={[127]=0xee0}};
  CHECK(ActRaiserSpriteOwnership_PropTag(&last,0xee0)==128);
  CHECK(!ActRaiserSpriteOwnership_Presented(4,4).valid);
  shadow[0]=1;ActRaiserSpriteOwnership_Upload(4,5,shadow);
  CHECK(!ActRaiserSpriteOwnership_Presented(4,5).valid);
  /* Type $01 bridge logs ($B4C0) are modeled wherever they are, including
   * after they begin falling with unchanged artwork. Type $00 floating logs
   * ($B499) stay flat. Ownership and world coordinates still feed priority
   * and lighting for both. */
  const unsigned log_heights[]={232,488,512};
  for(unsigned short_log=0;short_log<2;++short_log)
  for(unsigned height=0;height<sizeof(log_heights)/sizeof(*log_heights);++height) {
    const unsigned y=log_heights[height];
    const unsigned source=short_log?0xb4c0:0xb499;
    ActRaiserSpriteOwnership_Begin(2,1,0);
    ActRaiserSpriteOwnership_RecordActionObject(source,0xee0,1040,y,40,52);
    ActRaiserSpriteOwnership_RecordActionVisual(source,0xee0,0x4000,0x7e,
        short_log?0x45bc:0x45a2,20,80,40,52);
    ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(2,1,shadow);
    o=ActRaiserSpriteOwnership_Presented(2,1);
    CHECK(o.valid && ActRaiserSpriteOwnership_PropTag(&o,0xee0)==(short_log?11:0));
    for(unsigned slot=10;slot<13;++slot) {
      CHECK(o.slots[slot]==kActRaiserSprite_Enemy);
      CHECK(o.prop_owner[slot]==(short_log?0xee0:0));
      CHECK(o.prop_shape[slot]==(short_log?kActRaiserSpriteProp_Log:0));
      CHECK(o.world_x[slot]==1040 && o.world_y[slot]==(int)y);
    }
  }
  /* Companions, changed art/animation, different rooms, players and a reused
   * OAM range must never inherit a previous log's geometry. */
  for (unsigned scenario=0;scenario<7;++scenario) {
    const uint16_t record=scenario==4?0x8a0:0xf60;
    ActRaiserSpriteOwnership_Begin(2,scenario==3?2:1,0);
    ActRaiserSpriteOwnership_RecordActionObject(0xb4c0,record,1768,232,12,20);
    ActRaiserSpriteOwnership_RecordActionVisual(scenario==0?0xb499:0xb4c0,record,
        scenario==1?0x4002:0x4000,scenario==2?0:0x7e,0x45bc,20,80,12,20);
    if(scenario==5)ActRaiserSpriteOwnership_Record(kActRaiserSprite_Enemy,12,20);
    ActRaiserSpriteOwnership_Complete(shadow);
    ActRaiserSpriteOwnership_Upload(2,scenario==3?2:1,shadow);
    o=ActRaiserSpriteOwnership_Presented(2,scenario==3?2:1);
    /* Scenario 6 is the unchanged bridge log: the positive control. */
    CHECK(o.valid && o.prop_shape[3]==(scenario==6?kActRaiserSpriteProp_Log:0));
    CHECK(o.prop_owner[3]==(scenario==6?record:0));
  }
  for(unsigned scenario=0;scenario<7;++scenario) {
    const unsigned room=scenario==1?4:5;
    const uint16_t record=scenario==5?0x8a0:0xba0;
    ActRaiserSpriteOwnership_Begin(4,room,0);
    ActRaiserSpriteOwnership_RecordActionObject(0xd601,record,192,212,20,28);
    ActRaiserSpriteOwnership_RecordActionVisual(scenario==2?0xd602:0xd601,record,
        scenario==3?0x4002:0x4000,scenario==4?0:0x7e,0x4750,20,80,20,28);
    if(scenario==6)ActRaiserSpriteOwnership_RecordActionVisual(0xd601,record,0x4000,0x7e,0x4763,20,80,20,28);
    ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(4,room,shadow);
    o=ActRaiserSpriteOwnership_Presented(4,room);
    CHECK(o.valid && o.prop_shape[5]==(scenario==0?kActRaiserSpriteProp_RockPlatform:0));
    CHECK(o.prop_owner[6]==(scenario==0?record:0));
  }
  const struct { unsigned room,source,animation,composition,shape; } temple[]={
    {4,0xe42a,0x4000,0x49f4,kActRaiserSpriteProp_TempleBridge},
    {5,0xe2d1,0x4000,0x48be,kActRaiserSpriteProp_TemplePlatform},
    {7,0xe292,0x4000,0x488f,kActRaiserSpriteProp_TempleCrusher},
    {7,0xe2a6,0x4000,0x48be,kActRaiserSpriteProp_TemplePlatform},
    {7,0xe2bd,0x4000,0x48d1,kActRaiserSpriteProp_TempleSpikes},
    {8,0xe5cf,0x5000,0x5ccd,kActRaiserSpriteProp_TemplePlatform},
    {8,0xe606,0x5000,0x6022,kActRaiserSpriteProp_TempleSpikeRail},
  };
  for(unsigned i=0;i<sizeof(temple)/sizeof(*temple);++i)
  for(unsigned scenario=0;scenario<8;++scenario) {
    const unsigned group=scenario==1?4:5,room=scenario==2?6:temple[i].room;
    ActRaiserSpriteOwnership_Begin(group,room,0);
    ActRaiserSpriteOwnership_RecordActionObject(temple[i].source,0xba0,400,200,16,40);
    ActRaiserSpriteOwnership_RecordActionVisual(scenario==3?0xa686:temple[i].source,0xba0,
        temple[i].animation+(scenario==4?2:0),scenario==5?0:0x7e,
        temple[i].composition+(scenario==6?2:0),-32,80,16,40);
    if(scenario==7)ActRaiserSpriteOwnership_Record(kActRaiserSprite_Enemy,16,40);
    ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(group,room,shadow);
    o=ActRaiserSpriteOwnership_Presented(group,room);
    CHECK(o.valid && o.prop_shape[4]==(scenario==0?temple[i].shape:0));
    CHECK(o.prop_owner[9]==(scenario==0?0xba0:0));
    if(!scenario)CHECK(o.prop_origin_x[9]==-32 && o.prop_origin_y[9]==80);
  }
  ActRaiserSpriteOwnership_Begin(5,6,0);
  ActRaiserSpriteOwnership_RecordActionObject(0xe3a1,0xba0,400,200,16,32);
  ActRaiserSpriteOwnership_RecordActionVisual(0xe3a1,0xba0,0x4000,0x7e,0x48e4,0,0,16,32);
  ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(5,6,shadow);
  CHECK(!ActRaiserSpriteOwnership_Presented(5,6).prop_owner[4]);
  /* Wall flames attach by their native source family, independently of the
   * animation picture/composition and regional art selection. Nearby static
   * scenery, fire enemies and reused/player slots remain normal native OBJ. */
  for(unsigned room=3;room<=5;room+=2)
  for(unsigned scenario=0;scenario<8;++scenario) {
    const unsigned group=scenario==1?5:2,map=scenario==2?7:room;
    const uint16_t record=scenario==6?0x8a0:0xfa0;
    const uint16_t source=scenario==3?0xba12:0xba01;
    ActRaiserSpriteOwnership_Begin(group,map,0);
    ActRaiserSpriteOwnership_RecordActionObject(source,record,224,528,20,28);
    ActRaiserSpriteOwnership_RecordActionVisual(source,record,
        scenario==4?0x5000:0x4000,scenario==5?6:0x7e,0x49ff,-4,80,20,28);
    if(scenario==7)ActRaiserSpriteOwnership_Record(kActRaiserSprite_Enemy,20,28);
    ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(group,map,shadow);
    o=ActRaiserSpriteOwnership_Presented(group,map);
    CHECK(o.valid && o.prop_shape[5]==(scenario==0?kActRaiserSpriteProp_WallTorch:0));
    CHECK(o.prop_owner[6]==(scenario==0?record:0));
    if(!scenario)CHECK(o.world_x[5]==224 && o.world_y[5]==528 && o.prop_origin_x[5]==-4);
  }
}
static void TestApronWallTorches(void) {
  uint8_t shadow[544]={0};
  ActRaiserSpriteOwnership_Begin(2,3,0);
  ActRaiserSpriteOwnership_RecordActionObject(0xba01,0xfa0,80,464,508,512);
  ActRaiserSpriteOwnership_RecordActionVisual(0xba01,0xfa0,0x4000,0x7e,0, -130,80,508,512);
  ActRaiserSpriteOwnership_RecordWallTorchPart(0xba01,0xfa0,0x4000,0x7e,-130,80,
      80,464,-130,80,0x2000,16);
  ActRaiserSpriteOwnership_RecordWallTorchPart(0xba01,0xfe0,0x4000,0x7e,-160,80,
      48,464,-160,80,0x2000,16);
  /* No neighbouring scenery, unsupported part size, or player may be moved. */
  ActRaiserSpriteOwnership_RecordWallTorchPart(0xba12,0x1020,0x4000,0x7e,-160,80,
      48,464,-160,80,0x2000,16);
  ActRaiserSpriteOwnership_RecordWallTorchPart(0xba01,0x1020,0x4000,0x7e,-160,80,
      48,464,-160,80,0x2000,32);
  ActRaiserSpriteOwnership_RecordWallTorchPart(0xba01,0x8a0,0x4000,0x7e,-160,80,
      48,464,-160,80,0x2000,16);
  CHECK(!ActRaiserSpriteOwnership_Presented(2,3).wall_torch_part_count);
  ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(2,3,shadow);
  ActRaiserSpriteOwnership o=ActRaiserSpriteOwnership_Presented(2,3);
  CHECK(o.valid && o.wall_torch_part_count==2);
  CHECK(ActRaiserSpriteOwnership_PropTag(&o,0xfa0)==128);
  CHECK(ActRaiserSpriteOwnership_PropTag(&o,0xfe0)==127);
  CHECK(o.wall_torch_parts[0].owner_tag==128 && o.wall_torch_parts[1].owner_tag==127);
  CHECK(o.wall_torch_parts[1].x==-160 && o.wall_torch_parts[1].world_x==48);
  shadow[20]=1;ActRaiserSpriteOwnership_Upload(2,3,shadow);
  CHECK(!ActRaiserSpriteOwnership_Presented(2,3).valid);
  ActRaiserSpriteOwnership_Begin(2,3,0);
  for(unsigned i=0;i<kActRaiserWallTorchPartLimit+1;++i)
    ActRaiserSpriteOwnership_RecordWallTorchPart(0xba01,0x8e0+i*64,0x4000,0x7e,
        -160,80,48,464,-160,80,0x2000,16);
  ActRaiserSpriteOwnership_Complete(shadow);ActRaiserSpriteOwnership_Upload(2,3,shadow);
  o=ActRaiserSpriteOwnership_Presented(2,3);
  CHECK(o.wall_torch_part_count==kActRaiserWallTorchPartLimit);
  CHECK(!ActRaiserSpriteOwnership_PropTag(&o,0x8e0+kActRaiserWallTorchPartLimit*64));
}
int main(void) {
  TestApronWallTorches();
  TestVoxelProps();
  TestActionReceivers();
  TestNativeWrappers();
  TestPublication();
  TestArtIndependentRoles();
  return failures ? 1 : 0;
}
