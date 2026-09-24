#include "actraiser_action_motion.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_game.h"
#include "actraiser_cpu_hle_internal.h"

extern RecompReturn bank_00_8E2F_M0X0(CpuState *cpu);
extern RecompReturn bank_00_8E2F_M1X0(CpuState *cpu);
static bool s_delegate;
typedef struct MotionRow { uint16_t duration,dx,dy,next_row; bool skip_rows,replace_dy; } MotionRow;

static bool ActorShape(const CpuState *cpu) {
  if(!cpu || cpu->PB || cpu->DB || cpu->D || cpu->x_flag ||
      cpu->emulation || cpu->_flag_D || (cpu->P&CPU_P_D))return false;
  const unsigned x=cpu->X;
  return x>=kActRaiserWram_ActionObjectTable &&
      x<kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride &&
      !((x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride);
}
static int16_t SignedExtent(uint8_t value) {
  return value<128?value:(int16_t)((int)value-256);
}
enum { kExpandedProgramMarker=0x100 };
typedef struct ProgramRow {
  uint16_t row,native_row,visual_offset,duration;
  uint8_t northwall_expansion;
  bool end,replace_duration;
} ProgramRow;
static bool HeadProgram(CpuState *cpu,ProgramRow *out) {
  const unsigned x=cpu->X;
  const uint16_t stored_row=cpu_read16(cpu,0,x+0x1c);
  const uint16_t snapshot=ActRaiserRegional_ActionMotionSnapshot();
  const bool active=stored_row>=kExpandedProgramMarker && stored_row<=kExpandedProgramMarker+4;
  if(!active && !(snapshot&(1u<<kArRegionalActionMotion_HeadWithdrawal)))return false;
  if(x==cpu_read16(cpu,0,0x8a) || cpu_read16(cpu,0,0x18)!=0x0605 ||
      cpu_read16(cpu,0,x+0x32)!=0xe3a1 || cpu_read16(cpu,0,x+0x1a)!=35 ||
      cpu_read16(cpu,0,x+0x16)!=0x4000 || cpu_read8(cpu,0,x+0x18)!=0x7e ||
      cpu_read16(cpu,0,x+0x3c) || cpu_read16(cpu,0,cpu->S+1)==0x969d)return false;
  const unsigned sequence=cpu_read16(cpu,0x7e,0x4048),row=active?stored_row-kExpandedProgramMarker:stored_row;
  const unsigned end=cpu_read16(cpu,0x7e,0x4000);
  unsigned native_row;uint8_t pose;
  if(!ArRegionalActionMotion_HeadRow(active?(1u<<kArRegionalActionMotion_HeadWithdrawal):snapshot,row,&native_row,&pose) ||
      sequence<0x4a || sequence+13>end || end>0x1000)return false;
  static const uint8_t expected[]={40,7,0,0,38,3,0,0,37,3,0,0,255};
  for(unsigned i=0;i<sizeof(expected);++i)if(cpu_read8(cpu,0x7e,0x4000+sequence+i)!=expected[i])return false;
  /* Pose39 is retained in US data. Require its measured four-piece header;
   * incompatible future donor bundles must provide their own mapping. */
  const unsigned composition=cpu_read16(cpu,0x7e,0x4000+end+2*39);
  if(composition<end+80 || composition+33>0x1000 ||
      cpu_read16(cpu,0x7e,0x4000+composition)!=0x0808 ||
      cpu_read16(cpu,0x7e,0x4002+composition)!=0x1510 ||
      cpu_read8(cpu,0x7e,0x4004+composition)!=4)return false;
  if(out)*out=(ProgramRow){.row=(uint16_t)row,.native_row=(uint16_t)native_row,
      .visual_offset=(uint16_t)(row==1?1:0),.end=pose==255};
  return true;
}
static bool PlantProgram(CpuState *cpu,ProgramRow *out) {
  const unsigned x=cpu->X;
  const uint16_t stored_row=cpu_read16(cpu,0,x+0x1c);
  const bool active=stored_row>=kExpandedProgramMarker && stored_row<=kExpandedProgramMarker+4;
  const uint64_t policy=active?UINT64_C(2)<<(2*kArRegionalBoss_PlantOpen):ActRaiserRegional_BossSnapshot();
  if(ArRegionalBoss_Value(policy,kArRegionalBoss_PlantOpen)!=16 || cpu_read16(cpu,0,0x18)!=0x0305 ||
      cpu_read16(cpu,0,x+0x32)!=0xd974 || cpu_read16(cpu,0,x+0x1a)!=2 ||
      cpu_read16(cpu,0,x+0x16)!=0x5000 || cpu_read8(cpu,0,x+0x18)!=0x7e ||
      cpu_read16(cpu,0,x+0x3a) || cpu_read16(cpu,0,x+0x3c) || cpu_read16(cpu,0,cpu->S+1)==0x969d)return false;
  const unsigned row=active?stored_row-kExpandedProgramMarker:stored_row;
  unsigned native_row;uint8_t visual;
  if(!ArRegionalBoss_PlantOpenRow(policy,row,&native_row,&visual))return false;
  const unsigned sequence=cpu_read16(cpu,0x7e,0x5006),end=cpu_read16(cpu,0x7e,0x5000);
  if(sequence<8 || sequence+9>end || end>0x1000)return false;
  static const uint8_t expected[]={3,3,0,0,4,3,0,0,255};
  for(unsigned i=0;i<sizeof(expected);++i)if(cpu_read8(cpu,0x7e,0x5000+sequence+i)!=expected[i])return false;
  const unsigned composition=cpu_read16(cpu,0x7e,0x5000+end+4);
  if(composition<end+6 || composition+19>0x1000 ||
      cpu_read16(cpu,0x7e,0x5000+composition)!=0x0808 ||
      cpu_read16(cpu,0x7e,0x5002+composition)!=0x1010 || cpu_read8(cpu,0x7e,0x5004+composition)!=2)return false;
  if(out)*out=(ProgramRow){.row=(uint16_t)row,.native_row=(uint16_t)native_row,
      .visual_offset=(uint16_t)(row==0?0xffff:0),.end=visual==255};
  return true;
}
static bool TendrilShape(CpuState *cpu) {
  if(!ActorShape(cpu) || cpu_read16(cpu,0,0x18)!=0x0305 ||
      cpu_read16(cpu,0,cpu->X+0x32)!=0xd974 || cpu_read16(cpu,0,cpu->X+0x16)!=0x5000 ||
      cpu_read8(cpu,0,cpu->X+0x18)!=0x7e || !cpu_read16(cpu,0,cpu->X+0x3a) ||
      cpu_read16(cpu,0,cpu->X+0x3c) || cpu_read16(cpu,0,cpu->S+1)==0x969d)return false;
  const unsigned sequence=cpu_read16(cpu,0x7e,0x5022),end=cpu_read16(cpu,0x7e,0x5000);
  if(sequence<0x32 || sequence+17>end || end>0x1000)return false;
  static const uint8_t expected[]={27,3,255,255,27,7,255,0,27,3,255,1,27,7,255,0,255};
  for(unsigned i=0;i<sizeof(expected);++i)if(cpu_read8(cpu,0x7e,0x5000+sequence+i)!=expected[i])return false;
  return true;
}
bool ActRaiser_PlantTendrilEntry(CpuState *cpu) {
  return ActRaiserRegional_DifficultySnapshot().single_tendril_bob && TendrilShape(cpu) && !cpu->m_flag;
}
RecompReturn ActRaiser_PlantTendril(CpuState *cpu) {
  if(!ActRaiser_PlantTendrilEntry(cpu))ActRaiserHleFatal("Unsupported Beginner tendril sequence");
  /* One 48-update software program, rather than two 24-update repeats. The
   * real DAE0 JSR owns its repeat count and native coroutine return word. */
  cpu->A=0x1001;ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(0x00dae0,0x00dadd))ActRaiserHleFatal("Tendril sequence has no native owner");
  return RECOMP_RETURN_TAILCALL;
}
static bool TendrilProgram(CpuState *cpu,ProgramRow *out) {
  const uint16_t stored=cpu_read16(cpu,0,cpu->X+0x1c);
  const bool active=stored>=kExpandedProgramMarker && stored<=kExpandedProgramMarker+6;
  if((!active && !ActRaiserRegional_DifficultySnapshot().single_tendril_bob) ||
      cpu_read16(cpu,0,cpu->X+0x1a)!=16 || !TendrilShape(cpu))return false;
  const unsigned row=active?stored-kExpandedProgramMarker:stored;
  static const uint8_t native_rows[]={0,1,1,2,3,3,4},delays[]={3,3,15,3,3,15,0};
  if(row>=sizeof(native_rows))return false;
  if(out)*out=(ProgramRow){.row=(uint16_t)row,.native_row=native_rows[row],
      .duration=delays[row],.replace_duration=row<6,.end=row==6};
  return true;
}
/* Room0406 owns a 1549-byte boss blob at5000..560C. Its remaining animation
 * page is not a general scratch allocator: only this validated US profile
 * may use the final four64-byte slots, before graphics/raster RAM at6000.
 * Fixed per-pose addresses let simultaneous impacts retain independent poses.
 * Native OAM, collision, widescreen and 3D all continue reading native +20. */
