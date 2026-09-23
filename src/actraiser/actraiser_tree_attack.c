#include "actraiser_tree_attack.h"
#include "actraiser_regional_runtime.h"
#include "actraiser_hle_fatal.h"
#include "actraiser_game.h"

/* These are audited bounded RTS leaves, never the 8657/8669 coroutine helpers.
 * The shared native actor dispatcher still owns movement and +24 countdowns. */
extern RecompReturn bank_00_8538_M0X0(CpuState *cpu);
extern RecompReturn bank_00_853D_M0X0(CpuState *cpu);
extern RecompReturn bank_00_85E9_M0X0(CpuState *cpu);
extern RecompReturn bank_00_871E_M0X0(CpuState *cpu);
extern RecompReturn bank_00_8E2F_M0X0(CpuState *cpu);
extern RecompReturn bank_00_8FE7_M0X0(CpuState *cpu);
extern RecompReturn bank_00_9108_M0X0(CpuState *cpu);

/* Source A9B3 has no US code beyond RTS. Its peer/seed/visual roles share that
 * verified handler, with a private tag in the otherwise unused +3E word. The
 * tag belongs to the native slot, not a host-side timer or pointer registry;
 * live children finish after cache loss and native death replaces the handler. */
enum TreePhase {
  kOpening=0x5401, kSeedBorn, kFalling, kLanded, kSprouting,
  kWalking, kWithering, kVisualBorn, kVisualPlaying,
};
static uint16_t Read(CpuState *cpu,unsigned offset) {return cpu_read16(cpu,0,cpu->X+offset);}
static void Write(CpuState *cpu,unsigned offset,uint16_t value) {cpu_write16(cpu,0,cpu->X+offset,value);}
static bool Slot(unsigned x) {
  return x>=kActRaiserWram_ActionObjectTable &&
    x<kActRaiserWram_ActionObjectTable+kActRaiserActionObjectCount*kActRaiserActionObjectStride &&
    !((x-kActRaiserWram_ActionObjectTable)%kActRaiserActionObjectStride);
}
static bool Shape(CpuState *cpu,uint16_t source) {
  return cpu && !cpu->PB && !cpu->DB && !cpu->D && !cpu->m_flag && !cpu->x_flag &&
    !cpu->emulation && !cpu->_flag_D && !(cpu->P&CPU_P_D) && Slot(cpu->X) &&
    cpu_read16(cpu,0,0x18)==0x0101 && Read(cpu,0x32)==source &&
    Read(cpu,0x16)==0x4000 && cpu_read8(cpu,0,cpu->X+0x18)==0x7e &&
    !(Read(cpu,0)&0xc000) && !Read(cpu,0x3c);
}
static bool Enabled(void) {
  return (ActRaiserRegional_ActionMotionSnapshot()&(1u<<kArRegionalActionMotion_TreeSeeds))!=0;
}
static void Leaf(CpuState *cpu,RecompReturn (*function)(CpuState *)) {
  if(!cpu_invoke_rts_leaf(cpu,function,0x00a9bf))
    ActRaiserHleFatal("Tree helper violated its synchronous RTS contract");
}
static bool Compatible(CpuState *cpu) {
  static const struct {uint8_t state,rows;uint8_t bytes[32];} programs[]={
    {7,6,{20,2,0,0,21,2,0,0,20,2,0,0,21,2,0,0,20,7,0,0,21,7,0,0}},
    {16,2,{13,3,0,2,13,3,0,2}}, {17,1,{13,5,0,0}},
    {18,4,{22,3,255,0,23,3,255,0,24,3,255,0,25,3,255,0}},
    {20,3,{25,9,0,0,24,9,0,0,26,9,0,0}},
    {22,6,{14,3,1,253,14,3,1,254,16,3,1,255,16,3,1,1,17,3,1,2,17,3,1,3}},
    {23,8,{25,5,0,254,25,5,0,255,25,5,0,0,25,5,0,1,25,5,0,2,22,3,0,255,22,5,0,0,22,3,0,1}},
    {24,6,{15,3,255,253,15,3,255,254,17,3,255,255,17,3,255,1,16,3,255,2,16,3,255,3}},
  };
  const unsigned end=cpu_read16(cpu,0x7e,0x4000);
  if(end>0x1000)return false;
  for(unsigned i=0;i<sizeof(programs)/sizeof(programs[0]);++i) {
    const unsigned at=cpu_read16(cpu,0x7e,0x4002+2*programs[i].state),length=4*programs[i].rows;
    if(at<52 || at+length>=end || cpu_read8(cpu,0x7e,0x4000+at+length)!=255)return false;
    for(unsigned j=0;j<length;++j)if(cpu_read8(cpu,0x7e,0x4000+at+j)!=programs[i].bytes[j])return false;
  }
  const unsigned at=cpu_read16(cpu,0x7e,0x4016);
  return at>=52 && at+5<=end && cpu_read8(cpu,0x7e,0x4000+at)==6 &&
    (cpu_read8(cpu,0x7e,0x4001+at)==63 || cpu_read8(cpu,0x7e,0x4001+at)==0) &&
    !cpu_read16(cpu,0x7e,0x4002+at) && cpu_read8(cpu,0x7e,0x4004+at)==255;
}
static RecompReturn Return(uint32_t target,uint32_t source) {
  if(!cpu_hle_tailcall_request(target,source))ActRaiserHleFatal("Tree controller has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_TreePrepareEntry(CpuState *cpu) {
  if(!Enabled() || !Shape(cpu,0xa934) || Read(cpu,0x3a) || !Slot(cpu->X+64))return false;
  const unsigned peer=cpu->X+64;
  return cpu_read16(cpu,0,peer+0x32)==0xa9b3 && !(cpu_read16(cpu,0,peer)&0xc000) &&
    !cpu_read16(cpu,0,peer+0x3a) && cpu_read16(cpu,0,peer+0x16)==0x4000 &&
    cpu_read8(cpu,0,peer+0x18)==0x7e && Compatible(cpu);
}
RecompReturn ActRaiser_TreePrepare(CpuState *cpu) {
  if(!ActRaiser_TreePrepareEntry(cpu))ActRaiserHleFatal("Unsupported tree preparation");
  Write(cpu,0x1a,10);Write(cpu,0x1c,0);
  Leaf(cpu,bank_00_8E2F_M0X0);
  Write(cpu,0x78,(uint16_t)(Read(cpu,0x78)+1));
  /* JP/PAL apply the pose then wait128 separately. No copied donor PC or
   * fabricated yield word: resume the real US orb continuation after129
   * native updates. Its allocation, second shot and restart remain native. */
  Write(cpu,6,0);Write(cpu,8,0);Write(cpu,0x24,128);Write(cpu,0x12,0xa97b);
  return Return(0x00a948,0x00a975);
}
static bool Row(CpuState *cpu) {
  Leaf(cpu,bank_00_8E2F_M0X0);
  if(cpu->_flag_C)return false;
  Write(cpu,0x1c,(uint16_t)(Read(cpu,0x1c)+1));return true;
}
static void Start(CpuState *cpu,uint16_t phase,uint16_t state) {
  Write(cpu,0x3e,phase);Write(cpu,0x1a,state);Write(cpu,0x1c,0);
  if(!Row(cpu))ActRaiserHleFatal("Tree sequence has no initial row");
}
static bool Spawn(CpuState *cpu,bool after_peer,int offset,uint16_t phase,uint16_t animation) {
  Leaf(cpu,after_peer?bank_00_853D_M0X0:bank_00_8538_M0X0);
  if(cpu->_flag_C)return false; /* Full pool: retain earlier successful births. */
  const unsigned child=cpu->Y;
  if(!Slot(child))ActRaiserHleFatal("Tree allocator returned a non-slot child");
  cpu_write16(cpu,0,child+0x12,0xa9bf);cpu_write16(cpu,0,child+0x3e,phase);
  cpu_write16(cpu,0,child+0x38,animation);
  if(after_peer) {
    cpu_write16(cpu,0,child+2,(uint16_t)(cpu_read16(cpu,0,child+2)+offset));
    cpu_write16(cpu,0,child+4,(uint16_t)(cpu_read16(cpu,0,child+4)+24));
  }
  return true;
}
bool ActRaiser_TreeControllerEntry(CpuState *cpu) {
  if(!Shape(cpu,0xa9b3))return false;
  const unsigned phase=Read(cpu,0x3e);
  /* Active tags finish without a host cache after debug restore. */
  return (phase==kOpening && !Read(cpu,0x3a)) ||
    (phase>kOpening && phase<=kVisualPlaying && Slot(Read(cpu,0x3a)) && Read(cpu,0x3a)!=cpu->X) ||
    (Enabled() && !Read(cpu,0x3a) && Read(cpu,0x38) && Compatible(cpu));
}
RecompReturn ActRaiser_TreeController(CpuState *cpu) {
  if(!ActRaiser_TreeControllerEntry(cpu))ActRaiserHleFatal("Unsupported tree controller");
  const unsigned phase=Read(cpu,0x3e);
  if(phase<kOpening || phase>kVisualPlaying) {
    Start(cpu,kOpening,7);
  } else if(phase==kSeedBorn) {
    Write(cpu,0,0);Write(cpu,0x30,0);Write(cpu,0x2e,1);
    Leaf(cpu,bank_00_85E9_M0X0);Start(cpu,kFalling,16);
  } else if(phase==kVisualBorn) {
    Write(cpu,0,0);Write(cpu,0x30,0x20);Start(cpu,kVisualPlaying,Read(cpu,0x38));
  } else if(!Row(cpu)) {
    switch(phase) {
      case kOpening:
        if(Spawn(cpu,true,-32,kSeedBorn,0))Spawn(cpu,true,32,kSeedBorn,0);
        Write(cpu,0x38,0);Write(cpu,0x3e,0);Write(cpu,6,0);Write(cpu,8,0);break;
      case kFalling:
        Leaf(cpu,bank_00_8FE7_M0X0);
        Start(cpu,cpu->_flag_C?kLanded:kFalling,cpu->_flag_C?17:16);break;
      case kLanded:
        Spawn(cpu,false,0,kVisualBorn,24);Spawn(cpu,false,0,kVisualBorn,22);
        Start(cpu,kSprouting,23);break;
      case kSprouting: Write(cpu,0x38,16); /* fall through */
      case kWalking:
        if(phase==kWalking)Write(cpu,0x38,(uint16_t)(Read(cpu,0x38)-1));
        if(!Read(cpu,0x38)){Start(cpu,kWithering,20);break;}
        Leaf(cpu,bank_00_9108_M0X0);
        if(cpu->_flag_C)Leaf(cpu,bank_00_871E_M0X0);
        Start(cpu,kWalking,18);break;
      case kWithering: case kVisualPlaying:
        return Return(0x0085b7,0x00a9bf); /* Real native retire + RTS. */
      default:ActRaiserHleFatal("Invalid tree sequence phase");
    }
  }
  return Return(0x00a948,0x00a9bf); /* Native RTS consumes actual dispatcher frame. */
}
