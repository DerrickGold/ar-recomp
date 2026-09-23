#include "regional/regional_fingerprint.h"
#include "snesrecomp/support/digest.h"
#include <stdio.h>
#include <string.h>
static int failures;
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
  return failures ? 1 : 0;
}