enum { kNorthwallExpansionBase=0x5f00, kNorthwallExpansionStride=64 };
static bool NorthwallProgram(CpuState *cpu,ProgramRow *out) {
  const unsigned x=cpu->X,state=cpu_read16(cpu,0,x+0x1a),stored=cpu_read16(cpu,0,x+0x1c);
  if((state!=1 && state!=2) || cpu_read16(cpu,0,0x18)!=0x0406 ||
      cpu_read16(cpu,0,x+0x32)!=0xe7c6 || cpu_read16(cpu,0,x+0x16)!=0x5000 ||
      cpu_read8(cpu,0,x+0x18)!=0x7e || cpu_read16(cpu,0,x+0x3c) ||
      cpu_read16(cpu,0,cpu->S+1)==0x969d)return false;
  const bool active=stored>=kExpandedProgramMarker && stored<=kExpandedProgramMarker+(state==1?10:7);
  const unsigned row=active?stored-kExpandedProgramMarker:stored;
  const uint64_t policy=active?UINT64_C(2)<<(2*(state==1?kArRegionalBoss_NorthwallImpact:kArRegionalBoss_NorthwallThrow)):
      ActRaiserRegional_BossSnapshot();
  unsigned native_row,expansion;uint16_t duration;bool end;
  if(!ArRegionalBoss_NorthwallRow(policy,state,row,&native_row,&duration,&expansion,&end))return false;
  if(cpu_read16(cpu,0x7e,0x5000)!=0xc6 || cpu_read16(cpu,0x7e,0x5004)!=0x1f ||
      cpu_read16(cpu,0x7e,0x5006)!=0x38)return false;
  unsigned next=0xf0;
  for(unsigned pose=0;pose<21;++pose) {
    if(cpu_read16(cpu,0x7e,0x50c6+2*pose)!=next || next+5>0x60d)return false;
    next+=5+7*cpu_read8(cpu,0x7e,0x5004+next);
  }
  if(next!=0x60d)return false;
  static const uint8_t impact[]={3,5,0,0,4,5,0,0,5,5,0,0,6,7,0,0,7,7,0,0,8,7,0,0,255};
  static const uint8_t throwing[]={20,5,0,0,10,7,0,0,11,7,0,0,12,17,0,0,13,3,0,0,14,3,0,0,15,1,0,0,255};
  const uint8_t *expected=state==1?impact:throwing;
  const unsigned length=state==1?sizeof(impact):sizeof(throwing),at=state==1?0x501f:0x5038;
  for(unsigned i=0;i<length;++i)if(cpu_read8(cpu,0x7e,at+i)!=expected[i])return false;
  /* The four added poses rearrange only these retained two CHR references. */
  static const uint8_t seed[]={16,16,8,0,4,0,0,24,0,0,62,2,0,8,16,0,0,63,2,
      0,16,8,0,0,62,2,0,24,0,0,0,63,2};
  if(cpu_read16(cpu,0x7e,0x50d6)!=0x16c)return false;
  for(unsigned i=0;i<sizeof(seed);++i)if(cpu_read8(cpu,0x7e,0x516c+i)!=seed[i])return false;
  if(out)*out=(ProgramRow){.row=(uint16_t)row,.native_row=(uint16_t)native_row,
      .duration=duration,.northwall_expansion=(uint8_t)expansion,.replace_duration=!end,.end=end};
  return true;
}
static void NorthwallExpand(CpuState *cpu,unsigned object,unsigned step) {
  uint8_t composition[61];
  for(unsigned i=0;i<33;++i)composition[i]=cpu_read8(cpu,0x7e,0x516c+i);
  for(unsigned n=1;n<=step;++n) {
    const bool right=n==2;
    for(unsigned part=0;part<composition[4];++part)composition[5+7*part+(right?2:1)]+=8;
    const unsigned at=5+7*composition[4],width=(composition[4]+1)*8;
    composition[at]=0;composition[at+1]=right?width-8:0;composition[at+2]=right?0:width-8;
    composition[at+3]=composition[at+4]=0;
    composition[at+5]=(n==1 || n==4)?63:62;composition[at+6]=2;
    ++composition[4];composition[0]=composition[1]=(uint8_t)(width/2);
  }
  const unsigned pointer=kNorthwallExpansionBase+(step-1)*kNorthwallExpansionStride;
  for(unsigned i=0;i<5+7u*composition[4];++i)cpu_write8(cpu,0x7e,pointer+i,composition[i]);
  cpu_write16(cpu,0,object+0x20,(uint16_t)pointer);
  cpu_write16(cpu,0,object+0x0a,composition[0]);cpu_write16(cpu,0,object+0x0e,composition[1]);
  /* Top/bottom and flips are unchanged from the native seed pose. +22 keeps
   * its valid US visual8 alias; drawing consumes the full +20 pointer. */
}
static bool AnimationProgram(CpuState *cpu,ProgramRow *out) {
  return ActorShape(cpu) && (HeadProgram(cpu,out) || PlantProgram(cpu,out) || TendrilProgram(cpu,out) || NorthwallProgram(cpu,out));
}
static bool Initializing(CpuState *cpu) {
  /* Real 969B JSR return word. During this call +32 may still be a previous
   * occupant; the dedicated 969E boundary uses the selected descriptor Y.
   * Initial motion is subsequently cleared by the common initializer. */
  return cpu_read16(cpu,0,(uint16_t)(cpu->S+1))==0x969d;
}
static bool PosePlan(CpuState *cpu,uint16_t *offset) {
  const uint8_t snapshot=ActRaiserRegional_PoseSnapshot();
  if(!snapshot || !ActorShape(cpu) || cpu_read16(cpu,0,0x18)!=0x0104)return false;
  const unsigned x=cpu->X;
  if(x==cpu_read16(cpu,0,0x8a) || cpu_read16(cpu,0,x+0x16)!=0x4000 ||
      cpu_read8(cpu,0,x+0x18)!=0x7e || cpu_read16(cpu,0,x+0x3c))return false;
  const unsigned source=Initializing(cpu)?cpu->Y:cpu_read16(cpu,0,x+0x32);
  const unsigned state=cpu_read16(cpu,0,x+0x1a),row=cpu_read16(cpu,0,x+0x1c);
  if(row>=4 || !((source==0xce39 && state==10) || (source==0xce48 && state==45)))return false;
  const unsigned table=cpu_read16(cpu,0x7e,0x4000),sequence=cpu_read16(cpu,0x7e,0x4002+2*state);
  if(sequence<2+2*(state+1) || sequence+17>table || table+16>0x1000)return false;
  static const uint8_t shared[]={1,5,0,0,5,19,0,0,7,5,0,0,3,19,0,0,255};
  static const uint8_t humanoid[]={0,7,0,0,4,11,0,0,6,3,0,0,2,11,0,0,255};
  const uint8_t *expected=state==10?shared:humanoid;
  for(unsigned i=0;i<17;++i)if(cpu_read8(cpu,0x7e,0x4000+sequence+i)!=expected[i])return false;
  uint8_t visual;
  if(!ArRegionalPoses_Visual(snapshot,state,row,expected[row*4],&visual) || visual==expected[row*4])return false;
  /* Do not import foreign collision headers with a draw-only pose change.
   * Both swapped US compositions must have the same measured extents. */
  const unsigned old=cpu_read16(cpu,0x7e,0x4000+table+expected[row*4]*2);
  const unsigned next=cpu_read16(cpu,0x7e,0x4000+table+visual*2);
  if(old<table+16 || next<table+16 || old+5>0x1000 || next+5>0x1000)return false;
  for(unsigned i=0;i<4;++i)if(cpu_read8(cpu,0x7e,0x4000+old+i)!=16 ||
      cpu_read8(cpu,0x7e,0x4000+next+i)!=16)return false;
  if(offset)*offset=(uint16_t)((int)visual-expected[row*4]);
  return true;
}
static bool CollisionPlan(CpuState *cpu,bool birth,ArRegionalCollisionExtents *out) {
  const uint8_t collision=ActRaiserRegional_CollisionSnapshot();
  if(!collision || !ActorShape(cpu) || (!birth && Initializing(cpu)))return false;
  const unsigned x=cpu->X,area=cpu_read8(cpu,0,kActRaiserWram_MapGroup);
  if(x==cpu_read16(cpu,0,0x8a) || cpu_read16(cpu,0,x+0x16)!=0x4000 ||
      cpu_read8(cpu,0,x+0x18)!=0x7e)return false;
  const unsigned source=birth?cpu->Y:cpu_read16(cpu,0,x+0x32),state=cpu_read16(cpu,0,x+0x1a);
  unsigned family;
  if(area==3 && (source==0xc961 || source==0xc9be || (source==0xc863 && state==10)))
    family=kArRegionalCollision_Kasandora;
  else if(area==5 && (source==0xdfe5 || source==0xdff3) && state==41)
    family=kArRegionalCollision_Arrow;
  else return false;
  if(!(collision&(1u<<family)) || state>41)return false;
  const unsigned table=cpu_read16(cpu,0x7e,0x4000),row=cpu_read16(cpu,0,x+0x1c);
  const unsigned sequence=cpu_read16(cpu,0x7e,0x4002+2*state),at=sequence+4*row;
  if(sequence<2+2*(state+1) || at+4>table || table>0x1000)return false;
  const unsigned raw=cpu_read8(cpu,0x7e,0x4000+at);
  if(raw==255)return false;
  const unsigned visual=(raw+cpu_read16(cpu,0,x+0x3c))&255;
  if(table+2*visual+2>0x1000)return false;
  const unsigned comp=cpu_read16(cpu,0x7e,0x4000+table+2*visual);
  if(comp<table+2*visual+2 || comp+4>0x1000)return false;
  const ArRegionalCollisionExtents native={
    SignedExtent(cpu_read8(cpu,0x7e,0x4000+comp)),SignedExtent(cpu_read8(cpu,0x7e,0x4001+comp)),
    SignedExtent(cpu_read8(cpu,0x7e,0x4002+comp)),SignedExtent(cpu_read8(cpu,0x7e,0x4003+comp))};
  ArRegionalCollisionExtents next;
  if(!ArRegionalCollision_Pose(collision,family,visual,&native,&next))return false;
  const unsigned flip=cpu_read16(cpu,0,x+0x28);
  if(flip&kActRaiserObjectFlip_Horizontal){const int16_t t=next.left;next.left=next.right;next.right=t;}
  if(flip&0x8000){const int16_t t=next.top;next.top=next.bottom;next.bottom=t;}
  if(out)*out=next;
  return true;
}
static void ApplyExtents(CpuState *cpu,unsigned object,const ArRegionalCollisionExtents *extents) {
  cpu_write16(cpu,0,object+0x0a,(uint16_t)extents->left);
  cpu_write16(cpu,0,object+0x0e,(uint16_t)extents->right);
  cpu_write16(cpu,0,object+0x0c,(uint16_t)extents->top);
  cpu_write16(cpu,0,object+0x10,(uint16_t)extents->bottom);
}
bool ActRaiser_ActionCollisionBirthEntry(CpuState *cpu) {
  return cpu && !cpu->m_flag && CollisionPlan(cpu,true,NULL);
}
RecompReturn ActRaiser_ActionCollisionBirth(CpuState *cpu) {
  ArRegionalCollisionExtents extents;
  if(!cpu || cpu->m_flag || !CollisionPlan(cpu,true,&extents))ActRaiserHleFatal("Unsupported regional first-pose collision");
  /* Common initializer's 8E2F has just restored descriptor Y. +32 is not
   * assigned until 95B9, and may still identify a previous slot occupant. */
  ApplyExtents(cpu,cpu->X,&extents);
  cpu_write16(cpu,0,cpu->X+6,0); /* Original STZ at 969E, no flag changes. */
  if(!cpu_hle_tailcall_request(0x0096a1,0x00969e))ActRaiserHleFatal("First pose has no initializer continuation");
  return RECOMP_RETURN_TAILCALL;
}
static bool Plan(CpuState *cpu,MotionRow *out) {
  const uint16_t snapshot=ActRaiserRegional_ActionMotionSnapshot();
  const uint8_t emitters=ActRaiserRegional_EmitterSnapshot();
  const uint64_t bosses=ActRaiserRegional_BossSnapshot();
  const uint8_t fire=ActRaiserRegional_FireSnapshot();
  if((!snapshot && (emitters&3)!=2 && !bosses && !(fire&1)) || !ActorShape(cpu) || Initializing(cpu))return false;
  const unsigned x=cpu->X;
  const unsigned area=cpu_read8(cpu,0,kActRaiserWram_MapGroup);
  const uint16_t source=cpu_read16(cpu,0,x+0x32);
  const unsigned room=cpu_read8(cpu,0,kActRaiserWram_MapGroup+1);
  if(area==5 && room==3 && source==0xd974 && cpu_read16(cpu,0,x+0x16)==0x5000 &&
      cpu_read8(cpu,0,x+0x18)==0x7e && cpu_read16(cpu,0,x+0x3a)) {
    const unsigned state=cpu_read16(cpu,0,x+0x1a),index=cpu_read16(cpu,0,x+0x1c);
    if((state!=4 && state!=6) || index)return false;
    const unsigned sequence=cpu_read16(cpu,0x7e,0x5002+2*state),end=cpu_read16(cpu,0x7e,0x5000);
    if(sequence<2+2*(state+1) || sequence+9>end || end>0x1000)return false;
    const uint8_t expected[]={state==6?6:5,7,0,0,state==6?8:7,0,0,0,255};
    for(unsigned i=0;i<sizeof(expected);++i)if(cpu_read8(cpu,0x7e,0x5000+sequence+i)!=expected[i])return false;
    uint16_t duration=7;if(!ArRegionalBoss_PlantWindup(bosses,state,index,expected[0],&duration,0,0))return false;
    if(out)*out=(MotionRow){.duration=duration};
    return true;
  }
  const bool pharaoh=(area==3 && room==6 && source==0xc1a2) || (area==7 && room==4 && source==0xf6fa);
  if(pharaoh && cpu_read16(cpu,0,x+0x16)==0x5000 && cpu_read8(cpu,0,x+0x18)==0x7e) {
    const unsigned state=cpu_read16(cpu,0,x+0x1a),row=cpu_read16(cpu,0,x+0x1c);
    unsigned next;if(!ArRegionalBoss_PharaohSkip(bosses,area==7,state,row,&next))return false;
    const unsigned sequence=cpu_read16(cpu,0x7e,0x5018),end=cpu_read16(cpu,0x7e,0x5000);
    if(sequence<26 || sequence+45>end || end>0x1000)return false;
    /* Both encounters share the ten-row bounce. Only their final stationary
     * row differs. Let the native reader consume its real FF terminator. */
    static const uint8_t bounce[]={15,3,0,253,15,1,0,254,15,0,0,255,15,1,0,0,
      15,0,0,1,15,1,0,2,15,3,0,3,15,2,0,255,15,1,0,0,15,2,0,1};
    for(unsigned i=0;i<sizeof(bounce);++i)if(cpu_read8(cpu,0x7e,0x5000+sequence+i)!=bounce[i])return false;
    if(cpu_read8(cpu,0x7e,0x5028+sequence)!=15 ||
        cpu_read8(cpu,0x7e,0x5029+sequence)!=(area==7?31:15) ||
        cpu_read16(cpu,0x7e,0x502a+sequence) || cpu_read8(cpu,0x7e,0x502c+sequence)!=255)return false;
    if(out)*out=(MotionRow){.next_row=(uint16_t)next,.skip_rows=true};
    return true;
  }
  const bool viper=(area==5 && room==8 && source==0xe483) || (area==7 && room==6 && source==0xf72a);
  const bool floor=area==5 && room==8 && (source==0xe5cf || source==0xe606);
  if((viper || floor) && cpu_read16(cpu,0,x+0x16)==0x5000 && cpu_read8(cpu,0,x+0x18)==0x7e) {
    const unsigned state=cpu_read16(cpu,0,x+0x1a),row=cpu_read16(cpu,0,x+0x1c);
    if(viper?!(state>=4 && state<=6 && !row):!(state==(source==0xe5cf?9u:8u) && row>=6 && row<=9))return false;
    const unsigned sequence=cpu_read16(cpu,0x7e,0x5002+2*state),offset=sequence+4*row,end=cpu_read16(cpu,0x7e,0x5000);
    if(sequence<2+2*(state+1) || offset+4>end || end>0x1000)return false;
    const uint8_t visual=cpu_read8(cpu,0x7e,0x5000+offset);
    uint16_t duration=cpu_read8(cpu,0x7e,0x5001+offset);
    int16_t dx=SignedExtent(cpu_read8(cpu,0x7e,0x5002+offset)),dy=SignedExtent(cpu_read8(cpu,0x7e,0x5003+offset));
    if(!ArRegionalBoss_ViperRow(bosses,area==7,state,row,visual,&duration,dx,&dy))return false;
    const unsigned flip=cpu_read16(cpu,0,x+0x28);
    if(flip&0x4000)dx=-dx;
    if(flip&0x8000)dy=-dy;
    if(out)*out=(MotionRow){.duration=duration,.dx=(uint16_t)dx,.dy=(uint16_t)dy,.replace_dy=true};
    return true;
  }
  if(area==4 && cpu_read8(cpu,0,kActRaiserWram_MapGroup+1)==3 && source==0xd646 &&
      cpu_read16(cpu,0,x+0x16)==0x5000 && cpu_read8(cpu,0,x+0x18)==0x7e) {
    const unsigned state=cpu_read16(cpu,0,x+0x1a),row=cpu_read16(cpu,0,x+0x1c);
    if((state!=1 && state!=2) || row)return false;
    const unsigned sequence=cpu_read16(cpu,0x7e,0x5002+2*state),end=cpu_read16(cpu,0x7e,0x5000);
    if(sequence<2+2*(state+1) || sequence+5>end || end>0x1000 ||
        cpu_read8(cpu,0x7e,0x5004+sequence)!=255)return false;
    const uint8_t visual=cpu_read8(cpu,0x7e,0x5000+sequence);
    uint16_t duration=cpu_read8(cpu,0x7e,0x5001+sequence);
    int16_t dx=SignedExtent(cpu_read8(cpu,0x7e,0x5002+sequence));
    const int16_t dy=SignedExtent(cpu_read8(cpu,0x7e,0x5003+sequence));
    if(!ArRegionalBoss_DragonRow(bosses,state,row,visual,&duration,dx,dy))return false;
    if(cpu_read16(cpu,0,x+0x28)&0x4000)dx=-dx;
    if(out)*out=(MotionRow){.duration=duration,.dx=(uint16_t)dx};
    return true;
  }
  if(area==3 && source==0xc3a5 && cpu_read16(cpu,0,x+0x16)==0x4000 && cpu_read8(cpu,0,x+0x18)==0x7e) {
    const unsigned state=cpu_read16(cpu,0,x+0x1a),row=cpu_read16(cpu,0,x+0x1c);
    if((state!=13 && state!=14) || row>=8)return false;
    const unsigned sequence=cpu_read16(cpu,0x7e,0x4002+2*state),offset=sequence+4*row;
    const unsigned end=cpu_read16(cpu,0x7e,0x4000);
    if(sequence<2+2*(state+1) || offset+4>end || end>0x1000)return false;
    const uint8_t visual=cpu_read8(cpu,0x7e,0x4000+offset),duration=cpu_read8(cpu,0x7e,0x4001+offset);
    int16_t dx=SignedExtent(cpu_read8(cpu,0x7e,0x4002+offset)),dy=SignedExtent(cpu_read8(cpu,0x7e,0x4003+offset));
    if(!ArRegionalFire_CurveRow(fire,state,row,visual,duration,&dx,&dy))return false;
    const unsigned flip=cpu_read16(cpu,0,x+0x28);
    if(flip&0x4000)dx=-dx;
    if(flip&0x8000)dy=-dy;
    if(out)*out=(MotionRow){.duration=duration,.dx=(uint16_t)dx,.dy=(uint16_t)dy,.replace_dy=true};
    return true;
  }
  if(area==7 && cpu_read8(cpu,0,kActRaiserWram_MapGroup+1)==8 && source==0xf80f &&
      cpu_read16(cpu,0,x+0x16)==0x5000 && cpu_read8(cpu,0,x+0x18)==0x7e) {
    const unsigned state=cpu_read16(cpu,0,x+0x1a),row=cpu_read16(cpu,0,x+0x1c);
    if(!((state==10 && row==7) || (state==48 && !row) || (state==22 && !row)))return false;
    const unsigned sequence=cpu_read16(cpu,0x7e,0x5002+2*state),offset=sequence+4*row;
    const unsigned end=cpu_read16(cpu,0x7e,0x5000);
    const unsigned length=state==22?16:4;
    if(sequence<2+2*(state+1) || offset+length>end || end>0x1000)return false;
    static const uint8_t closing[]={11,31,0,0},upper[]={2,10,252,0};
    static const uint8_t minion[]={31,3,0,0,33,3,0,0,35,3,0,0,37,3,0,0};
    const uint8_t *expected=state==10?closing:state==48?upper:minion;
    for(unsigned i=0;i<length;++i)if(cpu_read8(cpu,0x7e,0x5000+offset+i)!=expected[i])return false;
    if(state==22) {
      unsigned next;if(!ArRegionalBoss_TanzraMinionSkip(bosses,state,row,&next))return false;
      if(out)*out=(MotionRow){.next_row=(uint16_t)next,.skip_rows=true};
    } else {
      uint16_t duration=expected[1];int16_t dx=state==48?-4:0;
      if(!ArRegionalBoss_TanzraRow(bosses,state,row,&duration,dx,0) || duration==expected[1])return false;
      if(cpu_read16(cpu,0,x+0x28)&kActRaiserObjectFlip_Horizontal)dx=-dx;
      if(out)*out=(MotionRow){.duration=duration,.dx=(uint16_t)dx};
    }
    return true;
  }
  if(area==7 && source==0xf760 && cpu_read16(cpu,0,x+0x16)==0x5000 && cpu_read8(cpu,0,x+0x18)==0x7e) {
    const unsigned state=cpu_read16(cpu,0,x+0x1a),row=cpu_read16(cpu,0,x+0x1c);
    unsigned next;
    if(!ArRegionalBoss_IceSkip(bosses,state,row,&next))return false;
    const unsigned sequence=cpu_read16(cpu,0x7e,0x5002+2*state),offset=sequence+4*row;
    const unsigned end=cpu_read16(cpu,0x7e,0x5000);
    if(sequence<2+2*(state+1) || offset+12>end || end>0x1000)return false;
    /* Body removes rows5/6; head removes rows6/7. Their remaining stationary
     * poses differ. Match the actual three-row windows before moving a cursor,
     * so an incompatible donor sequence is never silently reinterpreted. */
    static const uint8_t body[]={5,5,0,0,9,5,0,0,13,5,0,0};
    static const uint8_t head[]={8,5,0,0,11,5,0,0,4,5,0,255};
    const uint8_t *expected=state==17?body:head;
    for(unsigned i=0;i<12;++i)if(cpu_read8(cpu,0x7e,0x5000+offset+i)!=expected[i])return false;
    if(out)*out=(MotionRow){.next_row=(uint16_t)next,.skip_rows=true};
    return true;
  }
  const bool emitter=area==1 && source==0xb3bf;
  const bool minotaur=area==1 && source==0xaf5d; /* Original, NOT the rematch. */
  ArRegionalActionMotionFamily family;
  if(area==1 && source==0xaa9a)family=kArRegionalActionMotion_Bird;
  else if(area==1 && source==0xac8e)family=kArRegionalActionMotion_Leaper;
  else if(area==1 && source==0xb041)family=kArRegionalActionMotion_Cave;
  else if(area==1 && source==0xb0b4)family=kArRegionalActionMotion_CaveAttacker;
  else if(area==5 && source==0xdcdb)family=kArRegionalActionMotion_Caster;
  else if(area==2 && source==0xbba8)family=kArRegionalActionMotion_Swordsman;
  else if(area==5 && (source==0xdfe5 || source==0xdff3))family=kArRegionalActionMotion_Arrow;
  else if(emitter || minotaur)family=kArRegionalActionMotionFamily_Count;
  else return false;
  const unsigned base=minotaur?0x5000:0x4000;
  if(cpu_read16(cpu,0,x+0x16)!=base || cpu_read8(cpu,0,x+0x18)!=0x7e)return false;
  const unsigned state=cpu_read16(cpu,0,x+0x1a),row=cpu_read16(cpu,0,x+0x1c);
  if(state>41 || row>10)return false;
  const unsigned offset=cpu_read16(cpu,0x7e,base+2+state*2)+4*row;
  const unsigned end=cpu_read16(cpu,0x7e,base);
  if(offset<2+2*(state+1) || offset+4>end || end>0x1000)return false;
  if(cpu_read8(cpu,0x7e,base+offset)==0xff)return false;
  uint16_t duration=cpu_read8(cpu,0x7e,base+1+offset);
  const uint8_t raw_dx=cpu_read8(cpu,0x7e,base+2+offset);
  int16_t dx=raw_dx<128?raw_dx:(int16_t)((int)raw_dx-256);
  const uint16_t native_duration=duration;const int16_t native_dx=dx;
  const uint8_t raw_dy=cpu_read8(cpu,0x7e,base+3+offset);
  if(emitter && (cpu_read8(cpu,0x7e,base+offset)!=37 || raw_dy))return false;
  const bool planned=minotaur?ArRegionalBoss_MinoRow(bosses,state,row,&duration,dx,raw_dy):
      emitter?ArRegionalEmitter_Row(emitters,state,row,&duration,dx):
      ArRegionalActionMotion_Row(snapshot,family,state,row,&duration,&dx);
  if(!planned ||
      (duration==native_duration && dx==native_dx))return false;
  if(cpu_read16(cpu,0,x+0x28)&kActRaiserObjectFlip_Horizontal)dx=-dx;
  if(out)*out=(MotionRow){.duration=duration,.dx=(uint16_t)dx};
  return true;
}
bool ActRaiser_ActionMotionEntry(CpuState *cpu) {
  if(s_delegate){s_delegate=false;return false;}
  return Plan(cpu,NULL) || CollisionPlan(cpu,false,NULL) || AnimationProgram(cpu,NULL) || PosePlan(cpu,NULL);
}
RecompReturn ActRaiser_ActionMotion(CpuState *cpu) {
  MotionRow row={0};ArRegionalCollisionExtents extents;
  const bool motion=Plan(cpu,&row),collision=CollisionPlan(cpu,false,&extents);
  ProgramRow program={0};const bool expanded=AnimationProgram(cpu,&program);
  uint16_t pose_offset=0;const bool pose=PosePlan(cpu,&pose_offset);
  if(!motion && !collision && !expanded && !pose)ActRaiserHleFatal("Unsupported regional animation-row entry");
  const uint16_t object=cpu->X;
  const uint16_t old_row=(row.skip_rows || expanded)?cpu_read16(cpu,0,object+0x1c):0;
  if(row.skip_rows)cpu_write16(cpu,0,object+0x1c,row.next_row);
  if(pose)cpu_write16(cpu,0,object+0x3c,pose_offset);
  if(expanded) {
    /* Borrow only this actor's row and visual-offset fields during the
     * non-yielding reader. All velocities, extents, poses and flags remain
     * native outputs, except for the explicitly owned Northwall expansion. */
    cpu_write16(cpu,0,object+0x1c,program.native_row);
    cpu_write16(cpu,0,object+0x3c,program.visual_offset);
  }
  s_delegate=true;
  const RecompReturn result=cpu->m_flag?bank_00_8E2F_M1X0(cpu):bank_00_8E2F_M0X0(cpu);
  s_delegate=false;
  if(expanded || pose)cpu_write16(cpu,0,object+0x3c,0);
  if(result!=RECOMP_RETURN_NORMAL) {
    /* Audited reader never yields; preserve an unexpected escape token and
     * do not leave our speculative cursor edit behind. */
    if(row.skip_rows || expanded)cpu_write16(cpu,0,object+0x1c,old_row);
    return result;
  }
  if(expanded) {
    /* 8637 increments this logical cursor after return. A marker in its
     * unused high byte keeps an active sequence valid across debug restores
     * without a host cache. The native terminator/state initializer clears
     * it. No other routine in this source family consumes the row index. */
    if(!program.end)cpu_write16(cpu,0,object+0x1c,kExpandedProgramMarker+program.row);
    if(program.replace_duration)cpu_write16(cpu,0,object+0x24,program.duration);
    if(program.northwall_expansion)NorthwallExpand(cpu,object,program.northwall_expansion);
    return result;
  }
  if(row.skip_rows)return result; /* Native reader acquired the selected row. */
  /* 8E2F has no yield in the audited body. Native code has acquired one row,
   * mirrored motion, resolved composition/extents and consumed its RTS frame.
   * Update only the two numerical outputs; no fabricated CPU or stack state. */
  if(motion) {
    cpu_write16(cpu,0,object+0x24,row.duration);
    cpu_write16(cpu,0,object+0x06,row.dx);
    if(row.replace_dy)cpu_write16(cpu,0,object+0x08,row.dy);
  }
  if(collision) {
    /* Contact and victim tests consume these four words. Attack-part geometry
     * belongs to the attacker, not the victim's sprite composition. These
     * owners are enemies/projectiles, never the player or weapon objects. */
    ApplyExtents(cpu,object,&extents);
  }
  return result;
}

