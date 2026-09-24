#include "regional/regional_fingerprint.h"
#include "snesrecomp/support/digest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int failures;
static int CompareDigest(const void *a,const void *b){return memcmp(a,b,32);}
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Fingerprint(const ArRegionalCostPolicy *costs, const ArRegionalCostPolicy *active_costs,
    const ArRegionalTimerPolicy *timers, const ArRegionalTimerPolicy *active_timers,
    uint8_t out[32], bool *baseline) {
  const ArRegionalRules requested = {.costs = *costs, .timers = *timers};
  const ArRegionalRules effective = {.costs = *active_costs, .timers = *active_timers};
  return ArRegionalRules_Fingerprint(&requested, &effective, out, baseline);
}
int main(void) {
  ArRegionalCostPolicy us, eu, jp;
  ArRegionalCosts_Init(&us, kArRegionalSource_US);
  ArRegionalCosts_Init(&eu, kArRegionalSource_Europe);
  ArRegionalCosts_Init(&jp, kArRegionalSource_Japan);
  uint8_t native[32], current[32], pending[32], effective[32];
  bool baseline;
  CHECK(ArRegionalCosts_Fingerprint(&us, &us, native, &baseline) && baseline);
  CHECK(ArRegionalCosts_Fingerprint(&eu, &us, current, &baseline) && baseline);
  CHECK(!memcmp(native, current, 32));
  ArRegionalRules r = {.costs = us}, e = {.costs = eu};
  uint8_t placement_hashes[81][32];
  for(unsigned i=0;i<81;++i) {
    ArRegionalRules req={.placements={i%3,i/3%3}},eff={.placements={i/9%3,i/27}};
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,placement_hashes[i],&baseline) && baseline==!i);
    for(unsigned j=0;j<i;++j)CHECK(memcmp(placement_hashes[i],placement_hashes[j],32));
  }
  for(unsigned source=0;source<3;++source) {
    ArRegionalRules req={.placements={source,0}},eff={0};
    uint8_t levels[3][32];
    for(unsigned level=0;level<3;++level) {
      req.difficulty.level=level;
      CHECK(ArRegionalRules_Fingerprint(&req,&eff,levels[level],&baseline));
      for(unsigned j=0;j<level;++j)CHECK((memcmp(levels[level],levels[j],32)!=0)==(source==2));
    }
  }
  ArRegionalRules invalid_placements={.placements={3,0}},empty_placements={0};
  CHECK(!ArRegionalRules_Fingerprint(&invalid_placements,&empty_placements,current,&baseline));
  invalid_placements.placements=(ArRegionalPlacementPolicy){0,3};
  CHECK(!ArRegionalRules_Fingerprint(&empty_placements,&invalid_placements,current,&baseline));
  uint8_t hazard_hashes[9][32];
  uint8_t mosaic_hashes[9][32];
  for(unsigned request=0;request<3;++request)for(unsigned active=0;active<3;++active) {
    ArRegionalRules req={.mosaic=request},eff={.mosaic=active};
    const unsigned index=request*3+active;
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,mosaic_hashes[index],&baseline) && baseline==!index);
    for(unsigned j=0;j<index;++j)CHECK(memcmp(mosaic_hashes[index],mosaic_hashes[j],32));
  }
  ArRegionalRules invalid_mosaic={.mosaic=3},empty_mosaic={0};
  CHECK(!ArRegionalRules_Fingerprint(&invalid_mosaic,&empty_mosaic,current,&baseline));
  CHECK(!ArRegionalRules_Fingerprint(&empty_mosaic,&invalid_mosaic,current,&baseline));
  for(unsigned request=0;request<3;++request)for(unsigned active=0;active<3;++active) {
    ArRegionalRules req={.hazards=request},eff={.hazards=active};
    const unsigned index=request*3+active;
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,hazard_hashes[index],&baseline) && baseline==!index);
    for(unsigned j=0;j<index;++j)CHECK(memcmp(hazard_hashes[index],hazard_hashes[j],32));
  }
  ArRegionalRules invalid_hazard={.hazards=3},empty_hazard={0};
  CHECK(!ArRegionalRules_Fingerprint(&invalid_hazard,&empty_hazard,current,&baseline));
  uint8_t music_hashes[4][32];bool music_seen[4]={0};
  for(unsigned request=0;request<3;++request)for(unsigned active=0;active<3;++active) {
    ArRegionalRules req={.music=request},eff={.music=active};
    const unsigned index=(request==1)|((active==1)<<1);
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!index);
    if(music_seen[index])CHECK(!memcmp(music_hashes[index],current,32));
    else {memcpy(music_hashes[index],current,32);music_seen[index]=true;}
  }
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)CHECK(memcmp(music_hashes[i],music_hashes[j],32));
  ArRegionalRules invalid_music={.music=3},empty_music={0};
  CHECK(!ArRegionalRules_Fingerprint(&invalid_music,&empty_music,current,&baseline));
  uint8_t terrain_hashes[9][32];
  for(unsigned request=0;request<3;++request)for(unsigned active=0;active<3;++active) {
    ArRegionalRules req={.terrain=request},eff={.terrain=active};
    const unsigned index=request*3+active;
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,terrain_hashes[index],&baseline) && baseline==!index);
    for(unsigned j=0;j<index;++j)CHECK(memcmp(terrain_hashes[index],terrain_hashes[j],32));
  }
  ArRegionalRules invalid_terrain={.terrain=3},empty_terrain={0};
  CHECK(!ArRegionalRules_Fingerprint(&invalid_terrain,&empty_terrain,current,&baseline));
  uint8_t mode_hashes[16][32];bool mode_seen[16]={0};
  for(unsigned combination=0;combination<81;++combination) {
    ArRegionalRules req={0},eff={0};unsigned digits=combination,index=0;
    for(unsigned i=0;i<4;++i) {
      const unsigned source=digits%3;digits/=3;
      if(i<2)req.mode_entry.source[i]=source;else eff.mode_entry.source[i-2]=source;
      if(source==2)index|=1u<<i;
    }
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!index);
    if(mode_seen[index])CHECK(!memcmp(mode_hashes[index],current,32));
    else {memcpy(mode_hashes[index],current,32);mode_seen[index]=true;}
  }
  for(unsigned i=0;i<16;++i)for(unsigned j=0;j<i;++j)CHECK(memcmp(mode_hashes[i],mode_hashes[j],32));
  uint8_t inventory_hashes[4][32];bool inventory_seen[4]={0};
  for(unsigned request=0;request<3;++request)for(unsigned active=0;active<3;++active) {
    ArRegionalRules req={.spell_inventory=request},eff={.spell_inventory=active};
    const unsigned index=(request==2)|((active==2)<<1);
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!index);
    if(inventory_seen[index])CHECK(!memcmp(inventory_hashes[index],current,32));
    else {memcpy(inventory_hashes[index],current,32);inventory_seen[index]=true;}
  }
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)CHECK(memcmp(inventory_hashes[i],inventory_hashes[j],32));
  ArRegionalSpellInventory collection={0};
  CHECK(ArRegionalSpellInventory_Fingerprint(native,&collection,current) && !memcmp(native,current,32));
  ArRegionalSpellInventory_Reset(&collection,true);
  CHECK(ArRegionalSpellInventory_Fingerprint(native,&collection,current) && memcmp(native,current,32));
  collection.spells[255]=4;
  CHECK(ArRegionalSpellInventory_Fingerprint(native,&collection,pending) && !memcmp(current,pending,32));
  CHECK(ArRegionalSpellInventory_Push(&collection,2));
  CHECK(ArRegionalSpellInventory_Fingerprint(native,&collection,current) && memcmp(current,pending,32));
  uint8_t selected_spell;
  CHECK(ArRegionalSpellInventory_BeginCast(&collection,&selected_spell));
  CHECK(ArRegionalSpellInventory_Fingerprint(native,&collection,pending) && memcmp(current,pending,32));
  ArRegionalSpellInventory_Interrupt(&collection);
  CHECK(ArRegionalSpellInventory_Fingerprint(native,&collection,pending) && !memcmp(current,pending,32));
  collection.spells[0]=5;
  CHECK(!ArRegionalSpellInventory_Fingerprint(native,&collection,pending));
  uint8_t start_hashes[16][32];bool start_seen[16]={0};
  for(unsigned combination=0;combination<81;++combination) {
    ArRegionalRules req={0},eff={0};unsigned digits=combination,index=0;
    for(unsigned i=0;i<4;++i) {
      const unsigned source=digits%3;digits/=3;
      if(i<2)req.action_start.source[i]=source;else eff.action_start.source[i-2]=source;
      if((i%2==0 && source==1) || (i%2 && source==2))index|=1u<<i;
    }
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!index);
    if(start_seen[index])CHECK(!memcmp(start_hashes[index],current,32));
    else {memcpy(start_hashes[index],current,32);start_seen[index]=true;}
  }
  for(unsigned i=0;i<16;++i)for(unsigned j=0;j<i;++j)CHECK(memcmp(start_hashes[i],start_hashes[j],32));
  uint8_t life_hashes[4][32];bool life_seen[4]={0};
  for(unsigned req_source=0;req_source<3;++req_source)for(unsigned eff_source=0;eff_source<3;++eff_source) {
    ArRegionalRules req={.score_lives=req_source},eff={.score_lives=eff_source};
    const unsigned index=(req_source==2)|((eff_source==2)<<1);
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!index);
    if(life_seen[index])CHECK(!memcmp(life_hashes[index],current,32));
    else {memcpy(life_hashes[index],current,32);life_seen[index]=true;}
  }
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)CHECK(memcmp(life_hashes[i],life_hashes[j],32));
  uint8_t difficulty_hashes[256][32];bool difficulty_seen[256]={0};
  for(unsigned n=0;n<243;++n)for(unsigned level=0;level<3;++level)for(unsigned active=0;active<2;++active) {
    ArRegionalRules req={0},eff={0};unsigned digits=n;
    ArRegionalDifficultyPolicy *p=active?&eff.difficulty:&req.difficulty;p->level=level;
    for(unsigned i=0;i<5;++i){p->source[i]=digits%3;digits/=3;}
    ArRegionalDifficultySnapshot values;CHECK(ArRegionalDifficulty_Resolve(p,&values));
    const unsigned id=ArRegionalDifficulty_Identity(&values),index=id+(id?128*active:0);
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!id);
    if(difficulty_seen[index])CHECK(!memcmp(difficulty_hashes[index],current,32));
    else {memcpy(difficulty_hashes[index],current,32);difficulty_seen[index]=true;}
  }
  for(unsigned i=0;i<256;++i)if(difficulty_seen[i])for(unsigned j=0;j<i;++j)if(difficulty_seen[j])
    CHECK(memcmp(difficulty_hashes[i],difficulty_hashes[j],32));
  uint8_t hold_hashes[64][32];bool hold_seen[64]={0};
  uint8_t fire_hashes[256][32];bool fire_seen[256]={0};
  for(unsigned n=0;n<6561;++n) {
    ArRegionalRules req={0},eff={0};unsigned digits=n,index=0;
    for(unsigned i=0;i<8;++i) {
      const unsigned source=digits%3;digits/=3;
      if(i<4)req.fire_enemy.source[i]=source;else eff.fire_enemy.source[i-4]=source;
      if(source==1)index|=1u<<i;
    }
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!index);
    if(!index)CHECK(!memcmp(native,current,32));
    if(fire_seen[index])CHECK(!memcmp(fire_hashes[index],current,32));
    else {memcpy(fire_hashes[index],current,32);fire_seen[index]=true;}
  }
  qsort(fire_hashes,256,32,CompareDigest);
  for(unsigned i=1;i<256;++i)CHECK(memcmp(fire_hashes[i-1],fire_hashes[i],32));
  for(unsigned n=0;n<729;++n) {
    ArRegionalRules req={0},eff={0};unsigned digits=n,index=0;
    for(unsigned i=0;i<6;++i) {
      const unsigned source=digits%3;digits/=3;
      if(i<3)req.cast_hold.source[i]=source;else eff.cast_hold.source[i-3]=source;
      if(source==2)index|=1u<<i;
    }
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!index);
    if(!index)CHECK(!memcmp(native,current,32));
    if(hold_seen[index])CHECK(!memcmp(hold_hashes[index],current,32));
    else {memcpy(hold_hashes[index],current,32);hold_seen[index]=true;}
  }
  for(unsigned i=0;i<64;++i)for(unsigned j=0;j<i;++j)CHECK(memcmp(hold_hashes[i],hold_hashes[j],32));
  /* Freeze the v39 digest byte layout independently of the current leaf count.
   * Native child defaults (including aliases) must not add another domain. */
  for(unsigned region=1;region<3;++region) {
    ArRegionalRules req={0},eff={0};uint8_t bytes[174]="ARACTORSTAT-R1",expected[32];
    memcpy(bytes+16,native,32);
    for(unsigned i=0;i<63;++i) {
      req.actor_stats.source[i]=region;
      bytes[48+i]=(uint8_t)ArRegionalActorStats_Descriptor(i)->value[region];
      bytes[111+i]=(uint8_t)ArRegionalActorStats_Descriptor(i)->value[0];
    }
    req.actor_stats.source[kArRegionalActorStat_TanzraMinionHp]=1;
    eff.actor_stats.source[kArRegionalActorStat_TanzraMinionReward]=1;
    CHECK(sr_support_sha256(bytes,sizeof(bytes),expected));
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && !baseline && !memcmp(expected,current,32));
  }
  for(unsigned i=0;i<kArRegionalActorStat_Count;++i) {
    const ArRegionalActorStatDescriptor *d=ArRegionalActorStats_Descriptor(i);
    uint8_t hashes[9][32];
    for(unsigned request=0;request<3;++request)for(unsigned active=0;active<3;++active) {
      ArRegionalRules req={0},eff={0};req.actor_stats.source[i]=request;eff.actor_stats.source[i]=active;
      const unsigned index=3*request+active;
      const bool native_values=d->value[request]==d->value[0] && d->value[active]==d->value[0];
      CHECK(ArRegionalRules_Fingerprint(&req,&eff,hashes[index],&baseline) && baseline==native_values);
      CHECK((memcmp(native,hashes[index],32)==0)==native_values);
      for(unsigned other=0;other<index;++other) {
        const bool same=d->value[other/3]==d->value[request] && d->value[other%3]==d->value[active];
        CHECK((memcmp(hashes[other],hashes[index],32)==0)==same);
      }
    }
  }
  uint8_t platform_skull_digests[256][32];bool skull_seen[256]={false};
  for(unsigned choices=0;choices<6561;++choices) {
    unsigned n=choices,index=0;ArRegionalRules request={0},active={0};
    for(unsigned i=0;i<4;++i) {
      request.platform_skull.source[i]=n%3;n/=3;active.platform_skull.source[i]=n%3;n/=3;
      index|=(request.platform_skull.source[i]==1?1u:0u)<<(2*i);
      index|=(active.platform_skull.source[i]==1?1u:0u)<<(2*i+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(skull_seen[index])CHECK(!memcmp(platform_skull_digests[index],current,32));
    else {skull_seen[index]=true;memcpy(platform_skull_digests[index],current,32);}
  }
  CHECK(!memcmp(native,platform_skull_digests[0],32));
  qsort(platform_skull_digests,256,32,CompareDigest);
  for(unsigned i=1;i<256;++i)CHECK(memcmp(platform_skull_digests[i-1],platform_skull_digests[i],32));
  uint8_t collision_digests[16][32];bool collision_seen[16]={false};
  for(unsigned choices=0;choices<81;++choices) {
    unsigned n=choices,index=0;ArRegionalRules request={0},active={0};
    for(unsigned i=0;i<2;++i) {
      request.collision.source[i]=n%3;n/=3;active.collision.source[i]=n%3;n/=3;
      index|=(request.collision.source[i]==1?1u:0u)<<(2*i);
      index|=(active.collision.source[i]==1?1u:0u)<<(2*i+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(collision_seen[index])CHECK(!memcmp(collision_digests[index],current,32));
    else {collision_seen[index]=true;memcpy(collision_digests[index],current,32);}
  }
  CHECK(!memcmp(native,collision_digests[0],32));
  qsort(collision_digests,16,32,CompareDigest);
  for(unsigned i=1;i<16;++i)CHECK(memcmp(collision_digests[i-1],collision_digests[i],32));
  static uint8_t boss_digests[4096][32];
  for(unsigned bits=0;bits<4096;++bits) {
    ArRegionalRules request={0},active={0};
    for(unsigned i=0;i<6;++i) {
      const unsigned alternate=(i==2 || i==3)?2:1;
      request.bosses.source[i]=(bits&(1u<<(2*i)))?alternate:0;
      active.bosses.source[i]=(bits&(1u<<(2*i+1)))?alternate:0;
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,boss_digests[bits],&baseline) && baseline==!bits);
    for(unsigned i=0;i<6;++i) {
      const unsigned alias=(i==2 || i==3)?1:2;
      if(!request.bosses.source[i])request.bosses.source[i]=alias;
      if(!active.bosses.source[i])active.bosses.source[i]=alias;
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && !memcmp(current,boss_digests[bits],32));
  }
  CHECK(!memcmp(native,boss_digests[0],32));qsort(boss_digests,4096,32,CompareDigest);
  for(unsigned i=1;i<4096;++i)CHECK(memcmp(boss_digests[i-1],boss_digests[i],32));
  uint8_t ice_digests[4][32];
  for(unsigned mask=0;mask<4;++mask) {
    ArRegionalRules request={0},active={0};request.bosses.source[6]=mask&1;active.bosses.source[6]=(mask>>1)&1;
    CHECK(ArRegionalRules_Fingerprint(&request,&active,ice_digests[mask],&baseline) && baseline==!mask);
    if(mask)CHECK(!bsearch(ice_digests[mask],boss_digests,4096,32,CompareDigest));
    if(!(mask&1))request.bosses.source[6]=2;
    if(!(mask&2))active.bosses.source[6]=2;
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && !memcmp(current,ice_digests[mask],32));
    for(unsigned i=0;i<mask;++i)CHECK(memcmp(ice_digests[i],current,32));
  }
  uint8_t volley_digests[4][32];bool volley_seen[4]={false};
  uint8_t antlion_digests[16][32];bool antlion_seen[16]={false};
  uint8_t dragon_digests[16][32];bool dragon_seen[16]={false};
  uint8_t viper_digests[576][32];bool viper_seen[576]={false};
  for(unsigned n=0;n<6561;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=15;i<19;++i) {
      request.bosses.source[i]=digits%3;digits/=3;active.bosses.source[i]=digits%3;digits/=3;
      if(i==15)index=request.bosses.source[i]*3+active.bosses.source[i];
      else index=index*4+(request.bosses.source[i]==2?2u:0u)+(active.bosses.source[i]==2?1u:0u);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(viper_seen[index])CHECK(!memcmp(viper_digests[index],current,32));
    else {viper_seen[index]=true;memcpy(viper_digests[index],current,32);}
  }
  CHECK(!memcmp(native,viper_digests[0],32));
  for(unsigned i=0;i<576;++i)CHECK(viper_seen[i]);
  qsort(viper_digests,576,32,CompareDigest);
  for(unsigned i=1;i<576;++i)CHECK(memcmp(viper_digests[i-1],viper_digests[i],32));
  uint8_t pharaoh_digests[64][32];bool pharaoh_seen[64]={false};
  for(unsigned n=0;n<729;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=19;i<22;++i) {
      request.bosses.source[i]=digits%3;digits/=3;active.bosses.source[i]=digits%3;digits/=3;
      index|=(request.bosses.source[i]==1?1u:0u)<<(2*(i-19));
      index|=(active.bosses.source[i]==1?1u:0u)<<(2*(i-19)+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(pharaoh_seen[index])CHECK(!memcmp(pharaoh_digests[index],current,32));
    else {pharaoh_seen[index]=true;memcpy(pharaoh_digests[index],current,32);}
    if(index)CHECK(!bsearch(current,viper_digests,576,32,CompareDigest) && !bsearch(current,boss_digests,4096,32,CompareDigest));
  }
  CHECK(!memcmp(native,pharaoh_digests[0],32));
  for(unsigned i=0;i<64;++i)CHECK(pharaoh_seen[i]);
  qsort(pharaoh_digests,64,32,CompareDigest);
  for(unsigned i=1;i<64;++i)CHECK(memcmp(pharaoh_digests[i-1],pharaoh_digests[i],32));
  uint8_t plant_digests[256][32];bool plant_seen[256]={false};
  for(unsigned n=0;n<6561;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=22;i<26;++i) {
      request.bosses.source[i]=digits%3;digits/=3;active.bosses.source[i]=digits%3;digits/=3;
      const bool req=i==22?request.bosses.source[i]!=0:request.bosses.source[i]==2;
      const bool eff=i==22?active.bosses.source[i]!=0:active.bosses.source[i]==2;
      index|=(unsigned)req<<(2*(i-22));index|=(unsigned)eff<<(2*(i-22)+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(plant_seen[index])CHECK(!memcmp(plant_digests[index],current,32));
    else {plant_seen[index]=true;memcpy(plant_digests[index],current,32);}
    if(index)CHECK(!bsearch(current,pharaoh_digests,64,32,CompareDigest));
  }
  CHECK(!memcmp(native,plant_digests[0],32));
  for(unsigned i=0;i<256;++i)CHECK(plant_seen[i]);
  qsort(plant_digests,256,32,CompareDigest);
  for(unsigned i=1;i<256;++i)CHECK(memcmp(plant_digests[i-1],plant_digests[i],32));
  uint8_t northwall_digests[256][32];bool northwall_seen[256]={false};
  for(unsigned n=0;n<6561;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=26;i<30;++i) {
      request.bosses.source[i]=digits%3;digits/=3;active.bosses.source[i]=digits%3;digits/=3;
      index|=(unsigned)(request.bosses.source[i]==2)<<(2*(i-26));
      index|=(unsigned)(active.bosses.source[i]==2)<<(2*(i-26)+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(northwall_seen[index])CHECK(!memcmp(northwall_digests[index],current,32));
    else {northwall_seen[index]=true;memcpy(northwall_digests[index],current,32);}
    if(index)CHECK(!bsearch(current,plant_digests,256,32,CompareDigest));
  }
  CHECK(!memcmp(native,northwall_digests[0],32));
  for(unsigned i=0;i<256;++i)CHECK(northwall_seen[i]);
  qsort(northwall_digests,256,32,CompareDigest);
  for(unsigned i=1;i<256;++i)CHECK(memcmp(northwall_digests[i-1],northwall_digests[i],32));
  uint8_t geometry_digests[4][32];bool geometry_seen[4]={false};
  for(unsigned req=0;req<3;++req)for(unsigned eff=0;eff<3;++eff) {
    ArRegionalRules request={0},active={0};request.bosses.source[30]=req;active.bosses.source[30]=eff;
    const unsigned index=(req==1?1u:0u)|(eff==1?2u:0u);
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(geometry_seen[index])CHECK(!memcmp(geometry_digests[index],current,32));
    else {geometry_seen[index]=true;memcpy(geometry_digests[index],current,32);}
    if(index)CHECK(!bsearch(current,northwall_digests,256,32,CompareDigest));
  }
  CHECK(!memcmp(native,geometry_digests[0],32));
  for(unsigned i=0;i<4;++i)CHECK(geometry_seen[i]);
  qsort(geometry_digests,4,32,CompareDigest);
  for(unsigned i=1;i<4;++i)CHECK(memcmp(geometry_digests[i-1],geometry_digests[i],32));
  for(unsigned n=0;n<81;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=13;i<15;++i) {
      request.bosses.source[i]=digits%3;digits/=3;active.bosses.source[i]=digits%3;digits/=3;
      index|=(request.bosses.source[i]==2?1u:0u)<<(2*(i-13));
      index|=(active.bosses.source[i]==2?1u:0u)<<(2*(i-13)+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(dragon_seen[index])CHECK(!memcmp(dragon_digests[index],current,32));
    else {dragon_seen[index]=true;memcpy(dragon_digests[index],current,32);}
  }
  CHECK(!memcmp(native,dragon_digests[0],32));
  qsort(dragon_digests,16,32,CompareDigest);
  for(unsigned i=1;i<16;++i)CHECK(memcmp(dragon_digests[i-1],dragon_digests[i],32));
  for(unsigned n=0;n<81;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=11;i<13;++i) {
      request.bosses.source[i]=digits%3;digits/=3;active.bosses.source[i]=digits%3;digits/=3;
      index|=(request.bosses.source[i]==1?1u:0u)<<(2*(i-11));
      index|=(active.bosses.source[i]==1?1u:0u)<<(2*(i-11)+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(antlion_seen[index])CHECK(!memcmp(antlion_digests[index],current,32));
    else {antlion_seen[index]=true;memcpy(antlion_digests[index],current,32);}
  }
  CHECK(!memcmp(native,antlion_digests[0],32));
  qsort(antlion_digests,16,32,CompareDigest);
  for(unsigned i=1;i<16;++i)CHECK(memcmp(antlion_digests[i-1],antlion_digests[i],32));
  uint8_t tanzra_digests[256][32];bool tanzra_seen[256]={false};
  for(unsigned n=0;n<6561;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=7;i<11;++i) {
      request.bosses.source[i]=digits%3;digits/=3;active.bosses.source[i]=digits%3;digits/=3;
      const unsigned alternate=i==10?2:1;
      index|=(request.bosses.source[i]==alternate?1u:0u)<<(2*(i-7));
      index|=(active.bosses.source[i]==alternate?1u:0u)<<(2*(i-7)+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(tanzra_seen[index])CHECK(!memcmp(tanzra_digests[index],current,32));
    else {tanzra_seen[index]=true;memcpy(tanzra_digests[index],current,32);}
  }
  CHECK(!memcmp(native,tanzra_digests[0],32));
  qsort(tanzra_digests,256,32,CompareDigest);
  for(unsigned i=1;i<256;++i)CHECK(memcmp(tanzra_digests[i-1],tanzra_digests[i],32));
  for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b) {
    ArRegionalRules request={.statue_volley=a},active={.statue_volley=b};
    const unsigned index=(a!=0)|((b!=0)<<1);
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(volley_seen[index])CHECK(!memcmp(current,volley_digests[index],32));
    else {volley_seen[index]=true;memcpy(volley_digests[index],current,32);}
  }
  CHECK(!memcmp(volley_digests[0],native,32));
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)CHECK(memcmp(volley_digests[i],volley_digests[j],32));
  ArRegionalRules invalid_volley={.statue_volley=3};
  CHECK(!ArRegionalRules_Fingerprint(&invalid_volley,&e,current,&baseline));
  uint8_t emitter_digests[36][32];bool emitter_seen[36]={false};
  for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b)for(unsigned c=0;c<3;++c)for(unsigned d=0;d<3;++d) {
    ArRegionalRules request={.emitters={{a,b}}},active={.emitters={{c,d}}};
    const unsigned index=a+3*(b==2)+6*(c+3*(d==2));
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    if(emitter_seen[index])CHECK(!memcmp(current,emitter_digests[index],32));
    else {emitter_seen[index]=true;memcpy(emitter_digests[index],current,32);}
  }
  CHECK(!memcmp(emitter_digests[0],native,32));
  qsort(emitter_digests,36,32,CompareDigest);
  for(unsigned n=1;n<36;++n)CHECK(memcmp(emitter_digests[n-1],emitter_digests[n],32));
  static uint8_t motion_digests[16384][32];
  for(unsigned n=0;n<16384;++n) {
    ArRegionalRules request={0},active={0};
    for(unsigned i=0;i<7;++i){request.action_motion.source[i]=(n>>(2*i))&1;active.action_motion.source[i]=(n>>(2*i+1))&1;}
    CHECK(ArRegionalRules_Fingerprint(&request,&active,motion_digests[n],&baseline) && baseline==!n);
    for(unsigned i=0;i<7;++i){if(!request.action_motion.source[i])request.action_motion.source[i]=2;if(!active.action_motion.source[i])active.action_motion.source[i]=2;}
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && !memcmp(current,motion_digests[n],32));
  }
  CHECK(!memcmp(native,motion_digests[0],32));
  qsort(motion_digests,16384,32,CompareDigest);
  for(unsigned i=1;i<16384;++i)CHECK(memcmp(motion_digests[i-1],motion_digests[i],32));
  uint8_t sword_digests[1024][32];
  for(unsigned n=0;n<1024;++n) {
    ArRegionalRules request={0},active={0};
    for(unsigned i=0;i<5;++i){request.action_motion.source[7+i]=(n>>(2*i))&1;active.action_motion.source[7+i]=(n>>(2*i+1))&1;}
    CHECK(ArRegionalRules_Fingerprint(&request,&active,sword_digests[n],&baseline) && baseline==!n);
    if(n)CHECK(!bsearch(sword_digests[n],motion_digests,16384,32,CompareDigest));
    for(unsigned i=0;i<5;++i){if(!request.action_motion.source[7+i])request.action_motion.source[7+i]=2;if(!active.action_motion.source[7+i])active.action_motion.source[7+i]=2;}
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && !memcmp(current,sword_digests[n],32));
    for(unsigned j=0;j<n;++j)CHECK(memcmp(sword_digests[j],current,32));
  }
  ArRegionalRules invalid_motion={0};invalid_motion.action_motion.source[3]=kArRegionalSource_Count;
  uint8_t head_digests[4][32];bool head_seen[4]={false};
  for(unsigned requested=0;requested<3;++requested)for(unsigned effective=0;effective<3;++effective) {
    ArRegionalRules req={0},eff={0};req.action_motion.source[12]=requested;eff.action_motion.source[12]=effective;
    const unsigned index=(requested==1?2:0)+(effective==1?1:0);
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!index);
    if(head_seen[index])CHECK(!memcmp(current,head_digests[index],32));
    else {head_seen[index]=true;memcpy(head_digests[index],current,32);}
    if(index)CHECK(!bsearch(current,motion_digests,16384,32,CompareDigest));
  }
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)CHECK(memcmp(head_digests[i],head_digests[j],32));
  memcpy(current,native,32);baseline=true;
  uint8_t tree_digests[4][32]={{0}};bool tree_seen[4]={0};
  for(unsigned requested=0;requested<3;++requested)for(unsigned effective=0;effective<3;++effective) {
    ArRegionalRules req={0},eff={0};
    req.action_motion.source[kArRegionalActionMotion_TreeSeeds]=requested;
    eff.action_motion.source[kArRegionalActionMotion_TreeSeeds]=effective;
    const unsigned index=(requested!=0)*2+(effective!=0);
    CHECK(ArRegionalRules_Fingerprint(&req,&eff,current,&baseline) && baseline==!index);
    if(tree_seen[index])CHECK(!memcmp(current,tree_digests[index],32));
    else {memcpy(tree_digests[index],current,32);tree_seen[index]=true;}
    if(index)CHECK(!bsearch(current,motion_digests,16384,32,CompareDigest));
  }
  qsort(tree_digests,4,32,CompareDigest);
  for(unsigned i=1;i<4;++i)CHECK(memcmp(tree_digests[i-1],tree_digests[i],32));
  memcpy(current,native,32);baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&invalid_motion,&e,current,&baseline) && baseline && !memcmp(current,native,32));
  uint8_t support_digests[1024][32];
  for(unsigned index=0;index<1024;++index) {
    ArRegionalRules request={0},active={0};
    for(unsigned i=0;i<5;++i) {
      request.support.source[i]=(index>>(2*i))&1;
      active.support.source[i]=(index>>(2*i+1))&1;
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,support_digests[index],&baseline) && baseline==!index);
    CHECK((memcmp(support_digests[index],native,32)==0)==!index);
    for(unsigned i=0;i<5;++i) {
      if(!request.support.source[i])request.support.source[i]=2;
      if(!active.support.source[i])active.support.source[i]=2;
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && !memcmp(current,support_digests[index],32));
    for(unsigned j=0;j<index;++j)CHECK(memcmp(support_digests[j],current,32));
  }
  uint8_t construction_digests[4][32];bool construction_seen[4]={false};
  for(unsigned p=0;p<3;++p)for(unsigned a=0;a<3;++a) {
    ArRegionalRules request={.construction=p},active={.construction=a};
    unsigned index=(p==1)|((a==1)<<1);
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && baseline==!index);
    CHECK((memcmp(current,native,32)==0)==!index);
    if(construction_seen[index])CHECK(!memcmp(current,construction_digests[index],32));
    else { memcpy(construction_digests[index],current,32);construction_seen[index]=true; }
  }
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j)CHECK(memcmp(construction_digests[i],construction_digests[j],32));
  uint8_t ai_digests[4096][32];
  for(unsigned index=0;index<4096;++index) {
    ArRegionalRules request={0},active={0};
    for(unsigned i=0;i<6;++i) {
      request.sim_ai.source[i]=(index>>(2*i))&1;
      active.sim_ai.source[i]=(index>>(2*i+1))&1;
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,ai_digests[index],&baseline) && baseline==!index);
    for(unsigned i=0;i<6;++i) {
      if(!request.sim_ai.source[i])request.sim_ai.source[i]=kArRegionalSource_Europe;
      if(!active.sim_ai.source[i])active.sim_ai.source[i]=kArRegionalSource_Europe;
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline) && !memcmp(current,ai_digests[index],32));
    for(unsigned j=0;j<index;++j)CHECK(memcmp(ai_digests[j],current,32));
  }
  uint8_t combat_digests[1024][32];bool combat_seen[1024]={false};
  for(unsigned n=0;n<59049;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=0;i<5;++i) {
      request.sim_combat.source[i]=(ArRegionalSource)(digits%3);digits/=3;
      active.sim_combat.source[i]=(ArRegionalSource)(digits%3);digits/=3;
      if(request.sim_combat.source[i]==1) index|=1u<<(2*i);
      if(active.sim_combat.source[i]==1) index|=1u<<(2*i+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline));
    CHECK(baseline==!index && (memcmp(current,native,32)==0)==!index);
    if(combat_seen[index]) CHECK(!memcmp(combat_digests[index],current,32));
    else { memcpy(combat_digests[index],current,32);combat_seen[index]=true; }
  }
  for(unsigned i=0;i<1024;++i) for(unsigned j=0;j<i;++j) CHECK(memcmp(combat_digests[i],combat_digests[j],32));
  ArRegionalSimActors actors={0};
  CHECK(ArRegionalSimActors_Fingerprint(native,&actors,current,&baseline) && baseline && !memcmp(native,current,32));
  /* A v24 combat-only recording retains its byte-for-byte hash domain. */
  uint8_t old_bytes[116]="ARSIMACTOR-R1",old_hash[32];
  memcpy(old_bytes+16,native,32);memcpy(old_bytes+48,"ARSIMAC1",8);old_bytes[60]=7;
  actors.cached[0].combat=7;
  CHECK(sr_support_sha256(old_bytes,sizeof(old_bytes),old_hash));
  CHECK(ArRegionalSimActors_Fingerprint(native,&actors,current,&baseline) && !baseline && !memcmp(old_hash,current,32));
  actors.cached[0].combat=0;
  for(unsigned town=0;town<6;++town)for(unsigned slot=0;slot<4;++slot)for(unsigned bit=0;bit<6;++bit) {
    actors=(ArRegionalSimActors){0};actors.cached[town*4+slot].ai=1u<<bit;
    CHECK(ArRegionalSimActors_Fingerprint(native,&actors,current,&baseline) && !baseline && memcmp(native,current,32));
    CHECK(ArRegionalSimActors_LoadTown(&actors,town));
    CHECK(ArRegionalSimActors_Fingerprint(native,&actors,pending,&baseline) && !baseline && memcmp(current,pending,32));
    actors.cached[town*4+slot].ai=0;
    CHECK(ArRegionalSimActors_Fingerprint(native,&actors,effective,&baseline) && !baseline && memcmp(effective,pending,32));
    actors.active[slot].ai=0;
    CHECK(ArRegionalSimActors_Fingerprint(native,&actors,current,&baseline) && baseline && !memcmp(native,current,32));
  }
  actors=(ArRegionalSimActors){0};
  CHECK(ArRegionalSimActors_LoadTown(&actors,0));
  CHECK(ArRegionalSimActors_Fingerprint(native,&actors,current,&baseline) && baseline && !memcmp(native,current,32));
  for(unsigned town=0;town<6;++town)for(unsigned slot=0;slot<4;++slot)for(unsigned bit=0;bit<5;++bit) {
    actors=(ArRegionalSimActors){0};actors.cached[town*4+slot].combat=(uint16_t)(1u<<bit);
    CHECK(ArRegionalSimActors_Fingerprint(native,&actors,current,&baseline) && !baseline && memcmp(native,current,32));
    CHECK(ArRegionalSimActors_LoadTown(&actors,town));
    CHECK(ArRegionalSimActors_Fingerprint(native,&actors,pending,&baseline) && !baseline && memcmp(pending,current,32));
    actors.cached[town*4+slot].combat=0;
    CHECK(ArRegionalSimActors_Fingerprint(native,&actors,effective,&baseline) && !baseline && memcmp(pending,effective,32));
    actors.active[slot].combat=0;
    CHECK(ArRegionalSimActors_Fingerprint(native,&actors,current,&baseline) && baseline && !memcmp(native,current,32));
  }
  actors.active[0].combat=0x80;memset(current,0xa5,sizeof(current));
  CHECK(!ArRegionalSimActors_Fingerprint(native,&actors,current,&baseline) && current[0]==0xa5);
  uint8_t level_digests[4][32];bool level_seen[4]={false};
  for(unsigned p=0;p<3;++p)for(unsigned a=0;a<3;++a) {
    ArRegionalRules request={.level_goals=(ArRegionalSource)p},active={.level_goals=(ArRegionalSource)a};
    unsigned index=(p==1?1:0)|(a==1?2:0);
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline));
    CHECK(baseline==!index && (memcmp(current,native,32)==0)==!index);
    if(level_seen[index]) CHECK(!memcmp(level_digests[index],current,32));
    else { memcpy(level_digests[index],current,32);level_seen[index]=true; }
  }
  for(unsigned i=0;i<4;++i)for(unsigned j=0;j<i;++j) CHECK(memcmp(level_digests[i],level_digests[j],32));
  ArRegionalRules invalid_level={.level_goals=3};memset(current,0xa5,sizeof(current));
  CHECK(!ArRegionalRules_Fingerprint(&invalid_level,&e,current,&baseline) && current[0]==0xa5);
  uint8_t status_digests[1024][32]; bool status_seen[1024]={false};
  for(unsigned n=0;n<59049;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=0;i<5;++i) {
      request.town_status.source[i]=(ArRegionalSource)(digits%3);digits/=3;
      active.town_status.source[i]=(ArRegionalSource)(digits%3);digits/=3;
      if(request.town_status.source[i]==1) index|=1u<<(2*i);
      if(active.town_status.source[i]==1) index|=1u<<(2*i+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline));
    CHECK(baseline==!index && (memcmp(current,native,32)==0)==!index);
    if(status_seen[index]) CHECK(!memcmp(status_digests[index],current,32));
    else { memcpy(status_digests[index],current,32);status_seen[index]=true; }
  }
  for(unsigned i=0;i<1024;++i) for(unsigned j=0;j<i;++j) CHECK(memcmp(status_digests[i],status_digests[j],32));
  ArRegionalRules invalid_status={.town_status={{0,0,3,0,0}}};memset(current,0xa5,sizeof(current));
  CHECK(!ArRegionalRules_Fingerprint(&invalid_status,&e,current,&baseline) && current[0]==0xa5);
  uint8_t story_digests[64][32];bool story_seen[64]={false};
  for(unsigned n=0;n<729;++n) {
    ArRegionalRules request={0},active={0};unsigned digits=n,index=0;
    for(unsigned i=0;i<3;++i) {
      request.story.source[i]=(ArRegionalSource)(digits%3);digits/=3;
      active.story.source[i]=(ArRegionalSource)(digits%3);digits/=3;
      if(request.story.source[i]==1)index|=1u<<(2*i);
      if(active.story.source[i]==1)index|=1u<<(2*i+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline));
    CHECK(baseline==!index && (memcmp(current,native,32)==0)==!index);
    if(story_seen[index])CHECK(!memcmp(story_digests[index],current,32));
    else {memcpy(story_digests[index],current,32);story_seen[index]=true;}
  }
  for(unsigned i=0;i<64;++i)for(unsigned j=i+1;j<64;++j)CHECK(memcmp(story_digests[i],story_digests[j],32));
  ArRegionalRules invalid_story={.story={{0,3,0}}};memset(current,0xa5,sizeof(current));
  CHECK(!ArRegionalRules_Fingerprint(&invalid_story,&e,current,&baseline) && current[0]==0xa5);
  uint8_t skull_digests[4][32]; bool seen_skull[4]={false};
  for(unsigned p=0;p<3;++p)for(unsigned a=0;a<3;++a) {
    ArRegionalRules requested={.skull_wait=(ArRegionalSource)p}, active={.skull_wait=(ArRegionalSource)a};
    const unsigned index=(p==1?1:0)|(a==1?2:0);
    CHECK(ArRegionalRules_Fingerprint(&requested,&active,current,&baseline));
    CHECK(baseline==!index && (memcmp(current,native,32)==0)==!index);
    if(seen_skull[index])CHECK(!memcmp(skull_digests[index],current,32));
    else {memcpy(skull_digests[index],current,32);seen_skull[index]=true;}
  }
  for(unsigned i=0;i<4;++i)for(unsigned j=i+1;j<4;++j)CHECK(memcmp(skull_digests[i],skull_digests[j],32));
  ArRegionalRules invalid_skull={.skull_wait=kArRegionalSource_Count};
  memset(current,0xa5,sizeof(current));
  CHECK(!ArRegionalRules_Fingerprint(&invalid_skull,&e,current,&baseline) && current[0]==0xa5);
  uint8_t source_digests[16][32]; bool seen_sources[16]={false};
  for(unsigned n=0;n<81;++n) {
    ArRegionalRules request={0}, active={0}; unsigned digits=n,index=0;
    for(unsigned i=0;i<2;++i) {
      request.sources.source[i]=(ArRegionalSource)(digits%3); digits/=3;
      active.sources.source[i]=(ArRegionalSource)(digits%3); digits/=3;
      if(request.sources.source[i]==kArRegionalSource_Japan)index|=1u<<(2*i);
      if(active.sources.source[i]==kArRegionalSource_Japan)index|=1u<<(2*i+1);
    }
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline));
    CHECK(baseline==!index && (memcmp(current,native,32)==0)==!index);
    if(seen_sources[index])CHECK(!memcmp(source_digests[index],current,32));
    else { memcpy(source_digests[index],current,32);seen_sources[index]=true; }
  }
  for(unsigned i=0;i<16;++i)for(unsigned j=i+1;j<16;++j)
    CHECK(memcmp(source_digests[i],source_digests[j],32));
  CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && baseline);
  CHECK(!memcmp(native, current, 32));
  r.retry_score = kArRegionalSource_Japan;
  for (unsigned p=0;p<3;++p) for (unsigned a=0;a<3;++a) {
    ArRegionalRules request = {.lives_display=(ArRegionalSource)p};
    ArRegionalRules active = {.lives_display=(ArRegionalSource)a};
    CHECK(ArRegionalRules_Fingerprint(&request,&active,current,&baseline));
    CHECK(baseline==(p!=kArRegionalSource_Japan && a!=kArRegionalSource_Japan));
    CHECK((memcmp(native,current,32)==0)==baseline);
  }
  ArRegionalRules life_request={.lives_display=kArRegionalSource_Japan}, life_active={0};
  CHECK(ArRegionalRules_Fingerprint(&life_request,&life_active,pending,&baseline));
  CHECK(ArRegionalRules_Fingerprint(&life_active,&life_request,effective,&baseline));
  CHECK(memcmp(pending,effective,32));
  life_request.lives_display=kArRegionalSource_Count; memset(current,0xa5,sizeof(current));
  CHECK(!ArRegionalRules_Fingerprint(&life_request,&life_active,current,&baseline) && current[0]==0xa5);
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline) && !baseline);
  CHECK(memcmp(native, pending, 32));
  r.retry_score = kArRegionalSource_Europe; e.retry_score = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && !baseline);
  CHECK(memcmp(pending, effective, 32));
  e.retry_score = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && baseline);
  CHECK(!memcmp(native, current, 32));
  r.retry_score = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current));
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  r.retry_score = kArRegionalSource_US;
  for (unsigned p = 0; p < kArRegionalSource_Count; ++p)
    for (unsigned a = 0; a < kArRegionalSource_Count; ++a) {
      r.town_wait = (ArRegionalSource)p; e.town_wait = (ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline));
      CHECK(baseline == (p != kArRegionalSource_Japan && a != kArRegionalSource_Japan));
      CHECK((memcmp(native, current, 32) == 0) == baseline);
    }
  r.town_wait = kArRegionalSource_Japan; e.town_wait = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline));
  r.town_wait = kArRegionalSource_US; e.town_wait = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline));
  CHECK(memcmp(pending,effective,32));
  r.town_wait = kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current)); baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0] == 0xa5 && baseline);
  r.town_wait=e.town_wait=kArRegionalSource_US;
  for (unsigned p=0; p<kArRegionalSource_Count; ++p)
    for (unsigned a=0; a<kArRegionalSource_Count; ++a) {
      r.fishing=(ArRegionalSource)p; e.fishing=(ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r,&e,current,&baseline));
      CHECK(baseline == (p!=kArRegionalSource_Japan && a!=kArRegionalSource_Japan));
      CHECK((memcmp(native,current,32)==0) == baseline);
    }
  r.fishing=kArRegionalSource_Japan; e.fishing=kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r,&e,pending,&baseline));
  r.fishing=kArRegionalSource_US; e.fishing=kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r,&e,effective,&baseline));
  CHECK(memcmp(pending,effective,32));
  r.fishing=kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current)); baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  ArRegionalDevelopment_Init(&r.development, kArRegionalSource_US);
  ArRegionalDevelopment_Init(&e.development, kArRegionalSource_US);
  r.fishing = e.fishing = kArRegionalSource_US;
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i) {
    ArRegionalRecovery_Init(&r.recovery, kArRegionalSource_US);
    ArRegionalRecovery_Init(&e.recovery, kArRegionalSource_Europe);
    CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && baseline && !memcmp(native, current, 32));
    r.recovery.source[i] = kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline) && !baseline);
    r.recovery.source[i] = kArRegionalSource_US; e.recovery.source[i] = kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && !baseline);
    CHECK(memcmp(pending, effective, 32) && memcmp(native, effective, 32));
  }
  r.recovery.source[0] = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  ArRegionalRecovery_Init(&r.recovery, kArRegionalSource_US);
  ArRegionalRecovery_Init(&e.recovery, kArRegionalSource_US);
  r.fishing=e.fishing=kArRegionalSource_US;
  for(unsigned i=0;i<kArRegionalDevelopmentRule_Count;++i) {
    ArRegionalDevelopment_Init(&r.development,kArRegionalSource_US);
    ArRegionalDevelopment_Init(&e.development,kArRegionalSource_Europe);
    CHECK(ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && baseline && !memcmp(native,current,32));
    r.development.source[i]=kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r,&e,pending,&baseline) && !baseline);
    r.development.source[i]=kArRegionalSource_US;e.development.source[i]=kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r,&e,effective,&baseline) && !baseline);
    CHECK(memcmp(pending,effective,32) && memcmp(native,effective,32));
  }
  r.development.source[0]=kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current));baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  ArRegionalDevelopment_Init(&r.development, kArRegionalSource_US);
  ArRegionalDevelopment_Init(&e.development, kArRegionalSource_US);
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i) {
    ArRegionalQuake_Init(&r.quake, kArRegionalSource_US);
    ArRegionalQuake_Init(&e.quake, kArRegionalSource_Europe);
    CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && baseline && !memcmp(native, current, 32));
    r.quake.source[i] = kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline) && !baseline);
    r.quake.source[i] = kArRegionalSource_US; e.quake.source[i] = kArRegionalSource_Japan;
    CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && !baseline);
    CHECK(memcmp(pending, effective, 32) && memcmp(native, effective, 32));
  }
  r.quake.source[0] = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  ArRegionalQuake_Init(&r.quake, kArRegionalSource_US);
  ArRegionalQuake_Init(&e.quake, kArRegionalSource_US);
  for (unsigned p = 0; p < kArRegionalSource_Count; ++p)
    for (unsigned a = 0; a < kArRegionalSource_Count; ++a) {
      r.score_page = (ArRegionalSource)p; e.score_page = (ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline));
      CHECK(baseline == (p != kArRegionalSource_Japan && a != kArRegionalSource_Japan));
      CHECK((memcmp(native, current, 32) == 0) == baseline);
    }
  r.score_page = kArRegionalSource_Japan; e.score_page = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline));
  r.score_page = kArRegionalSource_US; e.score_page = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && memcmp(pending, effective, 32));
  r.score_page = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  r.score_page = e.score_page = kArRegionalSource_US;
  for(unsigned p=0;p<kArRegionalSource_Count;++p) for(unsigned a=0;a<kArRegionalSource_Count;++a) {
    r.lair_seeds=(ArRegionalSource)p; e.lair_seeds=(ArRegionalSource)a;
    CHECK(ArRegionalRules_Fingerprint(&r,&e,current,&baseline));
    CHECK(baseline==(p!=kArRegionalSource_Japan && a!=kArRegionalSource_Japan));
    CHECK((!memcmp(native,current,32))==baseline);
  }
  r.lair_seeds=kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current)); baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  r.lair_seeds=e.lair_seeds=kArRegionalSource_US;
  for(unsigned p=0;p<kArRegionalSource_Count;++p) for(unsigned a=0;a<kArRegionalSource_Count;++a) {
    r.house_credit=(ArRegionalSource)p; e.house_credit=(ArRegionalSource)a;
    CHECK(ArRegionalRules_Fingerprint(&r,&e,current,&baseline));
    CHECK(baseline==(p!=kArRegionalSource_Japan && a!=kArRegionalSource_Japan));
    CHECK((!memcmp(native,current,32))==baseline);
  }
  r.house_credit=kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current)); baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  r.house_credit=e.house_credit=kArRegionalSource_US;
  uint8_t score_digests[256][32]; bool seen_score[256]={false};
  for(unsigned p=0;p<81;++p)for(unsigned a=0;a<81;++a) {
    unsigned pd=p,ad=a,bits=0;
    for(unsigned i=0;i<kArRegionalScore_Count;++i) {
      r.score_feedback.source[i]=(ArRegionalSource)(pd%3);pd/=3;
      e.score_feedback.source[i]=(ArRegionalSource)(ad%3);ad/=3;
      if(r.score_feedback.source[i]==kArRegionalSource_Japan)bits|=1u<<i;
      if(e.score_feedback.source[i]==kArRegionalSource_Japan)bits|=16u<<i;
    }
    CHECK(ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && baseline==!bits);
    CHECK((!memcmp(native,current,32))==baseline);
    if(seen_score[bits]) CHECK(!memcmp(score_digests[bits],current,32));
    else { memcpy(score_digests[bits],current,32);seen_score[bits]=true; }
  }
  for(unsigned i=0;i<256;++i) {
    CHECK(seen_score[i]);
    for(unsigned j=0;j<i;++j)CHECK(memcmp(score_digests[i],score_digests[j],32));
  }
  r.score_feedback.source[2]=kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current));baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  ArRegionalScore_Init(&r.score_feedback,kArRegionalSource_US);
  ArRegionalScore_Init(&e.score_feedback,kArRegionalSource_US);
  for (unsigned p = 0; p < kArRegionalSource_Count; ++p)
    for (unsigned a = 0; a < kArRegionalSource_Count; ++a) {
      r.menu_return = (ArRegionalSource)p; e.menu_return = (ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline));
      CHECK(baseline == (p != kArRegionalSource_Japan && a != kArRegionalSource_Japan));
      CHECK((memcmp(native, current, 32) == 0) == baseline);
    }
  r.menu_return = kArRegionalSource_Japan; e.menu_return = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline));
  r.menu_return = kArRegionalSource_US; e.menu_return = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && memcmp(pending, effective, 32));
  r.menu_return = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  r.menu_return = e.menu_return = kArRegionalSource_US;
  for (unsigned p = 0; p < kArRegionalSource_Count; ++p)
    for (unsigned a = 0; a < kArRegionalSource_Count; ++a) {
      r.speed_range = (ArRegionalSource)p; e.speed_range = (ArRegionalSource)a;
      CHECK(ArRegionalRules_Fingerprint(&r, &e, current, &baseline));
      CHECK(baseline == (p != kArRegionalSource_Japan && a != kArRegionalSource_Japan));
      CHECK((memcmp(native, current, 32) == 0) == baseline);
    }
  r.speed_range = kArRegionalSource_Japan; e.speed_range = kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, pending, &baseline));
  r.speed_range = kArRegionalSource_US; e.speed_range = kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r, &e, effective, &baseline) && memcmp(pending, effective, 32));
  r.speed_range = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!ArRegionalRules_Fingerprint(&r, &e, current, &baseline) && current[0] == 0xa5 && baseline);
  r.speed_range=e.speed_range=kArRegionalSource_US;
  for (unsigned p=0;p<kArRegionalSource_Count;++p) for (unsigned a=0;a<kArRegionalSource_Count;++a) {
    r.magic_gesture=(ArRegionalSource)p; e.magic_gesture=(ArRegionalSource)a;
    CHECK(ArRegionalRules_Fingerprint(&r,&e,current,&baseline));
    CHECK(baseline==(p!=kArRegionalSource_Japan && a!=kArRegionalSource_Japan));
    CHECK((memcmp(native,current,32)==0)==baseline);
  }
  r.magic_gesture=kArRegionalSource_Japan; e.magic_gesture=kArRegionalSource_US;
  CHECK(ArRegionalRules_Fingerprint(&r,&e,pending,&baseline));
  r.magic_gesture=kArRegionalSource_US; e.magic_gesture=kArRegionalSource_Japan;
  CHECK(ArRegionalRules_Fingerprint(&r,&e,effective,&baseline) && memcmp(pending,effective,32));
  r.magic_gesture=kArRegionalSource_Count;
  memset(current,0xa5,sizeof(current)); baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  CHECK(ArRegionalCosts_Fingerprint(&jp, &us, pending, &baseline) && !baseline);
  CHECK(ArRegionalCosts_Fingerprint(&us, &jp, effective, &baseline) && !baseline);
  CHECK(memcmp(native, pending, 32) && memcmp(native, effective, 32) && memcmp(pending, effective, 32));
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    ArRegionalCostPolicy one = us;
    ArRegionalCosts_SetRule(&one, (ArRegionalCostRule)i, kArRegionalSource_Japan);
    CHECK(ArRegionalCosts_Fingerprint(&one, &us, current, &baseline));
    bool same_price = ArRegionalCosts_Descriptor((ArRegionalCostRule)i)->price[0] ==
                      ArRegionalCosts_Descriptor((ArRegionalCostRule)i)->price[1];
    CHECK(baseline == same_price);
  CHECK((memcmp(native, current, 32) == 0) == same_price);
  }
  ArRegionalTimerPolicy timer_us, timer_eu, timer_jp;
  ArRegionalTimers_Init(&timer_us, kArRegionalSource_US);
  ArRegionalTimers_Init(&timer_eu, kArRegionalSource_Europe);
  ArRegionalTimers_Init(&timer_jp, kArRegionalSource_Japan);
  CHECK(Fingerprint(&jp, &us, &timer_us, &timer_eu, current, &baseline));
  CHECK(!baseline && !memcmp(current, pending, 32));
  CHECK(Fingerprint(&eu, &us, &timer_us, &timer_eu, current, &baseline));
  CHECK(baseline && !memcmp(current, native, 32));
  CHECK(Fingerprint(&us, &eu, &timer_jp, &timer_us, pending, &baseline));
  CHECK(!baseline && memcmp(pending, native, 32));
  CHECK(Fingerprint(&us, &us, &timer_us, &timer_jp, effective, &baseline));
  CHECK(!baseline && memcmp(effective, pending, 32) && memcmp(effective, native, 32));
  for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i) {
    ArRegionalTimerPolicy one = timer_us;
    CHECK(ArRegionalTimers_SetRule(&one, (ArRegionalTimerRule)i, kArRegionalSource_Japan));
    CHECK(Fingerprint(&us, &us, &one, &timer_us, current, &baseline));
    CHECK(!baseline && memcmp(current, native, 32));
  }
  timer_jp.source[0] = kArRegionalSource_Count;
  memset(current, 0xa5, sizeof(current)); baseline = true;
  CHECK(!Fingerprint(&us, &us, &timer_jp, &timer_us, current, &baseline));
  CHECK(current[0] == 0xa5 && baseline);
  memset(current, 0xa5, sizeof(current)); baseline = true;
  jp.source[0] = kArRegionalSource_Count;
  CHECK(!ArRegionalCosts_Fingerprint(&jp, &us, current, &baseline));
  CHECK(current[0] == 0xa5 && baseline);
  for(unsigned req=0;req<3;++req)for(unsigned eff=0;eff<3;++eff) {
    ArRegionalRules a={0},b={0};a.arrival=req;b.arrival=eff;
    uint8_t first[32],second[32],policy[32];
    CHECK(ArRegionalRules_Fingerprint(&a,&b,policy,&baseline) && baseline==(req!=1 && eff!=1));
    CHECK(ArRegionalArrivalLock_Fingerprint(policy,req,eff,false,first));
    CHECK(ArRegionalArrivalLock_Fingerprint(policy,req,eff,true,second));
    CHECK((memcmp(first,second,32)!=0)==(req==1 || eff==1));
    if(baseline)CHECK(!memcmp(policy,native,32) && !memcmp(first,native,32));
  }
  r=(ArRegionalRules){.arrival=3};e=(ArRegionalRules){0};
  memset(current,0xa5,32);baseline=true;
  CHECK(!ArRegionalRules_Fingerprint(&r,&e,current,&baseline) && current[0]==0xa5 && baseline);
  return failures ? 1 : 0;
}
