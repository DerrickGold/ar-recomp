#include "actraiser_stage_placements.h"
#include "actraiser_native_call.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_game.h"
#include "randomizer.h"
#include "regional/action/regional_terrain.h"
#include "snesrecomp/support/digest.h"
#include <string.h>

typedef struct PlacementRoot { uint16_t scene, first, wave, end; } PlacementRoot;
#include "actraiser_stage_placement_roots.inc"
enum { kRoomCount = sizeof(kRoots)/sizeof(kRoots[0]), kFirstPlacedSlot = 0x0ae0,
       kFirstWaveSlot = 0x08a0, kSlotEnd = 0x1aa0, kSlotBytes = 0x40 };
static ActionPlacementProgram s_preparation[kRoomCount];
static ActionPlacementProgram s_program;
static const PlacementRoot *s_root;
static size_t s_after_wave;
static uint8_t s_digest[32];
extern RecompReturn bank_00_9557_M0X0(CpuState *cpu);

void ActRaiserStagePlacements_Reset(void) {
  s_root = NULL;
  s_program.count = s_after_wave = 0;
  memset(s_digest,0,sizeof(s_digest));
}
bool ActRaiserStagePlacements_Fingerprint(const uint8_t previous[32],uint8_t out[32],bool *native) {
  if (!previous || !out || !native) return false;
  if (!s_root) { memmove(out,previous,32);*native=true;return true; }
  uint8_t bytes[80]="ARLIVEPLACE-R1";
  memcpy(bytes+16,previous,32);memcpy(bytes+48,s_digest,32);
  if (!sr_support_sha256(bytes,sizeof(bytes),out)) return false;
  *native=false;return true;
}
bool ActRaiserStagePlacements_Prepare(uint16_t scene,
    const ArRegionalPlacementPolicy *policy, bool action_mode,
    ArRegionalDifficulty difficulty, uint8_t terrain_profile) {
  if (!policy || terrain_profile > 2 || (unsigned)difficulty >= kArRegionalDifficulty_Count ||
      (unsigned)policy->enemies >= kArRegionalSource_Count ||
      (unsigned)policy->pickups >= kArRegionalSource_Count) return false;
  RandomizerPlacementMap maps[kRoomCount];
  size_t selected = kRoomCount;
  for (size_t i = 0; i < kRoomCount; ++i)
    if (kRoots[i].scene == scene) selected = i;
  if (selected == kRoomCount) return false;
  if (policy->enemies == kArRegionalSource_US && policy->pickups == kArRegionalSource_US) {
    ActRaiserStagePlacements_Reset();
    return true;
  }
  for (size_t i = 0; i < kRoomCount; ++i) {
    if (!ArRegionalPlacements_Copy(policy,kRoots[i].scene,action_mode,difficulty,&s_preparation[i]))
      return false;
    maps[i] = (RandomizerPlacementMap){kRoots[i].scene,&s_preparation[i]};
  }
  if (!Randomizer_ApplyPlacementPrograms(maps,kRoomCount,NULL)) return false;
  ActionPlacementProgram *next = &s_preparation[selected];
  /* Canonical value bytes, not C padding or ROM addresses. Hash once per room,
   * after randomization; a pending edit cannot change a captured later wave. */
  uint8_t bytes[4+kActionPlacementCapacity*10],digest[32];
  bytes[0]=(uint8_t)scene;bytes[1]=(uint8_t)(scene>>8);
  bytes[2]=(uint8_t)next->count;bytes[3]=(uint8_t)(next->count>>8);
  size_t used=4,after_wave=0;
  for (size_t i=0;i<next->count;++i) {
    ActionPlacement *row=&next->rows[i];
    if (row->kind==kActionPlacement_Wave) {
      after_wave=i+1;
      if (scene==0x0101) row->retry_y=ArRegionalTerrain_FillmoreCheckpointY(terrain_profile);
    }
    bytes[used++]=(uint8_t)row->id;bytes[used++]=(uint8_t)(row->id>>8);
    bytes[used++]=row->kind;bytes[used++]=row->x;bytes[used++]=row->y;
    bytes[used++]=row->parameter;bytes[used++]=row->type;
    bytes[used++]=row->retry_x;bytes[used++]=row->retry_y;bytes[used++]=row->reserve;
  }
  if (!sr_support_sha256(bytes,used,digest)) return false;
  s_program = *next;
  s_root = &kRoots[selected];
  s_after_wave = after_wave;
  memcpy(s_digest,digest,sizeof(s_digest));
  return true;
}
static bool Slot(unsigned x) {
  return x >= kFirstWaveSlot && x < kSlotEnd && (x-kFirstWaveSlot)%kSlotBytes == 0;
}
static bool Context(CpuState *cpu) {
  return s_root && cpu && !cpu->emulation && !cpu->PB && cpu->DB == 0x0a &&
      !cpu->D && !cpu->m_flag && !cpu->x_flag && !cpu->_flag_D && !(cpu->P & CPU_P_D) &&
      cpu_read16(cpu,0,kActRaiserWram_MapGroup) == s_root->scene &&
      cpu_read8(cpu,0x0a,s_root->end) == 0xff &&
      (!s_root->wave || cpu_read8(cpu,0x0a,s_root->wave-5) == 0xfe);
}
bool ActRaiser_StagePlacementsEntry(CpuState *cpu) {
  if (!Context(cpu) || cpu->Y != s_root->first) return false;
  /* $932E/$9354 initializes one player on ordinary loads, but a statue and
   * descending light when $FC.low != 0 and $0341.word != 7. After $930B's
   * eight reserved slots, $941C therefore starts at $0AE0 or $0B20. Retain
   * both native entry paths without accepting a partly consumed batch. */
  const bool materializing = cpu_read8(cpu,0,0x00fc) != 0 && cpu_read16(cpu,0,0x0341) != 7;
  return cpu->X == kFirstPlacedSlot + (materializing ? kSlotBytes : 0);
}
static RecompReturn Tail(unsigned target,unsigned source) {
  if (!cpu_hle_tailcall_request(target,source)) ActRaiserHleFatal("Placement loader lost its native return owner");
  return RECOMP_RETURN_TAILCALL;
}
static RecompReturn Object(CpuState *cpu,const ActionPlacement *row,uint16_t caller) {
  const uint16_t x = cpu->X, y = cpu->Y;
  cpu_write16(cpu,0,x+0x34,(uint16_t)(row->x*16));
  cpu_write16(cpu,0,x+0x36,(uint16_t)(row->y*16));
  cpu_write16(cpu,0,x+0x38,row->parameter);
  cpu->A = row->type;
  cpu->P = (uint8_t)((cpu->P & ~(CPU_P_N|CPU_P_Z)) | (row->type ? 0 : CPU_P_Z));
  cpu_p_to_mirrors(cpu);
  const RecompReturn result = ActRaiserNativeCall(cpu,bank_00_9557_M0X0,0,caller,false);
  if (result == RECOMP_RETURN_NORMAL &&
      (cpu->X != x || cpu->Y != y || cpu->DB != 0x0a || cpu->m_flag || cpu->x_flag))
    ActRaiserHleFatal("Placement initializer violated its register contract");
  return result;
}
static bool InitialBatchFits(unsigned first_slot) {
  const unsigned available = (kSlotEnd-first_slot)/kSlotBytes;
  unsigned needed = 1; /* Native $946E writes the terminating sentinel. */
  for (size_t i = 0; i < s_program.count; ++i) {
    const ActionPlacement *row = &s_program.rows[i];
    switch (row->kind) {
      case kActionPlacement_End: return needed <= available;
      case kActionPlacement_Wave: return needed+1 <= available;
      case kActionPlacement_Object: ++needed;break;
      case kActionPlacement_Reserve: needed += row->reserve;break;
      default: return false;
    }
  }
  return false;
}
RecompReturn ActRaiser_StagePlacements(CpuState *cpu) {
  if (!ActRaiser_StagePlacementsEntry(cpu)) ActRaiserHleFatal("Invalid regional placement entry");
  /* Structural validation allows the ordinary-entry maximum. Statue/light
   * entry consumes one more slot; preflight the entire initial batch before
   * writing anything, including reservations, a wave gate and the sentinel.
   * Keep this out of the entry predicate: insufficient capacity must not
   * silently fall back to a different region's placements. */
  if (!InitialBatchFits(cpu->X))
    ActRaiserHleFatal("Regional initial placements have insufficient free actor slots");
  /* No settings or randomization is reread while installing this captured
   * generation. */
  for (size_t i = 0; i < s_program.count; ++i) {
    const ActionPlacement *row = &s_program.rows[i];
    if (!Slot(cpu->X)) ActRaiserHleFatal("Regional placement pool overflow");
    if (row->kind == kActionPlacement_End) {
      cpu->Y = s_root->end;
      cpu->P = (uint8_t)((cpu->P & ~CPU_P_V) | CPU_P_C);cpu_p_to_mirrors(cpu);
      return Tail(0x00946e,0x00941c);
    }
    if (row->kind == kActionPlacement_Wave) {
      const uint16_t x = cpu->X;
      if (!s_root->wave || cpu_read8(cpu,0x0a,s_root->wave-5) != 0xfe)
        ActRaiserHleFatal("Regional wave has no native cursor identity");
      cpu_write16(cpu,0,x+0x12,0xa813);cpu_write16(cpu,0,x,0x0800);
      cpu_write16(cpu,0,x+0x24,0);cpu_write16(cpu,0,x+0x26,0);cpu_write16(cpu,0,x+0x30,0);
      cpu_write16(cpu,0,x+0x34,(uint16_t)(row->x*16));cpu_write16(cpu,0,x+0x36,(uint16_t)(row->y*16));
      cpu_write16(cpu,0,x+2,(uint16_t)(row->retry_x*16));
      cpu_write16(cpu,0,x+4,(uint16_t)(row->retry_y*16));
      cpu_write16(cpu,0,x+0x38,s_root->wave);
      cpu->X += kSlotBytes;cpu->Y = s_root->wave;
      cpu->P &= ~(CPU_P_C|CPU_P_V);cpu_p_to_mirrors(cpu);
      return Tail(0x00946e,0x00941c);
    }
    if (row->kind == kActionPlacement_Reserve) {
      for (unsigned j = 0; j < row->reserve; ++j) {
        cpu_write16(cpu,0,cpu->X,0x4000);cpu->X += kSlotBytes;
      }
    } else {
      const RecompReturn result = Object(cpu,row,0x9461);
      if (result != RECOMP_RETURN_NORMAL) return result;
      cpu->X += kSlotBytes;
    }
  }
  ActRaiserHleFatal("Regional placement program has no terminator");
}
bool ActRaiser_StageWaveEntry(CpuState *cpu) {
  if (!Context(cpu) || !s_after_wave || cpu->X != kFirstWaveSlot || cpu->Y != s_root->wave ||
      cpu->S > 0x1ffbu) return false;
  /* PHX in the original prologue retains the real gate owner on the stack. */
  const unsigned owner = cpu_read16(cpu,0,cpu->S+1);
  return Slot(owner) && cpu_read16(cpu,0,owner+0x12) == 0xa82d &&
      cpu_read16(cpu,0,owner+0x38) == s_root->wave &&
      (cpu_read16(cpu,0,owner) & 0x0800);
}
RecompReturn ActRaiser_StageWave(CpuState *cpu) {
  if (!ActRaiser_StageWaveEntry(cpu)) ActRaiserHleFatal("Invalid regional later-wave entry");
  /* Preflight the live retained controllers/player before writing any actor.
   * The terminating sentinel also needs a free slot. */
  uint16_t available[kActRaiserActionObjectCount];unsigned count = 0;
  for (unsigned x = kFirstWaveSlot; x < kSlotEnd; x += kSlotBytes)
    if (!(cpu_read16(cpu,0,x)&0x0800) && !(cpu_read16(cpu,0,x+0x30)&1)) available[count++] = (uint16_t)x;
  const size_t needed = s_program.count-s_after_wave;
  if (needed > count) ActRaiserHleFatal("Regional wave has insufficient free actor slots");
  for (size_t i = s_after_wave; i < s_program.count; ++i) {
    const ActionPlacement *row = &s_program.rows[i];cpu->X = available[i-s_after_wave];
    if (row->kind == kActionPlacement_End) {
      cpu->Y = s_root->end;
      return Tail(0x00954d,0x009500); /* original sentinel / PLX / PLB / PLP / RTS */
    }
    const RecompReturn result = Object(cpu,row,0x9540);
    if (result != RECOMP_RETURN_NORMAL) return result;
  }
  ActRaiserHleFatal("Regional later wave has no terminator");
}