bool ActRaiser_WallHeadPauseEntry(CpuState *cpu) {
  const uint16_t snapshot=ActRaiserRegional_ActionMotionSnapshot();
  const unsigned short_bit=1u<<kArRegionalActionMotion_WallHeadShortHold;
  const unsigned long_bit=1u<<kArRegionalActionMotion_WallHeadLongHold;
  if(!(snapshot&(short_bit|long_bit)) || !ActorShape(cpu) || cpu->m_flag ||
      cpu_read8(cpu,0,kActRaiserWram_MapGroup)!=3 ||
      cpu_read8(cpu,0,cpu->X+0x18)!=0x7e || (cpu_read16(cpu,0,cpu->X+0x30)&0x400))return false;
  const uint16_t source=cpu_read16(cpu,0,cpu->X+0x32),base=cpu_read16(cpu,0,cpu->X+0x16);
  return (base==0x4000 && (source==0xc8e5 || source==0xc8f3) && (snapshot&short_bit)) ||
      (base==0x5000 && (source==0xc8c9 || source==0xc8d7) && (snapshot&long_bit));
}

RecompReturn ActRaiser_WallHeadPause(CpuState *cpu) {
  /* C908 is reached only after the native activation gate. JP has no LDA30 /
   * delay call here. Preserve all CPU, object and return state and let C90E
   * own LDA2, animation, allocation, recovery and the eventual RTS/yields. */
  if(!ActRaiser_WallHeadPauseEntry(cpu) || !cpu_hle_tailcall_request(0x00c90e,0x00c908))
    ActRaiserHleFatal("Cannot skip ordinary wall-head pause at its native boundary");
  return RECOMP_RETURN_TAILCALL;
}

