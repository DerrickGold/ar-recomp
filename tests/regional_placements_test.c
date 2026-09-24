#include "regional/action/regional_placements.h"
#include "byte_order.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint16_t scenes[] = {
  0x101,0x201,0x301,0x401,0x102,0x202,0x302,0x402,0x502,0x602,0x702,0x802,
  0x103,0x203,0x303,0x403,0x503,0x603,0x104,0x204,0x304,0x404,0x504,0x604,0x704,
  0x105,0x205,0x305,0x405,0x505,0x605,0x705,0x805,0x106,0x206,0x306,0x406,
  0x506,0x606,0x706,0x806,0x107,0x207,0x307,0x407,0x507,0x607,0x707,0x807
};
static uint32_t HashByte(uint32_t hash,unsigned byte) {return (hash^(uint8_t)byte)*16777619u;}
static uint32_t HashRow(uint32_t hash,const ActionPlacement *r) {
  const unsigned bytes[]={r->kind,r->x,r->y,r->parameter,r->type,r->retry_x,r->retry_y,r->reserve};
  for(unsigned i=0;i<sizeof(bytes)/sizeof(bytes[0]);++i)hash=HashByte(hash,bytes[i]);
  return hash;
}
static void Pure(void) {
  unsigned combinations=0;
  for(unsigned e=0;e<3;++e)for(unsigned p=0;p<3;++p)for(unsigned mode=0;mode<2;++mode)
    for(unsigned d=0;d<3;++d)for(unsigned scene=0;scene<49;++scene) {
      const ArRegionalPlacementPolicy policy={e,p};ActionPlacementProgram a,b;
      if(!ArRegionalPlacements_Copy(&policy,scenes[scene],mode,d,&a)) {
        fprintf(stderr,"rejected e=%u p=%u mode=%u d=%u scene=%04x\n",e,p,mode,d,scenes[scene]);
        assert(false);
      }
      assert(ActionPlacements_Validate(&a));
      assert(ArRegionalPlacements_Copy(&policy,scenes[scene],mode,d,&b));
      assert(a.count==b.count);
      for(size_t i=0;i<a.count;++i) {
        assert(a.rows[i].id==b.rows[i].id && HashRow(0,&a.rows[i])==HashRow(0,&b.rows[i]));
        if(a.rows[i].type==0x80)assert(a.rows[i].parameter<8);
      }
      ++combinations;
    }
  assert(combinations==2646);
  for(unsigned bad=0;bad<6;++bad) {
    ArRegionalPlacementPolicy policy={0,0};ActionPlacementProgram out,before;
    memset(&out,0xa5,sizeof(out));before=out;
    if(bad==0)policy.enemies=-1;
    if(bad==1)policy.pickups=3;
    assert(!ArRegionalPlacements_Copy(bad==2?NULL:&policy,bad==3?0xffff:0x101,
        false,bad==4?(ArRegionalDifficulty)-1:0,bad==5?NULL:&out));
    assert(!memcmp(&out,&before,sizeof(out)));
  }
  ActionPlacementProgram program={0};
  assert(!ActionPlacements_Validate(NULL) && !ActionPlacements_Validate(&program));
  program.count=kActionPlacementCapacity+1;assert(!ActionPlacements_Validate(&program));
  const ArRegionalPlacementPolicy policy={0,0};
  assert(ArRegionalPlacements_Copy(&policy,0x101,false,0,&program));
  ActionPlacementProgram bad=program;bad.rows[0].id=0;assert(!ActionPlacements_Validate(&bad));
  bad=program;bad.rows[1].id=bad.rows[0].id;assert(!ActionPlacements_Validate(&bad));
  bad=program;bad.rows[0].kind=255;assert(!ActionPlacements_Validate(&bad));
  bad=program;bad.rows[0].kind=kActionPlacement_Reserve;bad.rows[0].reserve=63;
  assert(!ActionPlacements_Validate(&bad));
  bad=program;bad.rows[0].kind=kActionPlacement_End;assert(!ActionPlacements_Validate(&bad));
  bad=program;--bad.count;assert(!ActionPlacements_Validate(&bad));
}
static uint8_t Read(const uint8_t *rom,unsigned bank,unsigned at) {
  assert(at>=0x8000 && at<=0xffff && bank<=0x1f);
  return rom[bank*0x8000+(at&0x7fff)];
}
static unsigned Word(const uint8_t *rom,unsigned bank,unsigned at) {
  return Read(rom,bank,at)|(unsigned)Read(rom,bank,at+1)<<8;
}
static void EqualRow(const ActionPlacement *a,const ActionPlacement *b) {
  assert(a->kind==b->kind && a->x==b->x && a->y==b->y && a->parameter==b->parameter &&
      a->type==b->type && a->retry_x==b->retry_x && a->retry_y==b->retry_y && a->reserve==b->reserve);
}
static void Rom(const uint8_t *rom,unsigned region,unsigned mode,unsigned difficulty) {
  const unsigned bank=region<2?10:31,base=region<2?0xb100:0x8000;
  const ArRegionalPlacementPolicy policy={region<2?region:2,region<2?region:2};
  unsigned index=region<2?base:base+Word(rom,bank,base+2*mode),rooms=0;
  for(;Word(rom,bank,index)!=0xffff;index+=4) {
    const unsigned scene=Word(rom,bank,index);assert(index<0xfff8);
    if(!(scene&255))continue;
    assert(++rooms<=49);
    unsigned cursor=base+Word(rom,bank,index+2)+3;
    for(unsigned boxes=0;Read(rom,bank,cursor)!=255;++boxes){assert(boxes<64);cursor+=5;}
    ++cursor;
    ActionPlacementProgram expected={0},actual;
    uint8_t seen[32768]={0};
    for(unsigned guard=0;;++guard) {
      assert(guard<256 && cursor>=0x8000 && cursor<=0xffff && !seen[cursor&0x7fff]);
      seen[cursor&0x7fff]=1;
      ActionPlacement row={0};unsigned byte=Read(rom,bank,cursor++);
      if(byte==0xfc){cursor=Word(rom,bank,cursor);continue;}
      if(byte==0xff)row.kind=kActionPlacement_End;
      else if(byte==0xfd){row.kind=kActionPlacement_Reserve;row.reserve=Read(rom,bank,cursor++);}
      else if(byte==0xfe) {
        row.kind=kActionPlacement_Wave;row.x=Read(rom,bank,cursor++);row.y=Read(rom,bank,cursor++);
        row.retry_x=Read(rom,bank,cursor++);row.retry_y=Read(rom,bank,cursor++);
      } else {
        row.x=byte;row.y=Read(rom,bank,cursor++);row.parameter=Read(rom,bank,cursor++);
        row.type=Read(rom,bank,cursor++);
        if(region>=2 && row.type<128 && (row.parameter==1 || row.parameter==2)) {
          if(difficulty==1 || (row.parameter==2 && difficulty!=2))continue;
          row.parameter=0;
        }
      }
      assert(expected.count<kActionPlacementCapacity);
      expected.rows[expected.count++]=row;
      if(byte==0xff)break;
    }
    assert(ArRegionalPlacements_Copy(&policy,scene,mode,difficulty,&actual));
    assert(actual.count==expected.count);
    for(size_t i=0;i<actual.count;++i)EqualRow(&actual.rows[i],&expected.rows[i]);
  }
  assert(rooms==49);
}
int main(int argc,char **argv) {
  Pure();
  if(argc==6)for(unsigned region=0;region<5;++region) {
    FILE *f=fopen(argv[region+1],"rb");assert(f);
    uint8_t *rom=malloc(0x100000);assert(rom);
    assert(fread(rom,1,0x100000,f)==0x100000 && fgetc(f)==EOF);fclose(f);
    for(unsigned mode=0;mode<2;++mode)for(unsigned d=0;d<3;++d)Rom(rom,region,mode,d);
    free(rom);
  } else assert(argc==1);
  puts("Regional placements: mixed profiles, capacity, stable rows, five-ROM ordered programs passed");
  return 0;
}
