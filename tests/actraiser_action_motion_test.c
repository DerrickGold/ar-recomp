#include "actraiser/actraiser_action_motion.h"
#include "regional/regional_action_motion.h"
#include "regional/regional_emitters.h"
#include "regional/regional_boss_rules.h"
#include "regional/regional_collision.h"
#include "regional/regional_fire_enemy.h"
#include "quintet_lzss.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t memory[65536];
static uint16_t snapshot;
static uint8_t emitters;
static uint64_t bosses;
static uint8_t collision;
static uint8_t fire;
uint8_t ActRaiserRegional_FireSnapshot(void){return fire;}
static bool decode_extents;
static unsigned calls;
static unsigned tail_target,tail_origin;
static RecompReturn native_result;
int cpu_hle_tailcall_request(uint32 pc,uint32 origin){tail_target=pc;tail_origin=origin;return 1;}
uint16_t ActRaiserRegional_ActionMotionSnapshot(void){return snapshot;}
uint8_t ActRaiserRegional_EmitterSnapshot(void){return emitters;}
uint64_t ActRaiserRegional_BossSnapshot(void){return bosses;}
uint8_t ActRaiserRegional_CollisionSnapshot(void){return collision;}
uint8 cpu_read8(CpuState *cpu,uint8 bank,uint16 at){(void)cpu;assert(bank==0 || bank==0x7e);return memory[at];}
uint16 cpu_read16(CpuState *cpu,uint8 bank,uint16 at){return cpu_read8(cpu,bank,at)|(uint16)cpu_read8(cpu,bank,at+1)<<8;}
void cpu_write8(CpuState *cpu,uint8 bank,uint16 at,uint8 value){(void)cpu;assert(bank==0 || bank==0x7e);memory[at]=value;}
void cpu_write16(CpuState *cpu,uint8 bank,uint16 at,uint16 value){cpu_write8(cpu,bank,at,value);cpu_write8(cpu,bank,at+1,value>>8);}
static uint16_t Read(unsigned at){return ByteOrder_ReadLe16(memory+at);}
static void Write(unsigned at,uint16_t value){ByteOrder_WriteLe16(memory+at,value);}
static int16_t Signed(uint8_t value){return value<128?value:(int16_t)((int)value-256);}
/* Native-reader stand-in. It deliberately owns all CPU/return side effects;
 * the regional adapter must preserve them and change only its declared outputs. */