static bool EmitterShape(CpuState *cpu) {
  return ActorShape(cpu) && !cpu->m_flag && cpu_read8(cpu,0,kActRaiserWram_MapGroup)==1 &&
      cpu_read16(cpu,0,cpu->X+0x32)==0xb3bf && cpu_read16(cpu,0,cpu->X+0x16)==0x4000 &&
      cpu_read8(cpu,0,cpu->X+0x18)==0x7e;
}
bool ActRaiser_EmitterPositionEntry(CpuState *cpu) {
  return (ActRaiserRegional_EmitterSnapshot()&4) && EmitterShape(cpu);
}
RecompReturn ActRaiser_EmitterPosition(CpuState *cpu) {
  if(!ActRaiser_EmitterPositionEntry(cpu))ActRaiserHleFatal("Unsupported emitter position entry");
  const uint16_t before=cpu_read16(cpu,0,cpu->X+2);
  const bool right=before>=0x380;
  cpu->A=(uint16_t)(right?before-22:before+6);
  /* PAL SEC/SBC22 on the right, CLC/ADC6 on the left. The branch range
   * excludes left overflow/carry and right borrow; retain exact SBC overflow. */
  cpu->_flag_C=right;cpu->_flag_V=right && ((before^22)&(before^cpu->A)&0x8000)!=0;
  cpu->P=(uint8_t)((cpu->P&~(CPU_P_C|CPU_P_V))|(cpu->_flag_C?CPU_P_C:0)|(cpu->_flag_V?CPU_P_V:0));
  ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);cpu_write16(cpu,0,cpu->X+2,cpu->A);
  if(!cpu_hle_tailcall_request(0x00b3d5,0x00b3cb))ActRaiserHleFatal("Emitter position has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_EmitterCadenceEntry(CpuState *cpu) {
  return (ActRaiserRegional_EmitterSnapshot()&3) && EmitterShape(cpu);
}
RecompReturn ActRaiser_EmitterCadence(CpuState *cpu) {
  if(!ActRaiser_EmitterCadenceEntry(cpu))ActRaiserHleFatal("Unsupported emitter cadence entry");
  /* One cycle for JP/PAL instead of US's two. The native repeated-animation
   * helper still owns every yield and spawn continuation. PAL rows share a
   * single stationary pose and resolve to 127+128 updates through the reader. */
  cpu->A=0x2401;ActRaiserCpuHle_SetNegativeZero16(cpu,cpu->A);
  if(!cpu_hle_tailcall_request(0x00b3e7,0x00b3e4))ActRaiserHleFatal("Emitter cadence has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
