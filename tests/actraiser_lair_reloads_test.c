#include "actraiser/actraiser_lair_reloads.h"
#include "regional/session/regional_lair_fingerprint.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t memory[65536];
static unsigned writes;
uint16 cpu_read16(CpuState *cpu, uint8 bank, uint16 at) {
  (void)cpu; assert(bank==0x7f); return ByteOrder_ReadLe16(memory+at);
}
void cpu_write16(CpuState *cpu, uint8 bank, uint16 at, uint16 value) {
  (void)cpu; assert(bank==0x7f && at>=0x9628 && at<0x9658 && !(at&1));
  ++writes; ByteOrder_WriteLe16(memory+at,value);
}
static void Install(const ArRegionalLairReloads *h, unsigned projection) {
  for(unsigned i=0;i<24;++i) ByteOrder_WriteLe16(memory+0x9628+2*i,h->delay[projection][i]);
}
static void Models(void) {
  ArRegionalLairReloads h={0}, copy;
  assert(ArRegionalLairReloads_Init(&h));
  assert(!ArRegionalLairReloads_Init(&h));
  for(unsigned source=0;source<3;++source) for(unsigned town=0;town<6;++town) for(unsigned count=0;count<10;++count) {
    ArRegionalLairReloads changed=h;
    for(unsigned n=0;n<count;++n) assert(ArRegionalLairReloads_ReduceTown(&changed,town));
    copy=(ArRegionalLairReloads){0};
    assert(ArRegionalLairReloads_Adopt(&copy,(ArRegionalSource)source,changed.delay[source==1]));
    assert(copy.approximate_towns==63 && !copy.diverged_towns);
    assert(!memcmp(copy.delay[source==1],changed.delay[source==1],48));
    assert(!ArRegionalLairReloads_Adopt(&copy,(ArRegionalSource)source,changed.delay[source==1]));
  }
  /* The hidden US 1->1 history cannot be recovered; choose zero reductions. */
  copy=(ArRegionalLairReloads){0};
  assert(ArRegionalLairReloads_Adopt(&copy,0,h.delay[0]));
  assert(!memcmp(copy.delay,h.delay,sizeof(h.delay)) && copy.approximate_towns==63);
  uint16_t unknown[24]; memcpy(unknown,h.delay[0],sizeof(unknown)); unknown[8]=65535;
  copy=(ArRegionalLairReloads){0}; assert(ArRegionalLairReloads_Adopt(&copy,0,unknown));
  assert(copy.diverged_towns==4 && copy.delay[0][8]==65535);
  uint8_t bytes[kArRegionalLairReloadEncodedBytes], original[sizeof(bytes)];
  h.approximate_towns=3; h.diverged_towns=2;
  assert(ArRegionalLairReloads_Encode(&h,bytes,sizeof(bytes)));
  assert(ArRegionalLairReloads_Decode(bytes,sizeof(bytes),&copy));
  assert(!memcmp(h.delay,copy.delay,sizeof(h.delay)) && copy.approximate_towns==3 && copy.diverged_towns==2);
  memcpy(original,bytes,sizeof(bytes)); const ArRegionalLairReloads before=copy;
  for(unsigned n=0;n<sizeof(bytes);++n) {
    assert(!ArRegionalLairReloads_Decode(bytes,n,&copy)); assert(!memcmp(&copy,&before,sizeof(copy)));
  }
  for(unsigned n=0;n<12;++n) {
    memcpy(bytes,original,sizeof(bytes));bytes[n]^=0x80;
    assert(!ArRegionalLairReloads_Decode(bytes,sizeof(bytes),&copy));
  }
  memset(bytes,0xaa,sizeof(bytes));
  assert(!ArRegionalLairReloads_Encode(&h,bytes,sizeof(bytes)-1) && bytes[0]==0xaa);
  h=(ArRegionalLairReloads){0}; memset(h.delay,0xff,sizeof(h.delay));
  assert(ArRegionalLairReloads_Encode(&h,bytes,sizeof(bytes)));
  for(unsigned i=12;i<sizeof(bytes);++i) assert(!bytes[i]);
  bytes[12]=1; assert(!ArRegionalLairReloads_Decode(bytes,sizeof(bytes),&copy));
}
static void Adapter(void) {
  CpuState cpu={.PB=3,.DB=0x7f,.A=0xabcd,.X=0xb30,.Y=0x1234,.S=0x1ef0};
  ArRegionalLairReloads fresh={0}; assert(ArRegionalLairReloads_Init(&fresh));
  for(unsigned source=0;source<3;++source) for(unsigned target=0;target<3;++target) {
    ArRegionalLairReloads h=fresh;
    for(unsigned t=0;t<6;++t) for(unsigned n=0;n<t;++n) assert(ArRegionalLairReloads_ReduceTown(&h,t));
    Install(&h,source==1); uint8_t before[65536];memcpy(before,memory,sizeof(before));
    const CpuState previous=cpu; writes=0;
    assert(ActRaiserLairReloads_Project(&h,&cpu,(ArRegionalSource)source,(ArRegionalSource)target));
    assert(!memcmp(&cpu,&previous,sizeof(cpu)));
    assert(!memcmp(before,memory,0x9628) && !memcmp(before+0x9658,memory+0x9658,sizeof(memory)-0x9658));
    assert(ActRaiserLairReloads_Check(&h,&cpu,(ArRegionalSource)target));
    if((source==1)==(target==1)) assert(!writes);
    assert(ActRaiserLairReloads_Project(&h,&cpu,(ArRegionalSource)target,(ArRegionalSource)source));
    assert(!memcmp(before,memory,sizeof(memory)));
    for(unsigned slot=0;slot<24;++slot) {
      ArRegionalLairReloads suspect=h;memory[0x9628+2*slot]^=1;memcpy(before,memory,sizeof(memory));writes=0;
      assert(!ActRaiserLairReloads_Project(&suspect,&cpu,(ArRegionalSource)source,(ArRegionalSource)target));
      assert(suspect.diverged_towns==(1u<<(slot/4)) && !writes && !memcmp(before,memory,sizeof(memory)));
      memory[0x9628+2*slot]^=1;
    }
  }
  for(unsigned source=0;source<3;++source) for(unsigned town=0;town<6;++town) for(unsigned token=0;token<=RECOMP_RETURN_OWNED_UNWIND;++token) {
    ArRegionalLairReloads h=fresh,candidate;unsigned captured=99;Install(&h,source==1);
    ByteOrder_WriteLe16(memory+0x7bfb,town*2);const CpuState previous=cpu;
    assert(ActRaiserLairReloads_BeginReduction(&h,&cpu,(ArRegionalSource)source,&candidate,&captured));
    assert(captured==town && !memcmp(&cpu,&previous,sizeof(cpu)));
    Install(&candidate,source==1);
    assert(ActRaiserLairReloads_EndReduction(&h,&cpu,(ArRegionalSource)source,&candidate,town,(RecompReturn)token)==(token==RECOMP_RETURN_NORMAL));
    if(token==RECOMP_RETURN_NORMAL) assert(!memcmp(h.delay,candidate.delay,sizeof(h.delay)));
    else assert(h.diverged_towns==(1u<<town) && !memcmp(h.delay,fresh.delay,sizeof(h.delay)));
  }
  cpu.DB=3;assert(!ActRaiserLairReloads_Entry(&cpu));cpu.DB=0x7f;
  cpu.x_flag=1;assert(!ActRaiserLairReloads_Entry(&cpu));cpu.x_flag=0;
  cpu.D=1;assert(!ActRaiserLairReloads_Entry(&cpu));cpu.D=0;
  cpu.emulation=1;assert(!ActRaiserLairReloads_Entry(&cpu));cpu.emulation=0;
  cpu.P=CPU_P_D;assert(!ActRaiserLairReloads_Entry(&cpu));cpu.P=0;
  cpu._flag_D=1;assert(!ActRaiserLairReloads_Entry(&cpu));cpu._flag_D=0;
  Install(&fresh,0);ArRegionalLairReloads h={0};assert(ActRaiserLairReloads_Initialize(&h,&cpu));
  assert(!ActRaiserLairReloads_Initialize(&h,&cpu));
  uint8_t image[kActRaiserSramSize]={0};
  for(unsigned i=0;i<24;++i) ByteOrder_WriteLe16(image+0x1573+2*i,fresh.delay[0][i]);
  Save_RecomputeChecksum(image);h=(ArRegionalLairReloads){0};
  assert(ActRaiserLairReloads_AdoptSaved(&h,image) && h.approximate_towns==63 && !h.diverged_towns);
  image[0]^=1;h=(ArRegionalLairReloads){0};assert(!ActRaiserLairReloads_AdoptSaved(&h,image));
}
static void Fingerprints(void) {
  ArRegionalLairReloads h={0};assert(ArRegionalLairReloads_Init(&h));
  uint8_t prior[32]={1},result[32],jp[32];bool native;
  for(unsigned from=0;from<3;++from) for(unsigned to=0;to<3;++to) {
    assert(ArRegionalLairReloads_Fingerprint(prior,&h,(ArRegionalSource)from,(ArRegionalSource)to,result,&native));
    assert(native==(from!=1 && to!=1)); assert((!memcmp(prior,result,32))==native);
  }
  assert(ArRegionalLairReloads_Fingerprint(prior,&h,1,0,jp,&native));
  for(unsigned p=0;p<2;++p) for(unsigned i=0;i<24;++i) {
    h.delay[p][i]^=1;assert(ArRegionalLairReloads_Fingerprint(prior,&h,1,0,result,&native));
    assert(memcmp(result,jp,32));h.delay[p][i]^=1;
  }
  h=(ArRegionalLairReloads){0};assert(!ArRegionalLairReloads_Fingerprint(prior,&h,1,0,result,&native));
}
int main(void) {
  Models();Adapter();Fingerprints();
  puts("lair reloads: reductions, estimates, nine round trips, countdown isolation, codec and replay identity passed");
  return 0;
}
