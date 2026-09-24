#include "regional/towns/regional_lair_history.h"
#include "regional/towns/regional_lair_reloads.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ArRegionalLairAccounting Policy(unsigned bits) {
  return (ArRegionalLairAccounting){bits&1 ? 1:0, bits&2 ? 1:0,
      bits&4 ? 1:0, bits&8 ? 1:0, bits&16 ? 1:0};
}
static uint16_t Read(const ArRegionalLairHistory *h, unsigned bits, unsigned lair) {
  const ArRegionalLairAccounting p=Policy(bits); uint16_t value=0xffff;
  assert(ArRegionalLairHistory_Read(h,&p,lair,&value)); return value;
}
static void Init(ArRegionalLairHistory *h) {
  memset(h,0,sizeof(*h));
  for (unsigned town=0;town<6;++town) assert(ArRegionalLairHistory_InitTown(h,town));
}
static unsigned Bcd(unsigned decimal) {
  unsigned word=0;
  for (unsigned shift=0;shift<16;shift+=4) { word|=(decimal%10)<<shift; decimal/=10; }
  return word;
}

static void Policies(void) {
  for (unsigned v=0;v<243;++v) {
    unsigned digits=v, sources[5], expected=0;
    for (unsigned n=0;n<5;++n) { sources[n]=digits%3; digits/=3; if(sources[n]==1)expected|=1u<<n; }
    ArRegionalLairAccounting p={sources[0],sources[1],sources[2],sources[3],sources[4]};
    unsigned actual=999; assert(ArRegionalLairAccounting_Projection(&p,&actual) && actual==expected);
  }
  ArRegionalLairAccounting p={0}; unsigned projection=999;
  p.score_route=kArRegionalSource_Count;
  assert(!ArRegionalLairAccounting_Projection(&p,&projection) && projection==999);
  assert(!ArRegionalLairAccounting_Projection(NULL,&projection));
  assert(!ArRegionalLairAccounting_Projection(&p,NULL));
}
static void Lifecycle(void) {
  ArRegionalLairHistory h={0},before=h;
  const ArRegionalLairAccounting p={0}; uint16_t out=0x1234;
  assert(ArRegionalLairHistory_Valid(&h));
  assert(!ArRegionalLairHistory_Read(&h,&p,0,&out) && out==0x1234);
  assert(!ArRegionalLairHistory_KillAttempt(&h,0));
  assert(!ArRegionalLairHistory_MiracleAttempt(&h,0));
  assert(!ArRegionalLairHistory_HouseLost(&h,0,0,0));
  assert(!ArRegionalLairHistory_SettleScore(&h,0,0x1000,1));
  assert(!memcmp(&h,&before,sizeof(h)));
  for (unsigned town=0;town<6;++town) {
    assert(ArRegionalLairHistory_InitTown(&h,town));
    before=h;
    assert(!ArRegionalLairHistory_InitTown(&h,town));
    assert(!ArRegionalLairHistory_AdoptTown(&h,town,0,(uint16_t[4]){1,2,3,4}));
    assert(!memcmp(&h,&before,sizeof(h)));
  }
  assert(h.initialized_towns==63 && !h.approximate_towns);
  for (unsigned l=0;l<24;++l) for (unsigned bits=0;bits<32;++bits) {
    uint16_t seed; assert(ArRegionalLair_Seed(bits&1 ? 1:0,l,&seed));
    assert(Read(&h,bits,l)==seed);
  }
  before=h;
  assert(!ArRegionalLairHistory_InitTown(&h,6));
  assert(!ArRegionalLairHistory_KillAttempt(&h,24));
  assert(!ArRegionalLairHistory_MiracleAttempt(&h,24));
  assert(!ArRegionalLairHistory_HouseLost(&h,0,0,16));
  assert(!ArRegionalLairHistory_SettleScore(&h,0,0xabcd,1));
  assert(!ArRegionalLairHistory_Read(&h,&p,24,&out) && out==0x1234);
  assert(!memcmp(&h,&before,sizeof(h)));
  h.approximate_towns=64; assert(!ArRegionalLairHistory_Valid(&h));
  h.approximate_towns=0; h.initialized_towns=64; assert(!ArRegionalLairHistory_Valid(&h));
  assert(!ArRegionalLairHistory_Valid(NULL));
  Init(&h);
  assert(ArRegionalLairHistory_MarkDiverged(&h,2));
  before=h;
  assert(!ArRegionalLairHistory_Read(&h,&p,8,&out) && out==0x1234);
  assert(!ArRegionalLairHistory_KillAttempt(&h,8));
  assert(!ArRegionalLairHistory_InitTown(&h,2));
  assert(!ArRegionalLairHistory_AdoptTown(&h,2,0,(uint16_t[4]){1,2,3,4}));
  assert(!memcmp(&before,&h,sizeof(h)));
  assert(ArRegionalLairHistory_KillAttempt(&h,0)); /* Other towns still tracked. */
}
static void Adoption(void) {
  const uint16_t remaining[4]={0,20,65535,301};
  for (unsigned source=0;source<3;++source) for (unsigned town=0;town<6;++town) {
    ArRegionalLairHistory h={0};
    assert(ArRegionalLairHistory_AdoptTown(&h,town,source,remaining));
    assert(h.initialized_towns==(1u<<town) && h.approximate_towns==h.initialized_towns);
    for (unsigned bits=0;bits<32;++bits) for (unsigned n=0;n<4;++n) {
      uint16_t from,to; assert(ArRegionalLair_Seed(source,town*4+n,&from));
      assert(ArRegionalLair_Seed(bits&1 ? 1:0,town*4+n,&to));
      int expected=(int)remaining[n]+to-from;
      if(expected<0)expected=0;
      if(expected>65535)expected=65535;
      assert(Read(&h,bits,town*4+n)==expected);
    }
    const ArRegionalLairHistory before=h;
    for (unsigned n=0;n<100;++n) { /* Switching is read-only, never reverse-estimation. */
      assert(Read(&h,source==1 ? 1:0,town*4+1)==20);
      (void)Read(&h,source==1 ? 0:1,town*4+1);
    }
    assert(!memcmp(&h,&before,sizeof(h)));
    assert(ArRegionalLairHistory_KillAttempt(&h,town*4));
    assert(h.approximate_towns==(1u<<town));
  }
  ArRegionalLairHistory h={0},before=h;
  assert(!ArRegionalLairHistory_AdoptTown(&h,0,kArRegionalSource_Count,remaining));
  assert(!ArRegionalLairHistory_AdoptTown(&h,0,0,NULL));
  assert(!memcmp(&h,&before,sizeof(h)));
}
static void WordArithmetic(void) {
  ArRegionalLairHistory h; Init(&h);
  for (unsigned word=0;word<65536;++word) {
    for (unsigned p=0;p<32;++p) h.stock[p][0]=(uint16_t)word;
    assert(ArRegionalLairHistory_KillAttempt(&h,0));
    for (unsigned p=0;p<32;++p) assert(Read(&h,p,0)==(word?word-1:0));
    for (unsigned p=0;p<32;++p) h.stock[p][0]=(uint16_t)word;
    assert(ArRegionalLairHistory_MiracleAttempt(&h,0));
    const unsigned subtraction=(word+65536-10)&65535;
    const unsigned expected=!word || subtraction>=32768 ? 0:subtraction;
    for (unsigned p=0;p<32;++p) assert(Read(&h,p,0)==expected);
  }
  Init(&h);
  for(unsigned n=0;n<200;++n)assert(ArRegionalLairHistory_KillAttempt(&h,0));
  assert(!Read(&h,0,0) && Read(&h,1,0)==50);
  assert(ArRegionalLairHistory_KillAttempt(&h,0));
  assert(!Read(&h,0,0) && Read(&h,1,0)==49);
}
static void Houses(void) {
  for(unsigned mask=0;mask<16;++mask)for(unsigned subtype=0;subtype<256;++subtype) {
    ArRegionalLairHistory h; Init(&h);
    const ArRegionalLairHistory before=h;
    assert(ArRegionalLairHistory_HouseLost(&h,2,(uint8_t)subtype,mask));
    for(unsigned p=0;p<32;++p) {
      uint16_t expected[24]; memcpy(expected,before.stock[p],sizeof(expected));
      unsigned units=(p&2)?4:4+2*((subtype/16)%4),at=0;
      while(units && mask!=15) {
        if(!(mask&(1u<<at))) { ++expected[8+at]; --units; }
        at=(at+1)%4;
      }
      assert(!memcmp(expected,h.stock[p],sizeof(expected)));
    }
  }
  ArRegionalLairHistory h={0};
  assert(ArRegionalLairHistory_AdoptTown(&h,0,0,(uint16_t[4]){1,9,10,20}));
  for(unsigned n=1;n<4;++n)assert(ArRegionalLairHistory_MiracleAttempt(&h,n));
  for(unsigned n=0;n<3;++n)assert(ArRegionalLairHistory_HouseLost(&h,0,0,1));
  assert(Read(&h,0,0)==1 && Read(&h,0,1)==6 && Read(&h,0,2)==3 && Read(&h,0,3)==13);
  /* Not equivalent to one batched twelve-unit distribution [1,4,4,14]. */
  for(unsigned p=0;p<32;++p)h.stock[p][3]=65535;
  assert(ArRegionalLairHistory_HouseLost(&h,0,0,7));
  for(unsigned p=0;p<32;++p)assert(Read(&h,p,3)==3);
}
static void Scores(void) {
  ArRegionalLairHistory h; Init(&h);
  const unsigned starts[]={0,1,100,32768,65535};
  for(unsigned decimal=0;decimal<10000;++decimal)for(unsigned count=0;count<4;++count) {
    for(unsigned p=0;p<32;++p)for(unsigned n=0;n<4;++n)h.stock[p][4+n]=starts[(decimal+n)%5];
    assert(ArRegionalLairHistory_SettleScore(&h,1,(uint16_t)Bcd(decimal),(uint16_t)count));
    for(unsigned p=0;p<32;++p)for(unsigned n=0;n<4;++n) {
      unsigned value=starts[(decimal+n)%5];
      if(count!=2 && (count==1 || p&16)) {
        unsigned units=decimal/10*2;
        if(p&4)units=decimal<650 ? 0:((decimal-650)>>5)*10;
        units/=4;
        value=(p&8) ? (units>value ? 0:value-units) : (value+units)&65535;
      }
      assert(Read(&h,p,4+n)==value);
    }
  }
}
static void RomTables(const char *path) {
  FILE *file=fopen(path,"rb"); assert(file);
  /* Explicit country byte, not the path or localized cartridge title. */
  assert(!fseek(file,0x7fd9,SEEK_SET)); const int region=fgetc(file);
  const ArRegionalSource source=region==0 ? kArRegionalSource_Japan : kArRegionalSource_US;
  assert(!fseek(file,region==0 ? 0x1b5ae:0x1b825,SEEK_SET));
  uint8_t rows[24*9]; assert(fread(rows,1,sizeof(rows),file)==sizeof(rows) && !fclose(file));
  for(unsigned n=0;n<24;++n) {
    uint16_t value;
    assert(ArRegionalLair_Seed(source,n,&value) && value==rows[n*9+4]);
    assert(ArRegionalLair_Reload(source,n,&value) && value==(rows[n*9+5]|rows[n*9+6]<<8));
  }
}
static void Codec(void) {
  ArRegionalLairHistory h={0},decoded;
  uint8_t bytes[kArRegionalLairHistoryEncodedBytes+1],before[sizeof(bytes)];
  assert(ArRegionalLairHistory_InitTown(&h,0));
  assert(ArRegionalLairHistory_AdoptTown(&h,5,1,(uint16_t[4]){0,1,65535,350}));
  for(unsigned i=0;i<300;++i)assert(ArRegionalLairHistory_KillAttempt(&h,0));
  assert(ArRegionalLairHistory_HouseLost(&h,0,0x20,2));
  assert(ArRegionalLairHistory_SettleScore(&h,0,0x1250,1));
  memset(bytes,0xa5,sizeof(bytes)); memcpy(before,bytes,sizeof(bytes));
  assert(!ArRegionalLairHistory_Encode(&h,bytes,kArRegionalLairHistoryEncodedBytes-1));
  assert(!memcmp(bytes,before,sizeof(bytes)));
  assert(ArRegionalLairHistory_Encode(&h,bytes,sizeof(bytes)));
  assert(bytes[sizeof(bytes)-1]==0xa5 && !memcmp(bytes,"ARLHIST1",8));
  assert(bytes[8]==33 && bytes[9]==32 && bytes[10]==0 && bytes[11]==0);
  memset(&decoded,0xa5,sizeof(decoded));
  assert(ArRegionalLairHistory_Decode(bytes,kArRegionalLairHistoryEncodedBytes,&decoded));
  assert(decoded.initialized_towns==h.initialized_towns &&
      decoded.approximate_towns==h.approximate_towns && decoded.diverged_towns==h.diverged_towns &&
      !memcmp(decoded.stock,h.stock,sizeof(h.stock)));
  h=decoded; /* Canonical padding for the bytewise failure sentinels below. */
  for(unsigned size=0;size<sizeof(bytes);++size) {
    if(size==kArRegionalLairHistoryEncodedBytes)continue;
    assert(!ArRegionalLairHistory_Decode(bytes,size,&decoded));
    assert(!memcmp(&decoded,&h,sizeof(h)));
  }
  assert(!ArRegionalLairHistory_Decode(bytes,sizeof(bytes),&decoded));
  memcpy(before,bytes,sizeof(bytes));
  const unsigned corrupt[]={0,7,8,9,10,11,20}; /* unknown town's slot4 at20 */
  for(unsigned n=0;n<sizeof(corrupt)/sizeof(corrupt[0]);++n) {
    bytes[corrupt[n]]^=0x40;
    assert(!ArRegionalLairHistory_Decode(bytes,kArRegionalLairHistoryEncodedBytes,&decoded));
    assert(!memcmp(&decoded,&h,sizeof(h)));
    memcpy(bytes,before,sizeof(bytes));
  }
  h.stock[0][4]=1;
  assert(!ArRegionalLairHistory_Encode(&h,bytes,sizeof(bytes)));
  assert(!memcmp(bytes,before,sizeof(bytes)));
  assert(!ArRegionalLairHistory_Encode(NULL,bytes,sizeof(bytes)));
  assert(!ArRegionalLairHistory_Encode(&h,NULL,sizeof(bytes)));
  assert(!ArRegionalLairHistory_Decode(NULL,kArRegionalLairHistoryEncodedBytes,&decoded));
  assert(!ArRegionalLairHistory_Decode(bytes,kArRegionalLairHistoryEncodedBytes,NULL));
  Init(&h); assert(ArRegionalLairHistory_MarkDiverged(&h,1));
  assert(ArRegionalLairHistory_Encode(&h,bytes,sizeof(bytes)));
  assert(ArRegionalLairHistory_Decode(bytes,kArRegionalLairHistoryEncodedBytes,&decoded));
  assert(decoded.diverged_towns==2 && !memcmp(decoded.stock,h.stock,sizeof(h.stock)));
}
int main(int argc,char **argv) {
  Policies(); Lifecycle(); Adoption(); WordArithmetic(); Houses(); Scores(); Codec();
  for(int n=1;n<argc;++n)RomTables(argv[n]);
  puts("lair histories: independent projections, arithmetic, adoption and optional ROM tables passed");
  return 0;
}