static RecompReturn Native(CpuState *cpu) {
  if(native_result!=RECOMP_RETURN_NORMAL)return native_result;
  const unsigned x=cpu->X,base=Read(x+0x16),at=base+Read(base+2+Read(x+0x1a)*2)+Read(x+0x1c)*4;
  if(memory[at]==255) {
    Write(x+0x1c,0);cpu->_flag_C=1;cpu->P|=CPU_P_C;
    cpu->A=255;cpu->Y=0xbeef;cpu->S+=2;return native_result;
  }
  Write(x+0x24,memory[at+1]);
  Write(x+6,(uint16_t)(Signed(memory[at+2])*((Read(x+0x28)&0x4000)?-1:1)));
  Write(x+8,(uint16_t)(Signed(memory[at+3])*((Read(x+0x28)&0x8000)?-1:1)));
  Write(x+0x22,(memory[at]+Read(x+0x3c))&255);
  if(decode_extents) {
    const unsigned comp=base+Read(base+Read(base)+2*Read(x+0x22)),flip=Read(x+0x28);
    Write(x+0x20,comp);
    Write(x+0x0a,(uint16_t)Signed(memory[comp+!!(flip&0x4000)]));
    Write(x+0x0e,(uint16_t)Signed(memory[comp+!(flip&0x4000)]));
    Write(x+0x0c,(uint16_t)Signed(memory[comp+2+!!(flip&0x8000)]));
    Write(x+0x10,(uint16_t)Signed(memory[comp+2+!(flip&0x8000)]));
  }
  cpu->_flag_C=0;cpu->P&=~CPU_P_C;
  cpu->A=0x1234;cpu->Y=0xbeef;cpu->S+=2;return native_result;
}
RecompReturn bank_00_8E2F_M0X0(CpuState *cpu){assert(!cpu->m_flag && !ActRaiser_ActionMotionEntry(cpu));++calls;return Native(cpu);}
RecompReturn bank_00_8E2F_M1X0(CpuState *cpu){assert(cpu->m_flag && !ActRaiser_ActionMotionEntry(cpu));++calls;return Native(cpu);}
static const uint16_t sources[]={0xaa9a,0xac8e,0xb041,0xb0b4,0xdcdb,0xbba8,0xdfe5};
static uint16_t arrow_source=0xdfe5;
static CpuState Setup(unsigned family,unsigned state,unsigned row,unsigned flip,unsigned width) {
  memset(memory,0,sizeof(memory));memory[0x18]=(family==4 || family==6)?5:family==5?2:1;
  Write(0x912,family==6?arrow_source:sources[family]);Write(0x8f6,0x4000);memory[0x8f8]=0x7e;
  Write(0x8fa,state);Write(0x8fc,row);Write(0x908,flip);
  CpuState cpu={.A=0xabcd,.X=0x8e0,.Y=0x4321,.S=0x1f00,.P=width?CPU_P_M:0};
  cpu_p_to_mirrors(&cpu);calls=0;native_result=RECOMP_RETURN_NORMAL;return cpu;
}
static void CheckRow(unsigned family,unsigned state,unsigned row,unsigned delay,int dx) {
  /* These seven families predate the independent expanded head program. */
  for(unsigned mask=0;mask<(1u<<kArRegionalActionMotion_HeadWithdrawal);++mask)for(unsigned flip=0;flip<4;++flip)for(unsigned width=0;width<2;++width) {
    CpuState cpu=Setup(family,state,row,flip<<14,width),reference=cpu;snapshot=mask;
    Write(0x4000,0x600);Write(0x4002+state*2,0x100);
    const unsigned at=0x4100+row*4;memory[at]=31;memory[at+1]=delay;memory[at+2]=(uint8_t)dx;memory[at+3]=0xfd;
    uint8_t before[sizeof(memory)],expected[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    uint16_t duration=delay;int16_t horizontal=dx;
    const bool recognized=ArRegionalActionMotion_Row(mask,family,state,row,&duration,&horizontal);
    const bool change=recognized && (duration!=delay || horizontal!=dx);
    assert(ActRaiser_ActionMotionEntry(&cpu)==change);
    assert(Native(&reference)==RECOMP_RETURN_NORMAL);
    if(change){Write(0x904,duration);Write(0x8e6,(uint16_t)(horizontal*(flip&1?-1:1)));}
    memcpy(expected,memory,sizeof(memory));memcpy(memory,before,sizeof(memory));
    assert((change?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(memory,expected,sizeof(memory)) && !memcmp(&cpu,&reference,sizeof(cpu)) && calls==(unsigned)change);
  }
}
static void CheckRoms(char **paths) {
  static const unsigned offsets[5][5]={{0xcd695,0xcf335,0xd21f8,0x5782e,0xccf22},{0xcbe43,0xcdaf0,0xd06b7,0xc9fab,0xcb6d5},
      {0xcd696,0xcf335,0xd1b5a,0x5782e,0xccf23},{0xcce78,0xceb17,0xd0d9f,0x5782e,0xcc705},{0xcce78,0xce3fd,0xd0d72,0x5782e,0xcc705}};
  static uint8_t blobs[5][5][4096],rom[1048576];
  for(unsigned region=0;region<5;++region) {
    FILE *file=fopen(paths[region],"rb");assert(file);
    assert(fread(rom,1,sizeof(rom),file)==sizeof(rom));assert(!fclose(file));
    const unsigned head_entries[]={0xc8ff,0xc990,0xc5c6,0xc5c8,0xc5cb};
    const uint8_t *head=rom+head_entries[region]-0x8000;
    assert(!memcmp(head,(uint8_t[]){0xbd,0x30,0,0x89,0,4,0xf0,1,0x60},9));
    const unsigned hold=ArRegionalActionMotion_Descriptor(kArRegionalActionMotion_WallHeadShortHold)->value[region==1?1:region?2:0];
    assert(hold==ArRegionalActionMotion_Descriptor(kArRegionalActionMotion_WallHeadLongHold)->value[region==1?1:region?2:0]);
    if(region==1)assert(!hold && !memcmp(head+9,(uint8_t[]){0xa9,2,0},3));
    else {
      assert(head[9]==0xa9 && ByteOrder_ReadLe16(head+10)+1==hold && head[12]==0x20);
      assert(ByteOrder_ReadLe16(head+13)==(region?0x8612:0x86fa));
      assert(!memcmp(head+15,(uint8_t[]){0xa9,2,0},3));
    }
    for(unsigned area=0;area<5;++area) {
      const unsigned off=offsets[region][area],size=ByteOrder_ReadLe16(rom+off);
      assert(size<=4096 && QuintetLzss_DecompressAsset(rom+off,sizeof(rom)-off,blobs[region][area],size,NULL));
    }
  }
  for(unsigned region=0;region<5;++region) {
    const uint8_t *us=blobs[0][4],*other=blobs[region][4];
    const unsigned a=ByteOrder_ReadLe16(us+72),b=ByteOrder_ReadLe16(other+72);
    unsigned total=0;
    for(unsigned row=0;;++row) {
      unsigned native_row=row;uint8_t visual=us[a+4*row];
      (void)ArRegionalActionMotion_HeadRow(region==1?(1u<<kArRegionalActionMotion_HeadWithdrawal):0,row,&native_row,&visual);
      assert(visual==other[b+4*row]);
      if(visual==255)break;
      assert(!memcmp(us+a+4*native_row+1,other+b+4*row+1,3));
      const unsigned uc=ByteOrder_ReadLe16(us+ByteOrder_ReadLe16(us)+2*visual);
      const unsigned oc=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*visual);
      assert(!memcmp(us+uc,other+oc,5+7*us[uc+4]));total+=us[a+4*native_row+1]+1;
    }
    assert(total==(region==1?20:16));
  }
  static const unsigned states[7][4]={{21,0,0,0},{30,31,32,33},{31,0,0,0},{39,41,0,0},{16,19,0,0},{29,30,0,0},{41,0,0,0}};
  const uint8_t *emitter_us=blobs[0][1];
  const unsigned emitter_base=ByteOrder_ReadLe16(emitter_us+74);
  assert(!memcmp(emitter_us+emitter_base,(uint8_t[]){37,89,0,0,37,89,0,0,255},9));
  for(unsigned region=0;region<5;++region) {
    const uint8_t *other=blobs[region][1];const unsigned base=ByteOrder_ReadLe16(other+74);
    const unsigned count=region>=2?1:2;unsigned total=0;
    for(unsigned row=0;row<count;++row) {
      assert(other[base+4*row]==37 && !other[base+4*row+2] && !other[base+4*row+3]);
      total+=other[base+4*row+1]+1;
    }
    assert(other[base+4*count]==255);
    if(!region)total*=2;
    assert(total==ArRegionalEmitter_Descriptor(0)->value[region>=2?2:region]);
    unsigned host_total=0;
    for(unsigned row=0;row<2;++row) {
      uint16_t delay=89;
      (void)ArRegionalEmitter_Row(region>=2?2:region,36,row,&delay,0);
      host_total+=delay+1;
    }
    assert(host_total*(!region?2:1)==total);
    const unsigned comp=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*37);
    const unsigned us_comp=ByteOrder_ReadLe16(emitter_us+ByteOrder_ReadLe16(emitter_us)+2*37);
    assert(other[comp+4]==1 && !memcmp(other+comp,emitter_us+us_comp,12));
  }
  const unsigned counts[]={1,4,1,2,2,2,1};unsigned cases=0;
  for(unsigned family=0;family<kArRegionalActionMotionFamily_Count;++family)for(unsigned si=0;si<counts[family];++si) {
    const unsigned area=family<2?0:family==4?2:family==5?3:family==6?4:1,state=states[family][si];
    const uint8_t *us=blobs[0][area];const unsigned base=ByteOrder_ReadLe16(us+2+state*2);
    for(unsigned row=0;us[base+4*row]!=255;++row)for(unsigned region=0;region<5;++region) {
      const bool jp_arrow=family==kArRegionalActionMotion_Arrow && region==1;
      const uint8_t *other=blobs[region][area];const unsigned target=ByteOrder_ReadLe16(other+2+state*2)+(jp_arrow?0:4*row);
      uint16_t delay=us[base+4*row+1];int16_t dx=Signed(us[base+4*row+2]);
      (void)ArRegionalActionMotion_Row(region==1?(1u<<kArRegionalActionMotion_Count)-1:0,family,state,row,&delay,&dx);
      assert(dx==Signed(other[target+2]) && us[base+4*row+3]==other[target+3]);
      if(jp_arrow) {
        /* JP has a single non-flashing row. This speed-only setting preserves
         * Western visuals/delays, but every update uses JP's displacement. */
        assert(delay==us[base+4*row+1] && delay==1 && other[target+1]==0 && other[target]==0x2c);
      } else assert(delay==other[target+1] && us[base+4*row]==other[target]);
      /* Speed/delay rules must not import regional collision headers. */
      const unsigned us_comp=ByteOrder_ReadLe16(us+ByteOrder_ReadLe16(us)+2*us[base+4*row]);
      const unsigned other_comp=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*other[target]);
      if(jp_arrow) {
        assert(!memcmp(us+us_comp,(uint8_t[]){8,8,8,0},4));
        assert(!memcmp(other+other_comp,(uint8_t[]){16,16,8,0},4));
      } else assert(!memcmp(us+us_comp,other+other_comp,4));
      ++cases;
    }
  }
  printf("five-ROM numerical animation/vertical-motion/collision comparison: %u rows; emitter totals/identical stationary pose verified\n",cases);
}
static void CheckHeadPauses(void) {
  const uint16_t owners[]={0xc8e5,0xc8f3,0xc8c9,0xc8d7};
  for(unsigned owner=0;owner<4;++owner)for(unsigned mask=0;mask<4;++mask)for(unsigned flags=0;flags<32;++flags) {
    CpuState cpu=Setup(0,0,0,0,0);memory[0x18]=3;
    cpu.P=(uint8_t)((flags&7)|((flags&8)<<3)|((flags&16)<<3));cpu_p_to_mirrors(&cpu);
    Write(0x912,owners[owner]);Write(0x8f6,owner<2?0x4000:0x5000);
    snapshot=(uint16_t)(mask<<kArRegionalActionMotion_WallHeadShortHold);
    const bool skip=(mask&(owner<2?1:2))!=0;
    const CpuState before=cpu;uint8_t saved[sizeof(memory)];memcpy(saved,memory,sizeof(memory));
    assert(ActRaiser_WallHeadPauseEntry(&cpu)==skip);
    if(skip) {
      tail_target=tail_origin=0;
      assert(ActRaiser_WallHeadPause(&cpu)==RECOMP_RETURN_TAILCALL && tail_target==0xc90e && tail_origin==0xc908);
    }
    assert(!memcmp(&before,&cpu,sizeof(cpu)) && !memcmp(saved,memory,sizeof(memory)));
    snapshot=0xc00;
    for(unsigned malformed=0;malformed<11;++malformed) {
      cpu=before;memcpy(memory,saved,sizeof(memory));
      switch(malformed) {
        case 0:cpu.m_flag=1;break;case 1:cpu.x_flag=1;break;case 2:cpu.DB=1;break;
        case 3:cpu.PB=1;break;case 4:cpu.D=1;break;case 5:cpu.X++;break;
        case 6:memory[0x18]=7;break;case 7:Write(0x8f6,owner<2?0x5000:0x4000);break;
        case 8:Write(0x912,0xc1a2);break; /* Pharaoh-created head, not ordinary. */
        case 9:Write(0x910,0x400);break;case 10:cpu.P|=CPU_P_D;break;
      }
      assert(!ActRaiser_WallHeadPauseEntry(&cpu));
    }
  }
}
static void CheckIceRows(void) {
  static const uint8_t windows[2][12]={{5,5,0,0,9,5,0,0,13,5,0,0},{8,5,0,0,11,5,0,0,4,5,0,255}};
  for(unsigned region=0;region<3;++region)for(unsigned which=0;which<2;++which)
  for(unsigned encounter=0;encounter<2;++encounter)for(unsigned flip=0;flip<4;++flip)for(unsigned width=0;width<2;++width) {
    ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,region) && ArRegionalBoss_Resolve(&policy,&bosses));
    CpuState cpu=Setup(0,17+which,5+which,flip<<14,width),expected=cpu;
    Write(0x912,encounter?0xf760:0xf161);memory[0x18]=encounter?7:6;Write(0x8f6,0x5000);
    Write(0x5000,0x600);Write(0x5002+(17+which)*2,0x100);
    const unsigned at=0x5100+4*(5+which);memcpy(memory+at,windows[which],12);
    const bool changed=region==1 && encounter;
    assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    uint8_t saved[sizeof(memory)],wanted[sizeof(memory)];memcpy(saved,memory,sizeof(memory));
    if(changed)Write(0x8fc,7+which);
    assert(Native(&expected)==RECOMP_RETURN_NORMAL);memcpy(wanted,memory,sizeof(memory));memcpy(memory,saved,sizeof(memory));
    assert((changed?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(memory)));
    if(changed) {
      for(unsigned byte=0;byte<12;++byte) {
        memcpy(memory,saved,sizeof(memory));memory[at+byte]^=1;
        assert(!ActRaiser_ActionMotionEntry(&cpu));
      }
      for(unsigned token=1;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
        memcpy(memory,saved,sizeof(memory));native_result=token;
        const CpuState before=cpu;
        assert(ActRaiser_ActionMotion(&cpu)==(RecompReturn)token && !memcmp(&cpu,&before,sizeof(cpu)) && !memcmp(memory,saved,sizeof(memory)));
      }
      native_result=RECOMP_RETURN_NORMAL;
    }
  }
  bosses=0;
}
static void CheckTanzraRows(void) {
  const unsigned states[]={10,48,22},rows[]={7,0,0},lengths[]={4,4,16};
  static const uint8_t windows[3][16]={{11,31,0,0},{2,10,252,0},{31,3,0,0,33,3,0,0,35,3,0,0,37,3,0,0}};
  unsigned cases=0;
  for(unsigned choice=0;choice<81;++choice)for(unsigned which=0;which<3;++which)
  for(unsigned flip=0;flip<4;++flip)for(unsigned width=0;width<2;++width) {
    ArRegionalBossPolicy policy={{0}};unsigned digits=choice;
    for(unsigned i=kArRegionalBoss_TanzraClosing;i<kArRegionalBoss_Count;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalBoss_Resolve(&policy,&bosses));
    CpuState cpu=Setup(0,states[which],rows[which],flip<<14,width),expected=cpu,initial=cpu;
    Write(0x18,0x0807);Write(0x912,0xf80f);Write(0x8f6,0x5000);
    Write(0x5000,0x600);Write(0x5002+states[which]*2,0x100);
    const unsigned at=0x5100+4*rows[which];memcpy(memory+at,windows[which],lengths[which]);
    for(unsigned visual=0;visual<48;++visual) {
      const unsigned comp=0x680+8*visual;Write(0x5600+2*visual,comp);
      memory[0x5000+comp]=8;memory[0x5001+comp]=8;memory[0x5002+comp]=7;memory[0x5003+comp]=9;
    }
    const unsigned first=0x5000+Read(0x5600+31*2),second=0x5000+Read(0x5600+33*2);
    memory[first+2]=9;memory[first+3]=7;memory[second]=7;memory[second+1]=9;memory[second+2]=8;memory[second+3]=8;
    decode_extents=true;
    const bool changed=policy.source[which==0?kArRegionalBoss_TanzraClosing:which==1?kArRegionalBoss_TanzraUpperTurn:kArRegionalBoss_TanzraMinionTurn]==(which==2?2:1);
    assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    uint8_t saved[sizeof(memory)],wanted[sizeof(memory)];memcpy(saved,memory,sizeof(saved));
    if(changed && which==2)Write(0x8fc,2);
    assert(Native(&expected)==RECOMP_RETURN_NORMAL);
    if(changed && which<2)Write(0x904,which?11:3);
    memcpy(wanted,memory,sizeof(wanted));memcpy(memory,saved,sizeof(saved));
    assert((changed?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(memory)));++cases;
    if(changed) {
      for(unsigned byte=0;byte<lengths[which];++byte) {
        cpu=initial;memcpy(memory,saved,sizeof(saved));memory[at+byte]^=1;
        assert(!ActRaiser_ActionMotionEntry(&cpu));
      }
      for(unsigned bad=0;bad<12;++bad) {
        cpu=initial;memcpy(memory,saved,sizeof(saved));
        switch(bad) {
          case 0:Write(0x18,0x0707);break;case 1:Write(0x912,0xf760);break;
          case 2:Write(0x8f6,0x4000);break;case 3:memory[0x8f8]=0x7f;break;
          case 4:Write(0x8fa,23);break;case 5:Write(0x8fc,50);break;
          case 6:Write(cpu.S+1,0x969d);break;case 7:Write(0x5000,0x1001);break;
          case 8:Write(0x5002+states[which]*2,0);break;case 9:cpu.X++;break;
          case 10:cpu.D=1;break;case 11:cpu.x_flag=1;break;
        }
        assert(!ActRaiser_ActionMotionEntry(&cpu));
      }
      for(unsigned token=1;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
        cpu=initial;memcpy(memory,saved,sizeof(saved));native_result=token;
        assert(ActRaiser_ActionMotion(&cpu)==(RecompReturn)token && !memcmp(&cpu,&initial,sizeof(cpu)) && !memcmp(memory,saved,sizeof(saved)));
      }
      native_result=RECOMP_RETURN_NORMAL;
    }
    decode_extents=false;
  }
  bosses=0;printf("Tanzra timeline adapter: %u mixed-policy/width/facing cases, pose extents, signatures and escapes verified\n",cases);
}
static void CheckBossRows(void) {
  static const unsigned states[]={0,1,2,4};
  static const unsigned lengths[]={1,3,5,6};
  static const uint8_t rows[][6][4]={
    {{10,47,0,0}},{{10,23,0,0},{16,3,0,0},{11,0,0,0}},
    {{12,1,0,0},{13,1,0,0},{14,1,0,0},{15,1,0,0},{11,3,0,0}},
    {{8,19,0,0},{9,9,0,247},{9,8,0,248},{9,7,0,249},{9,5,0,250},{9,0,0,254}}
  };
  snapshot=0;emitters=0;
  for(unsigned mask=0;mask<64;++mask)for(unsigned encounter=0;encounter<2;++encounter)
  for(unsigned si=0;si<4;++si)for(unsigned row=0;row<lengths[si];++row)
  for(unsigned facing=0;facing<4;++facing)for(unsigned width=0;width<2;++width) {
    ArRegionalBossPolicy policy={{0}};
    for(unsigned i=0;i<6;++i)if(mask&(1u<<i))policy.source[i]=(i==2 || i==3)?2:1;
    assert(ArRegionalBoss_Resolve(&policy,&bosses));
    CpuState cpu=Setup(0,states[si],row,facing<<14,width),expected=cpu;
    Write(0x912,encounter?0xf6ca:0xaf5d);memory[0x18]=encounter?7:1;Write(0x8f6,0x5000);
    Write(0x5000,0x600);Write(0x5002+states[si]*2,0x100);
    const unsigned at=0x5100+row*4;memcpy(memory+at,rows[si][row],4);
    uint16_t duration=memory[at+1];
    const bool recognized=!encounter && ArRegionalBoss_MinoRow(bosses,states[si],row,&duration,
        Signed(memory[at+2]),Signed(memory[at+3]));
    const bool changed=recognized && duration!=memory[at+1];
    assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    assert(Native(&expected)==RECOMP_RETURN_NORMAL);
    if(changed)Write(0x904,duration);
    uint8_t wanted[sizeof(memory)];memcpy(wanted,memory,sizeof(memory));
    assert((changed?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(memory)));
  }
  bosses=0;
}
static void CheckEmitters(void) {
  for(unsigned timing=0;timing<3;++timing)for(unsigned position=0;position<3;++position) {
    const ArRegionalEmitterPolicy policy={{timing,position}};uint8_t resolved=255;
    assert(ArRegionalEmitter_Resolve(&policy,&resolved));
    assert(resolved==(timing|(position==2?4:0)));emitters=resolved;snapshot=0;
    for(unsigned row=0;row<2;++row)for(unsigned width=0;width<2;++width) {
      CpuState cpu=Setup(0,36,row,0,width);Write(0x912,0xb3bf);
      Write(0x4000,0x600);Write(0x404a,0x100);
      memory[0x4100+row*4]=37;memory[0x4101+row*4]=89;
      assert(ActRaiser_ActionMotionEntry(&cpu)==(timing==2));
      assert((timing==2?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
      assert(Read(0x904)==(timing==2?126+row:89) && !Read(0x8e6) && !Read(0x8e8));
    }
    CpuState cpu=Setup(0,36,0,0,0);Write(0x912,0xb3bf);cpu.P=CPU_P_C|CPU_P_V;cpu_p_to_mirrors(&cpu);
    assert(ActRaiser_EmitterCadenceEntry(&cpu)==(timing!=0));
    if(timing) {
      assert(ActRaiser_EmitterCadence(&cpu)==RECOMP_RETURN_TAILCALL && tail_target==0xb3e7 && tail_origin==0xb3e4);
      assert(cpu.A==0x2401 && cpu.P==(CPU_P_C|CPU_P_V) && cpu.S==0x1f00);
    }
    assert(ActRaiser_EmitterPositionEntry(&cpu)==(position==2));
    if(position==2)for(unsigned x=0;x<=65535;++x) {
      Write(0x8e2,x);cpu.P=CPU_P_I|CPU_P_C|CPU_P_V;cpu_p_to_mirrors(&cpu);
      assert(ActRaiser_EmitterPosition(&cpu)==RECOMP_RETURN_TAILCALL && tail_target==0xb3d5 && tail_origin==0xb3cb);
      const uint16_t expected=(uint16_t)(x<896?x+6:x-22);
      const bool overflow=x>=32768 && x<32790;
      assert(cpu.A==expected && Read(0x8e2)==expected && cpu.S==0x1f00 && cpu.X==0x8e0);
      assert(cpu.P==(CPU_P_I|(x>=896?CPU_P_C:0)|(overflow?CPU_P_V:0)|(expected&32768?CPU_P_N:0)));
    }
  }
  emitters=0;
}
static void CheckCollision(void) {
  static const uint8_t visuals[]={8,14,15,22,23,44,54};
  static const uint8_t native[][4]={{16,15,20,0},{16,16,32,24},{16,16,32,24},
      {28,12,24,24},{28,12,24,24},{8,8,8,0},{8,8,8,0}};
  static const uint8_t jp[][4]={{16,16,16,0},{16,16,31,24},{16,16,31,24},
      {36,12,24,24},{36,12,24,24},{16,16,8,0},{16,16,8,0}};
  unsigned cases=0;decode_extents=true;
  for(unsigned bits=0;bits<4;++bits)for(unsigned movement=0;movement<2;++movement)
  for(unsigned pose=0;pose<7;++pose)for(unsigned owner=0;owner<2;++owner)
  for(unsigned flip=0;flip<4;++flip)for(unsigned width=0;width<2;++width) {
    const bool arrow=pose>=5,changed=(bits&(arrow?2:1))!=0;
    const unsigned state=arrow?41:pose?14:10;
    CpuState cpu=Setup(0,state,0,flip<<14,width),reference=cpu;
    snapshot=movement?(1u<<kArRegionalActionMotion_ArrowSpeed):0;collision=bits;
    memory[0x18]=arrow?5:3;
    Write(0x912,arrow?(owner?0xdff3:0xdfe5):pose?(owner?0xc9be:0xc961):0xc863);
    Write(0x4000,0x600);Write(0x4002+state*2,0x100);memory[0x4100]=visuals[pose];
    memory[0x4101]=1;memory[0x4102]=arrow?254:0;
    Write(0x4600+2*visuals[pose],0x700);memcpy(memory+0x4700,native[pose],4);
    uint8_t saved[sizeof(memory)],wanted[sizeof(memory)];memcpy(saved,memory,sizeof(memory));
    const bool intercepted=changed || (arrow && movement);
    assert(ActRaiser_ActionMotionEntry(&cpu)==intercepted);
    Native(&reference);
    if(changed) {
      Write(0x8ea,jp[pose][flip&1?1:0]);Write(0x8ee,jp[pose][flip&1?0:1]);
      Write(0x8ec,jp[pose][flip&2?3:2]);Write(0x8f0,jp[pose][flip&2?2:3]);
    }
    if(arrow && movement)Write(0x8e6,(uint16_t)(flip&1?3:-3));
    memcpy(wanted,memory,sizeof(memory));memcpy(memory,saved,sizeof(memory));
    assert((intercepted?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(wanted,memory,sizeof(memory)) && !memcmp(&cpu,&reference,sizeof(cpu)));
    if(changed && !movement) {
      for(unsigned malformed=0;malformed<19;++malformed) {
        memcpy(memory,saved,sizeof(memory));cpu=reference;
        switch(malformed) {
          case 0:cpu.PB=1;break;case 1:cpu.DB=1;break;case 2:cpu.D=1;break;
          case 3:cpu.x_flag=1;break;case 4:cpu.X++;break;case 5:cpu.emulation=1;break;
          case 6:Write(0x8a,cpu.X);break;case 7:Write(0x912,0xc1a2);break;
          case 8:memory[0x18]=0;break;case 9:Write(0x8f6,0x5000);break;
          case 10:memory[0x8f8]=0x7f;break;case 11:Write(0x4002+state*2,1);break;
          case 12:Write(0x8fc,65535);break;case 13:memory[0x4100]=255;break;
          case 14:Write(0x4600+2*visuals[pose],0x100);break;
          case 15:Write(0x4600+2*visuals[pose],0xffff);break;
          case 16:memory[0x4701]++;break;case 17:cpu.P|=CPU_P_D;break;
          case 18:Write(cpu.S+1,0x969d);break;
        }
        assert(!ActRaiser_ActionMotionEntry(&cpu));
      }
      for(unsigned token=1;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
        memcpy(memory,saved,sizeof(memory));cpu=reference;native_result=token;
        assert(ActRaiser_ActionMotion(&cpu)==(RecompReturn)token);
        assert(!memcmp(&cpu,&reference,sizeof(cpu)) && !memcmp(memory,saved,sizeof(memory)));
      }
    }
    ++cases;
  }
  /* First spawn has descriptor Y, not an installed +32. Confirm both cleared
   * and reused sources, and that the birth continuation preserves registers. */
  for(unsigned owner=0;owner<4;++owner)for(unsigned stale=0;stale<3;++stale)for(unsigned flip=0;flip<4;++flip) {
    const bool arrow=owner>=2;const unsigned state=arrow?41:owner?19:15,pose=arrow?5:owner?2:1;
    CpuState cpu=Setup(0,state,0,flip<<14,0);collision=3;snapshot=0;
    const unsigned source=owner==0?0xc961:owner==1?0xc9be:owner==2?0xdfe5:0xdff3;
    memory[0x18]=arrow?5:3;Write(0x912,stale==0?0:stale==1?0xbba8:source);
    Write(0x4000,0x600);Write(0x4002+state*2,0x100);memory[0x4100]=visuals[pose];
    Write(0x4600+2*visuals[pose],0x700);memcpy(memory+0x4700,native[pose],4);
    Native(&cpu);cpu.Y=source;const CpuState reference=cpu;
    uint8_t wanted[sizeof(memory)];memcpy(wanted,memory,sizeof(memory));
    ByteOrder_WriteLe16(wanted+0x8ea,jp[pose][flip&1?1:0]);ByteOrder_WriteLe16(wanted+0x8ee,jp[pose][flip&1?0:1]);
    ByteOrder_WriteLe16(wanted+0x8ec,jp[pose][flip&2?3:2]);ByteOrder_WriteLe16(wanted+0x8f0,jp[pose][flip&2?2:3]);
    ByteOrder_WriteLe16(wanted+0x8e6,0);
    assert(ActRaiser_ActionCollisionBirthEntry(&cpu));
    assert(ActRaiser_ActionCollisionBirth(&cpu)==RECOMP_RETURN_TAILCALL && tail_origin==0x969e && tail_target==0x96a1);
    assert(!memcmp(wanted,memory,sizeof(memory)) && !memcmp(&cpu,&reference,sizeof(cpu)));
    cpu.Y=0xbba8;assert(!ActRaiser_ActionCollisionBirthEntry(&cpu));
  }
  {
    CpuState cpu=Setup(6,41,0,0,0);collision=3;snapshot=(1u<<kArRegionalActionMotion_Count)-1;
    Write(0x4000,0x600);Write(0x4054,0x100);memory[0x4100]=44;memory[0x4101]=1;memory[0x4102]=254;
    Write(0x4658,0x700);memcpy(memory+0x4700,native[5],4);Write(cpu.S+1,0x969d);
    assert(!ActRaiser_ActionMotionEntry(&cpu)); /* stale arrow source during any other birth */
  }
  decode_extents=false;collision=0;snapshot=0;native_result=RECOMP_RETURN_NORMAL;
  printf("regional collision: %u pose/facing/width/owner/motion combinations plus malformed/escape guards\n",cases);
}
static void CheckCollisionRoms(char **paths) {
  const unsigned offsets[5][2]={{0xd9943,0xccf22},{0xd731b,0xcb6d5},{0x37b41,0xccf23},{0x37b41,0xcc705},{0x37b41,0xcc705}};
  /* Compare all changed headers, not only the two readily reached sword poses. */
  static uint8_t rom[1048576],blobs[5][2][4096];
  for(unsigned region=0;region<5;++region) {
    FILE *file=fopen(paths[region],"rb");assert(file);
    assert(fread(rom,1,sizeof(rom),file)==sizeof(rom));assert(!fclose(file));
    for(unsigned family=0;family<2;++family) {
      const unsigned off=offsets[region][family],size=ByteOrder_ReadLe16(rom+off);
      assert(size<=4096 && QuintetLzss_DecompressAsset(rom+off,sizeof(rom)-off,blobs[region][family],size,NULL));
    }
  }
  const unsigned poses[]={8,14,15,22,23,44,54};unsigned cases=0;
  for(unsigned region=0;region<5;++region)for(unsigned pose=0;pose<7;++pose) {
    const unsigned family=pose>=5,visual=poses[pose];const uint8_t *us=blobs[0][family],*other=blobs[region][family];
    const unsigned original=ByteOrder_ReadLe16(us+ByteOrder_ReadLe16(us)+2*visual);
    const unsigned target=ByteOrder_ReadLe16(other+ByteOrder_ReadLe16(other)+2*(region==1 && family?44:visual));
    ArRegionalCollisionExtents ext={Signed(us[original]),Signed(us[original+1]),Signed(us[original+2]),Signed(us[original+3])},result=ext;
    assert(ArRegionalCollision_Pose(region==1?3:0,family,visual,&ext,&result)==(region==1));
    assert(result.left==Signed(other[target]) && result.right==Signed(other[target+1]) &&
        result.top==Signed(other[target+2]) && result.bottom==Signed(other[target+3]));
    ++cases;
  }
  printf("five-ROM collision headers: %u compositions verified\n",cases);
}
static void CheckFireRows(void) {
  static const int8_t native[2][8][2]={
    {{-2,-1},{-2,-2},{-2,-3},{-2,-3},{-2,-3},{-1,-2},{-1,-1},{-1,0}},
    {{1,0},{1,1},{1,2},{2,3},{2,3},{2,3},{2,2},{2,1}}};
  unsigned cases=0;
  for(unsigned mask=0;mask<16;++mask)for(unsigned state=13;state<=14;++state)
  for(unsigned row=0;row<8;++row)for(unsigned flip=0;flip<4;++flip)for(unsigned width=0;width<2;++width) {
    CpuState cpu=Setup(0,state,row,flip<<14,width),expected=cpu;
    memory[0x18]=3;Write(0x912,0xc3a5);fire=mask;snapshot=0;bosses=0;
    Write(0x4000,0x600);Write(0x4002+state*2,0x100);
    const unsigned at=0x4100+4*row;
    memory[at]=32+row/4;memory[at+1]=1;
    memory[at+2]=(uint8_t)native[state-13][row][0];memory[at+3]=(uint8_t)native[state-13][row][1];
    int16_t dx=native[state-13][row][0],dy=native[state-13][row][1];
    const bool changed=ArRegionalFire_CurveRow(mask,state,row,memory[at],1,&dx,&dy);
    assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    uint8_t before[sizeof(memory)],wanted[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    Native(&expected);
    if(changed){Write(cpu.X+6,(uint16_t)(dx*(flip&1?-1:1)));Write(cpu.X+8,(uint16_t)(dy*(flip&2?-1:1)));}
    memcpy(wanted,memory,sizeof(memory));memcpy(memory,before,sizeof(memory));
    assert((changed?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(memory)));
    if(changed)for(unsigned byte=0;byte<4;++byte) {
      memcpy(memory,before,sizeof(memory));memory[at+byte]^=1;
      assert(!ActRaiser_ActionMotionEntry(&cpu));
    }
    ++cases;
  }
  fire=0;
  printf("fire animation adapter: %u mixed-policy/row/facing/width cases and native-row guards passed\n",cases);
}
static void CheckDragonRows(void) {
  for(unsigned source=0;source<3;++source)for(unsigned state=1;state<=2;++state)
  for(unsigned flip=0;flip<4;++flip)for(unsigned width=0;width<2;++width) {
    ArRegionalBossPolicy policy;assert(ArRegionalBoss_Init(&policy,source) && ArRegionalBoss_Resolve(&policy,&bosses));
    CpuState cpu=Setup(0,state,0,flip<<14,width),expected=cpu;Write(0x18,0x0304);Write(0x912,0xd646);Write(0x8f6,0x5000);
    Write(0x5000,0x600);Write(0x5002+2*state,0x100);
    const uint8_t row[]={(uint8_t)(34-state),0,253,(uint8_t)(state==1?1:255),255};memcpy(memory+0x5100,row,5);
    uint8_t saved[sizeof(memory)],wanted[sizeof(memory)];memcpy(saved,memory,sizeof(memory));
    const bool changed=source==2;assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    Native(&expected);if(changed)Write(0x904,15);memcpy(wanted,memory,sizeof(memory));memcpy(memory,saved,sizeof(memory));
    assert((changed?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(memory)));
    if(changed)for(unsigned byte=0;byte<5;++byte) {
      memcpy(memory,saved,sizeof(memory));memory[0x5100+byte]^=1;assert(!ActRaiser_ActionMotionEntry(&cpu));
    }
  }
  bosses=0;
}
static void CheckViperRows(void) {
  unsigned cases=0;
  for(unsigned mix=0;mix<81;++mix)for(unsigned which=0;which<14;++which)
  for(unsigned flip=0;flip<4;++flip)for(unsigned width=0;width<2;++width) {
    ArRegionalBossPolicy policy={{0}};unsigned digits=mix;
    for(unsigned i=kArRegionalBoss_ViperChoice;i<kArRegionalBoss_Count;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalBoss_Resolve(&policy,&bosses));
    const bool floor=which>=6,rematch=which>=3 && which<6;
    const unsigned state=floor?8+(which-6)/4:4+which%3,row=floor?6+(which-6)%4:0;
    const unsigned source=floor?(state==8?0xe606:0xe5cf):rematch?0xf72a:0xe483;
    CpuState cpu=Setup(0,state,row,flip<<14,width),expected=cpu;
    Write(0x18,rematch?0x0607:0x0805);Write(0x912,source);Write(0x8f6,0x5000);
    Write(0x5000,0x600);Write(0x5002+2*state,0x100);
    static const uint8_t delays[]={1,1,1,15},speeds[]={1,2,4,6},visuals[]={17,25,26};
    const unsigned at=0x5100+4*row;
    uint16_t duration=floor?delays[row-6]:rematch?10:21;
    int16_t dy=floor?speeds[row-6]:rematch?8:4,dx=floor?0:((int)state-6)*dy/2;
    memory[at]=floor?(state==8?27:16):visuals[state-4];memory[at+1]=duration;
    memory[at+2]=(uint8_t)dx;memory[at+3]=(uint8_t)dy;
    const bool changed=ArRegionalBoss_ViperRow(bosses,rematch,state,row,memory[at],&duration,dx,&dy);
    assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    uint8_t saved[sizeof(memory)],wanted[sizeof(memory)];memcpy(saved,memory,sizeof(memory));
    Native(&expected);
    if(changed){Write(cpu.X+0x24,duration);Write(cpu.X+8,(uint16_t)(dy*(flip&2?-1:1)));}
    memcpy(wanted,memory,sizeof(memory));memcpy(memory,saved,sizeof(memory));
    assert((changed?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(wanted,memory,sizeof(memory)));
    if(changed)for(unsigned bad=0;bad<13;++bad) {
      memcpy(memory,saved,sizeof(memory));CpuState invalid=cpu;
      if(bad<4)memory[at+bad]^=1;
      else switch(bad) {
        case 4:Write(0x18,0x0105);break;
        case 5:Write(0x912,source+1);break;
        case 6:Write(0x8f6,0x4000);break;
        case 7:memory[0x8f8]=0x7f;break;
        case 8:Write(0x8fc,row+1);break;
        case 9:Write(0x5002+2*state,0xfffe);break;
        case 10:Write(0x5000,0x1001);break;
        case 11:Write(invalid.S+1,0x969d);break;
        case 12:Write(0x912,floor?0xe483:0xe5cf);break;
      }
      assert(!ActRaiser_ActionMotionEntry(&invalid));
    }
    ++cases;
  }
  bosses=0;
  printf("Viper animation adapter: %u mixed-policy/row/facing/width cases and owner/signature guards passed\n",cases);
}
static CpuState PharaohReader(unsigned encounter,unsigned row,unsigned width) {
  CpuState cpu=Setup(0,11,row,0,width);Write(0x18,encounter?0x0407:0x0603);
  Write(0x912,encounter?0xf6fa:0xc1a2);Write(0x8f6,0x5000);
  Write(0x5000,0x600);Write(0x5018,0x100);
  const uint8_t bounce[]={15,3,0,253,15,1,0,254,15,0,0,255,15,1,0,0,
    15,0,0,1,15,1,0,2,15,3,0,3,15,2,0,255,15,1,0,0,15,2,0,1,15,15,0,0,255};
  memcpy(memory+0x5100,bounce,sizeof(bounce));memory[0x5129]=encounter?31:15;return cpu;
}
static void CheckPharaohRows(void) {
  unsigned cases=0;
  for(unsigned mix=0;mix<27;++mix)for(unsigned encounter=0;encounter<2;++encounter)
  for(unsigned row=0;row<=11;++row)for(unsigned width=0;width<2;++width)for(unsigned flip=0;flip<4;++flip) {
    ArRegionalBossPolicy policy={{0}};unsigned digits=mix;
    for(unsigned i=19;i<22;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalBoss_Resolve(&policy,&bosses));
    CpuState cpu=PharaohReader(encounter,row,width),expected=cpu;Write(cpu.X+0x28,flip<<14);
    const bool changed=row==10 && policy.source[encounter?kArRegionalBoss_PharaohRematchLanding:kArRegionalBoss_PharaohLanding]==1;
    assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    uint8_t before[sizeof(memory)],wanted[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    if(changed)Write(cpu.X+0x1c,11);
    Native(&expected);memcpy(wanted,memory,sizeof(memory));memcpy(memory,before,sizeof(memory));
    assert((changed?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(memory,wanted,sizeof(memory)));++cases;
  }
  bosses=UINT64_C(1)<<(2*kArRegionalBoss_PharaohLanding);
  for(unsigned bad=0;bad<58;++bad) {
    CpuState cpu=PharaohReader(0,10,0);
    if(bad<45)memory[0x5100+bad]^=1;
    else switch(bad) {
      case 45:Write(0x18,0x0503);break;case 46:Write(0x912,0xc8c9);break;
      case 47:Write(0x8f6,0x4000);break;case 48:memory[0x8f8]=0x7f;break;
      case 49:Write(0x8fa,25);break;case 50:Write(0x5018,0xfffe);break;
      case 51:Write(0x5018,0);break;case 52:Write(0x5000,0x1001);break;
      case 53:Write(0x5000,0x12c);break;case 54:Write(cpu.S+1,0x969d);break;
      case 55:cpu.DB=1;break;case 56:cpu.D=1;break;case 57:cpu.X++;break;
    }
    assert(!ActRaiser_ActionMotionEntry(&cpu));
  }
  for(unsigned escape=1;escape<=RECOMP_RETURN_OWNED_UNWIND;++escape) {
    CpuState cpu=PharaohReader(0,10,0),expected=cpu;
    native_result=(RecompReturn)escape;
    uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    assert(ActRaiser_ActionMotionEntry(&cpu) && ActRaiser_ActionMotion(&cpu)==native_result);
    assert(!memcmp(memory,before,sizeof(memory)) && !memcmp(&cpu,&expected,sizeof(cpu)));
  }
  bosses=0;native_result=RECOMP_RETURN_NORMAL;
  printf("Pharaoh landing: %u mixed-policy/row/facing/width cases, 58 invalid contexts and escape restoration passed\n",cases);
}
static CpuState HeadReader(unsigned row,unsigned flags) {
  CpuState cpu=Setup(0,35,row,0,0);Write(0x18,0x0605);Write(0x912,0xe3a1);
  Write(0xa5,0x4000);Write(0x4048,0x100);Write(0x4000,0x600);
  const uint8_t sequence[]={40,7,0,0,38,3,0,0,37,3,0,0,255,41,1,254,0};
  memcpy(memory+0x4100,sequence,sizeof(sequence));
  const uint8_t bottoms[]={16,19,21,24};
  for(unsigned i=0;i<4;++i) {
    Write(0x4600+2*(37+i),0x800+40*i);
    memcpy(memory+0x4800+40*i,(uint8_t[]){8,8,16,bottoms[i],4},5);
  }
  cpu.P=flags;cpu_p_to_mirrors(&cpu);Write(cpu.S+1,0x8633);return cpu;
}
static void CheckHeadProgram(void) {
  for(unsigned mask=0;mask<(1u<<kArRegionalActionMotion_Count);++mask)for(unsigned row=0;row<6;++row) {
    unsigned mapped=99;uint8_t visual=99;
    const bool accepted=(mask&(1u<<kArRegionalActionMotion_HeadWithdrawal)) && row<=4;
    assert(ArRegionalActionMotion_HeadRow(mask,row,&mapped,&visual)==accepted);
    if(!accepted)assert(mapped==99 && visual==99);
  }
  unsigned mapped=99;uint8_t visual=99;
  assert(!ArRegionalActionMotion_HeadRow(UINT16_MAX,0,&mapped,&visual) && mapped==99 && visual==99);
  assert(!ArRegionalActionMotion_HeadRow(0x1000,0,NULL,&visual));
  assert(!ArRegionalActionMotion_HeadRow(0x1000,0,&mapped,NULL));
  const unsigned peers[]={0,1,0xaaa,0xfff};unsigned cases=0;
  for(unsigned region=0;region<3;++region)for(unsigned peer=0;peer<4;++peer)
  for(unsigned row=0;row<6;++row)for(unsigned flags=0;flags<16;++flags)
  for(unsigned tagged=0;tagged<2;++tagged)for(unsigned width=0;width<2;++width) {
    snapshot=peers[peer]|(region==1?(1u<<kArRegionalActionMotion_HeadWithdrawal):0);
    CpuState cpu=HeadReader(row+(tagged?0x100:0),(flags&1)|(flags&2)|((flags&4)<<4)|((flags&8)<<4)|(width?CPU_P_M:0)),expected=cpu;
    Write(cpu.X+0x28,(flags&3)<<14);decode_extents=true;
    uint8_t saved[sizeof(memory)],wanted[sizeof(memory)];memcpy(saved,memory,sizeof(memory));
    const bool changed=(region==1 || tagged) && row<=4;
    assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    if(changed) {
      static const unsigned native[]={0,1,1,2,3};
      Write(cpu.X+0x1c,native[row]);Write(cpu.X+0x3c,row==1?1:0);Native(&expected);
      Write(cpu.X+0x3c,0);if(row<4)Write(cpu.X+0x1c,0x100+row);
      memcpy(wanted,memory,sizeof(memory));memcpy(memory,saved,sizeof(memory));
      assert(ActRaiser_ActionMotion(&cpu)==RECOMP_RETURN_NORMAL);
      assert(!memcmp(wanted,memory,sizeof(memory)) && !memcmp(&cpu,&expected,sizeof(cpu)));
    } else {
      assert(!memcmp(saved,memory,sizeof(memory)) && !memcmp(&cpu,&expected,sizeof(cpu)));
    }
    ++cases;
  }
  snapshot=UINT16_MAX;CpuState invalid_policy=HeadReader(1,0);
  assert(!ActRaiser_ActionMotionEntry(&invalid_policy));
  snapshot=1u<<kArRegionalActionMotion_HeadWithdrawal;
  for(unsigned bad=0;bad<33;++bad) {
    CpuState cpu=HeadReader(1,0);
    if(bad<13)memory[0x4100+bad]^=1;
    else switch(bad) {
      case 13:cpu.PB=1;break;case 14:cpu.DB=1;break;case 15:cpu.D=1;break;
      case 16:cpu.X=0x1aa0;break;case 17:cpu.x_flag=1;break;case 18:cpu.emulation=1;break;
      case 19:cpu.P|=CPU_P_D;break;case 20:cpu._flag_D=1;break;case 21:cpu.X++;break;
      case 22:Write(0x8a,cpu.X);break;case 23:Write(0x18,0x0505);break;case 24:Write(0x912,0xe3a2);break;
      case 25:Write(0x8fa,34);break;case 26:Write(0x8f6,0x5000);break;case 27:memory[0x8f8]=0x7f;break;
      case 28:Write(cpu.X+0x3c,1);break;case 29:Write(cpu.S+1,0x969d);break;
      case 30:memory[0x4800+80+3]++;break;
      case 31:Write(0x4600+78,0xfffe);break;case 32:Write(0x4000,0x1001);break;
    }
    assert(!ActRaiser_ActionMotionEntry(&cpu));
  }
  for(unsigned token=1;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
    CpuState cpu=HeadReader(0x101,0),expected=cpu;uint8_t saved[sizeof(memory)];memcpy(saved,memory,sizeof(memory));
    native_result=token;assert(ActRaiser_ActionMotionEntry(&cpu));
    assert(ActRaiser_ActionMotion(&cpu)==token && !memcmp(&cpu,&expected,sizeof(cpu)) && !memcmp(saved,memory,sizeof(memory)));
  }
  snapshot=0;decode_extents=false;
  printf("head withdrawal: %u policy/row/flag/width/cache cases, 33 invalid contexts and escape restoration passed\n",cases);
}
static CpuState PlantReader(unsigned row,unsigned flags) {
  CpuState cpu=Setup(0,2,row,0,0);Write(0x18,0x0305);Write(0x912,0xd974);Write(cpu.X+0x16,0x5000);
  Write(0x5000,0x600);Write(0x5006,0x100);
  memcpy(memory+0x5100,(uint8_t[]){3,3,0,0,4,3,0,0,255},9);
  for(unsigned i=0;i<3;++i) {
    Write(0x5604+2*i,0x800+32*i);
    memcpy(memory+0x5800+32*i,(uint8_t[]){i?16:8,8,16,16,2},5);
  }
  cpu.P=flags;cpu_p_to_mirrors(&cpu);return cpu;
}
static void CheckPlantProgram(void) {
  unsigned cases=0;
  for(unsigned mix=0;mix<81;++mix)for(unsigned row=0;row<6;++row)
  for(unsigned flags=0;flags<16;++flags)for(unsigned tagged=0;tagged<2;++tagged)for(unsigned width=0;width<2;++width) {
    ArRegionalBossPolicy policy={{0}};unsigned digits=mix;
    for(unsigned i=22;i<26;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalBoss_Resolve(&policy,&bosses));
    CpuState cpu=PlantReader(row+(tagged?0x100:0),(flags&3)|((flags&12)<<4)|(width?CPU_P_M:0)),expected=cpu;
    Write(cpu.X+0x28,(flags&3)<<14);decode_extents=true;
    uint8_t saved[sizeof(memory)],wanted[sizeof(memory)];memcpy(saved,memory,sizeof(memory));
    const bool changed=(policy.source[23]==2 || tagged) && row<=4;
    assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    if(changed) {
      const unsigned native[]={0,0,1,0,2};
      Write(cpu.X+0x1c,native[row]);Write(cpu.X+0x3c,row==0?0xffff:0);Native(&expected);
      Write(cpu.X+0x3c,0);if(row<4)Write(cpu.X+0x1c,0x100+row);
      memcpy(wanted,memory,sizeof(memory));memcpy(memory,saved,sizeof(memory));
      assert(ActRaiser_ActionMotion(&cpu)==RECOMP_RETURN_NORMAL);
      assert(!memcmp(memory,wanted,sizeof(memory)) && !memcmp(&cpu,&expected,sizeof(cpu)));
    } else assert(!memcmp(memory,saved,sizeof(memory)) && !memcmp(&cpu,&expected,sizeof(cpu)));
    ++cases;
  }
  bosses=UINT64_C(2)<<(2*kArRegionalBoss_PlantOpen);
  for(unsigned bad=0;bad<25;++bad) {
    CpuState cpu=PlantReader(0,0);
    if(bad<9)memory[0x5100+bad]^=1;
    else switch(bad) {
      case 9:Write(0x18,0x0405);break;case 10:Write(0x912,0xe3a1);break;
      case 11:Write(cpu.X+0x16,0x4000);break;case 12:memory[cpu.X+0x18]=0x7f;break;
      case 13:Write(cpu.X+0x3a,0x920);break;case 14:Write(cpu.X+0x3c,1);break;
      case 15:Write(cpu.S+1,0x969d);break;case 16:Write(0x5006,0xffff);break;
      case 17:Write(0x5000,0x108);break;case 18:Write(0x5000,0x1001);break;
      case 19:memory[0x5800]++;break;case 20:memory[0x5803]++;break;
      case 21:memory[0x5804]++;break;case 22:Write(0x5604,0xffff);break;
      case 23:cpu.PB=1;break;case 24:cpu.X++;break;
    }
    assert(!ActRaiser_ActionMotionEntry(&cpu));
  }
  for(unsigned token=1;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
    CpuState cpu=PlantReader(0x100,0),expected=cpu;uint8_t before[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    native_result=(RecompReturn)token;assert(ActRaiser_ActionMotionEntry(&cpu));
    assert(ActRaiser_ActionMotion(&cpu)==token && !memcmp(memory,before,sizeof(memory)) && !memcmp(&cpu,&expected,sizeof(cpu)));
  }
  bosses=0;decode_extents=false;native_result=RECOMP_RETURN_NORMAL;
  printf("Plant open program: %u mixed-policy/row/facing/width/cache cases, 25 invalid contexts and escape restoration passed\n",cases);
}
static void CheckPlantWindups(void) {
  unsigned cases=0;
  for(unsigned mix=0;mix<81;++mix)for(unsigned state=4;state<=6;state+=2)
  for(unsigned row=0;row<2;++row)for(unsigned width=0;width<2;++width)for(unsigned flip=0;flip<4;++flip) {
    ArRegionalBossPolicy policy={{0}};unsigned digits=mix;
    for(unsigned i=22;i<26;++i){policy.source[i]=digits%3;digits/=3;}
    assert(ArRegionalBoss_Resolve(&policy,&bosses));
    CpuState cpu=Setup(0,state,row,flip<<14,width),expected=cpu;
    Write(0x18,0x0305);Write(cpu.X+0x32,0xd974);Write(cpu.X+0x16,0x5000);Write(cpu.X+0x3a,0x920);
    Write(0x5000,0x600);Write(0x5002+2*state,0x100);
    const uint8_t program[]={state==4?5:6,7,0,0,state==4?7:8,0,0,0,255};memcpy(memory+0x5100,program,sizeof(program));
    const bool changed=!row && policy.source[state==4?kArRegionalBoss_PlantLowWindup:kArRegionalBoss_PlantHighWindup]==2;
    assert(ActRaiser_ActionMotionEntry(&cpu)==changed);
    uint8_t before[sizeof(memory)],wanted[sizeof(memory)];memcpy(before,memory,sizeof(memory));
    Native(&expected);if(changed)Write(cpu.X+0x24,23);
    memcpy(wanted,memory,sizeof(memory));memcpy(memory,before,sizeof(memory));
    assert((changed?ActRaiser_ActionMotion(&cpu):Native(&cpu))==RECOMP_RETURN_NORMAL);
    assert(!memcmp(memory,wanted,sizeof(memory)) && !memcmp(&cpu,&expected,sizeof(cpu)));
    if(changed)for(unsigned bad=0;bad<13;++bad) {
      memcpy(memory,before,sizeof(memory));CpuState invalid=cpu;
      if(bad<9)memory[0x5100+bad]^=1;
      else switch(bad) {
        case 9:Write(cpu.X+0x3a,0);break;case 10:Write(0x5000,0x108);break;
        case 11:Write(cpu.S+1,0x969d);break;case 12:Write(0x18,0x0805);break;
      }
      assert(!ActRaiser_ActionMotionEntry(&invalid));
    }
    ++cases;
  }
  bosses=0;printf("Plant wind-ups: %u mixed-policy/row/facing/width cases and signature guards passed\n",cases);
}
int main(int argc,char **argv) {
  assert(argc==1 || argc==6);
  CheckHeadProgram();
  CheckCollision();
  CheckFireRows();
  CheckDragonRows();
  CheckViperRows();
  CheckPharaohRows();
  CheckPlantProgram();CheckPlantWindups();
  CheckIceRows();
  CheckTanzraRows();
  CheckBossRows();
  CheckEmitters();
  CheckHeadPauses();
  CheckRow(0,21,0,1,-3);CheckRow(0,21,1,1,-3);
  for(unsigned row=0;row<11;++row)CheckRow(1,30,row,row<8?5:7,row<8?0:-2);
  for(unsigned state=31;state<=33;++state)for(unsigned row=0;row<4;++row)CheckRow(1,state,row,5,-2);
  CheckRow(2,31,0,59,0);CheckRow(3,39,0,9,0);CheckRow(3,39,1,47,0);
  for(unsigned row=0;row<5;++row)CheckRow(3,41,row,row==4?47:3,0);
  CheckRow(4,16,0,47,0);CheckRow(4,16,1,7,0);CheckRow(4,19,0,47,0);
  for(unsigned row=1;row<4;++row)CheckRow(4,19,row,row==2?7:5,0);
  const unsigned sword_straight[]={7,2,1,1,5,1,2,3,31},sword_high[]={15,3,6,3,31};
  for(unsigned row=0;row<9;++row)CheckRow(5,29,row,sword_straight[row],0);
  for(unsigned row=0;row<5;++row)CheckRow(5,30,row,sword_high[row],0);
  for(unsigned owner=0;owner<2;++owner) {
    arrow_source=owner?0xdff3:0xdfe5;
    CheckRow(6,41,0,1,-2);CheckRow(6,41,1,1,-2);
  }
  for(unsigned token=1;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
    CpuState cpu=Setup(0,21,0,0,0);snapshot=1;Write(0x4000,0x600);Write(0x402c,0x100);
    memory[0x4100]=4;memory[0x4101]=1;memory[0x4102]=253;
    native_result=token;const CpuState before=cpu;uint8_t saved[sizeof(memory)];memcpy(saved,memory,sizeof(memory));
    assert(ActRaiser_ActionMotionEntry(&cpu));assert(ActRaiser_ActionMotion(&cpu)==native_result);
    assert(!memcmp(&before,&cpu,sizeof(cpu)) && !memcmp(saved,memory,sizeof(memory)));
  }
  for(unsigned malformed=0;malformed<12;++malformed) {
    CpuState cpu=Setup(0,21,0,0,0);snapshot=1;Write(0x4000,0x600);Write(0x402c,0x100);
    memory[0x4100]=4;memory[0x4101]=1;memory[0x4102]=253;
    switch(malformed) {
      case 0:cpu.DB=1;break;case 1:cpu.PB=1;break;case 2:cpu.D=1;break;case 3:cpu.x_flag=1;break;
      case 4:cpu.X++;break;case 5:Write(0x912,0);break;case 6:memory[0x18]=0;break;
      case 7:Write(0x8f6,0x5000);break;case 8:Write(0x402c,0xffff);break;
      case 9:memory[0x4100]=255;break;case 10:memory[0x4102]=252;break;case 11:cpu.emulation=1;break;
    }
    assert(!ActRaiser_ActionMotionEntry(&cpu));
  }
  if(argc==6){CheckRoms(argv+1);CheckCollisionRoms(argv+1);}
  printf("action motion: 57 owner/rows x%u mixes x4 facings x2 widths, escape and shape checks passed\n",1u<<kArRegionalActionMotion_HeadWithdrawal);return 0;
}
