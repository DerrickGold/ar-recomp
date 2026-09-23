#include "regional_fingerprint.h"

#include "byte_order.h"
#include "snesrecomp/support/digest.h"
#include <string.h>

bool ArRegionalCosts_Fingerprint(const ArRegionalCostPolicy *requested,
    const ArRegionalCostPolicy *effective, uint8_t out[32], bool *baseline) {
  ArRegionalCostSnapshot pending, active;
  if (!out || !baseline || !ArRegionalCosts_Resolve(requested, &pending) ||
      !ArRegionalCosts_Resolve(effective, &active)) return false;
  uint8_t bytes[512] = "ARCOST-R1";
  size_t used = 9;
  bool native = true;
  for (unsigned i = 0; i < kArRegionalCostRule_Count; ++i) {
    const ArRegionalCostDescriptor *rule = ArRegionalCosts_Descriptor((ArRegionalCostRule)i);
    size_t length = strlen(rule->key);
    if (length > 255 || length + 5 > sizeof(bytes) - used) return false;
    bytes[used++] = (uint8_t)length;
    memcpy(bytes + used, rule->key, length); used += length;
    ByteOrder_WriteLe16(bytes + used, pending.price[i]);
    ByteOrder_WriteLe16(bytes + used + 2, active.price[i]); used += 4;
    native &= pending.price[i] == rule->price[kArRegionalSource_US] &&
              active.price[i] == rule->price[kArRegionalSource_US];
  }
  if (!sr_support_sha256(bytes, used, out)) return false;
  *baseline = native;
  return true;
}

bool ArRegionalRules_Fingerprint(const ArRegionalRules *requested,
    const ArRegionalRules *effective,
    uint8_t out[32], bool *baseline) {
  if (!requested || !effective || !out || !baseline ||
      (unsigned)requested->lair_reloads>=kArRegionalSource_Count ||
      (unsigned)effective->lair_reloads>=kArRegionalSource_Count ||
      !ArRegionalTimers_Valid(&requested->timers) ||
      !ArRegionalTimers_Valid(&effective->timers)) return false;
  bool retry_requested, retry_effective;
  bool arrival_requested,arrival_effective;
  if(!ArRegionalArrival_Resolve(requested->arrival,&arrival_requested) ||
      !ArRegionalArrival_Resolve(effective->arrival,&arrival_effective))return false;
  if (!ArRegionalRetry_Resolve(requested->retry_score, &retry_requested) ||
      !ArRegionalRetry_Resolve(effective->retry_score, &retry_effective)) return false;
  uint16_t wait_requested, wait_effective;
  if (!ArRegionalTownWait_Resolve(requested->town_wait, &wait_requested) ||
      !ArRegionalTownWait_Resolve(effective->town_wait, &wait_effective)) return false;
  uint16_t fish_requested, fish_effective;
  if (!ArRegionalFishing_Resolve(requested->fishing, &fish_requested) ||
      !ArRegionalFishing_Resolve(effective->fishing, &fish_effective)) return false;
  ArRegionalDevelopmentSnapshot development;
  if(!ArRegionalDevelopment_Resolve(&requested->development,&development) ||
     !ArRegionalDevelopment_Resolve(&effective->development,&development))return false;
  ArRegionalRecoverySnapshot recovery;
  if (!ArRegionalRecovery_Resolve(&requested->recovery, &recovery) ||
      !ArRegionalRecovery_Resolve(&effective->recovery, &recovery)) return false;
  ArRegionalQuakeSnapshot quake_requested, quake_effective;
  if (!ArRegionalQuake_Resolve(&requested->quake, &quake_requested) ||
      !ArRegionalQuake_Resolve(&effective->quake, &quake_effective)) return false;
  bool score_requested, score_effective;
  ArRegionalStorySnapshot story_requested,story_effective;
  ArRegionalTownStatusSnapshot status_requested,status_effective;
  bool level_requested,level_effective;
  bool construction_requested,construction_effective;
  ArRegionalSupportSnapshot support_requested,support_effective;
  if (!ArRegionalSupport_Resolve(&requested->support,&support_requested) ||
      !ArRegionalSupport_Resolve(&effective->support,&support_effective)) return false;
  if (!ArRegionalConstruction_Resolve(requested->construction,&construction_requested) ||
      !ArRegionalConstruction_Resolve(effective->construction,&construction_effective)) return false;
  uint16_t combat_requested,combat_effective;
  uint16_t ai_requested,ai_effective;
  if (!ArRegionalSimAi_Resolve(&requested->sim_ai,&ai_requested) ||
      !ArRegionalSimAi_Resolve(&effective->sim_ai,&ai_effective)) return false;
  if (!ArRegionalSimCombat_Resolve(&requested->sim_combat,&combat_requested) ||
      !ArRegionalSimCombat_Resolve(&effective->sim_combat,&combat_effective)) return false;
  if (!ArRegionalLevelGoals_Resolve(requested->level_goals,&level_requested) ||
      !ArRegionalLevelGoals_Resolve(effective->level_goals,&level_effective)) return false;
  if (!ArRegionalTownStatus_Resolve(&requested->town_status,&status_requested) ||
      !ArRegionalTownStatus_Resolve(&effective->town_status,&status_effective)) return false;
  if (!ArRegionalStory_Resolve(&requested->story,&story_requested) ||
      !ArRegionalStory_Resolve(&effective->story,&story_effective)) return false;
  uint16_t skull_requested, skull_effective;
  if (!ArRegionalSkullWait_Resolve(requested->skull_wait,&skull_requested) ||
      !ArRegionalSkullWait_Resolve(effective->skull_wait,&skull_effective)) return false;
  ArRegionalSourcesSnapshot sources_requested, sources_effective;
  if (!ArRegionalSources_Resolve(&requested->sources,&sources_requested) ||
      !ArRegionalSources_Resolve(&effective->sources,&sources_effective)) return false;
  bool lives_requested, lives_effective;
  if (!ArRegionalLivesDisplay_Resolve(requested->lives_display, &lives_requested) ||
      !ArRegionalLivesDisplay_Resolve(effective->lives_display, &lives_effective)) return false;
  if (!ArRegionalScorePage_Resolve(requested->score_page, &score_requested) ||
      !ArRegionalScorePage_Resolve(effective->score_page, &score_effective)) return false;
  bool menu_requested, menu_effective;
  if (!ArRegionalMenuReturn_Resolve(requested->menu_return, &menu_requested) ||
      !ArRegionalMenuReturn_Resolve(effective->menu_return, &menu_effective)) return false;
  uint8_t bytes[512] = "ARTIME-R1", digest[32];
  uint16_t speed_requested, speed_effective;
  if (!ArRegionalSpeedRange_Resolve(requested->speed_range, &speed_requested) ||
      !ArRegionalSpeedRange_Resolve(effective->speed_range, &speed_effective)) return false;
  bool gesture_requested, gesture_effective;
  if (!ArRegionalMagicGesture_Resolve(requested->magic_gesture, &gesture_requested) ||
      !ArRegionalMagicGesture_Resolve(effective->magic_gesture, &gesture_effective)) return false;
  if ((unsigned)requested->lair_seeds >= kArRegionalSource_Count ||
      (unsigned)effective->lair_seeds >= kArRegionalSource_Count) return false;
  const bool seeds_requested=requested->lair_seeds==kArRegionalSource_Japan;
  const bool seeds_effective=effective->lair_seeds==kArRegionalSource_Japan;
  if ((unsigned)requested->house_credit >= kArRegionalSource_Count ||
      (unsigned)effective->house_credit >= kArRegionalSource_Count) return false;
  const bool house_requested=requested->house_credit==kArRegionalSource_Japan;
  const bool house_effective=effective->house_credit==kArRegionalSource_Japan;
  ArRegionalScoreSnapshot score_pending, score_active;
  if (!ArRegionalScore_Resolve(&requested->score_feedback,&score_pending) ||
      !ArRegionalScore_Resolve(&effective->score_feedback,&score_active)) return false;
  bool costs_native;
  if (!ArRegionalCosts_Fingerprint(&requested->costs, &effective->costs, digest, &costs_native))
    return false;
  memcpy(bytes + 9, digest, sizeof(digest));
  size_t used = 9 + sizeof(digest);
  bool timers_native = true;
  for (unsigned i = 0; i < kArRegionalTimerRule_Count; ++i) {
    const ArRegionalTimerDescriptor *rule = ArRegionalTimers_Descriptor((ArRegionalTimerRule)i);
    const uint16_t pending = rule->bcd[requested->timers.source[i]];
    const uint16_t active = rule->bcd[effective->timers.source[i]];
    const size_t length = strlen(rule->key);
    if (length > 255 || length + 5 > sizeof(bytes) - used) return false;
    bytes[used++] = (uint8_t)length;
    memcpy(bytes + used, rule->key, length); used += length;
    ByteOrder_WriteLe16(bytes + used, pending);
    ByteOrder_WriteLe16(bytes + used + 2, active); used += 4;
    timers_native &= pending == rule->bcd[kArRegionalSource_US] &&
                     active == rule->bcd[kArRegionalSource_US];
  }
  if (!timers_native && !sr_support_sha256(bytes, used, digest)) return false;
  if (retry_requested || retry_effective) {
    /* Preserve every older digest when this new rule is numerically native. */
    uint8_t retry_bytes[64] = "ARRETRY-R1";
    memcpy(retry_bytes + 10, digest, sizeof(digest));
    retry_bytes[42] = retry_requested;
    retry_bytes[43] = retry_effective;
    if (!sr_support_sha256(retry_bytes, 44, digest)) return false;
  }
  const uint16_t native_wait = ArRegionalTownWait_Descriptor()->updates[kArRegionalSource_US];
  const bool wait_native = wait_requested == native_wait && wait_effective == native_wait;
  if (!wait_native) {
    uint8_t wait_bytes[64] = "ARTOWNWAIT-R1";
    memcpy(wait_bytes + 14, digest, sizeof(digest));
    ByteOrder_WriteLe16(wait_bytes + 46, wait_requested);
    ByteOrder_WriteLe16(wait_bytes + 48, wait_effective);
    if (!sr_support_sha256(wait_bytes, 50, digest)) return false;
  }
  const uint16_t native_fish = ArRegionalFishing_Descriptor()->updates[kArRegionalSource_US];
  const bool fish_native = fish_requested == native_fish && fish_effective == native_fish;
  if (!fish_native) {
    uint8_t fish_bytes[64] = "ARFISHING-R1";
    memcpy(fish_bytes + 12, digest, sizeof(digest));
    ByteOrder_WriteLe16(fish_bytes + 44, fish_requested);
    ByteOrder_WriteLe16(fish_bytes + 46, fish_effective);
    if (!sr_support_sha256(fish_bytes, 48, digest)) return false;
  }
  uint8_t development_bytes[256]="ARDEVELOP-R1";
  memcpy(development_bytes+12,digest,sizeof(digest));used=12+sizeof(digest);
  bool development_native=true;
  for(unsigned i=0;i<kArRegionalDevelopmentRule_Count;++i) {
    const ArRegionalDevelopmentDescriptor *rule=ArRegionalDevelopment_Descriptor((ArRegionalDevelopmentRule)i);
    const uint16_t pending=rule->updates[requested->development.source[i]],active=rule->updates[effective->development.source[i]];
    const size_t length=strlen(rule->key);
    if(length>255 || length+5>sizeof(development_bytes)-used)return false;
    development_bytes[used++]=(uint8_t)length;
    memcpy(development_bytes+used,rule->key,length);used+=length;
    ByteOrder_WriteLe16(development_bytes+used,pending);
    ByteOrder_WriteLe16(development_bytes+used+2,active);used+=4;
    development_native &= pending==rule->updates[kArRegionalSource_US] && active==rule->updates[kArRegionalSource_US];
  }
  if(!development_native && !sr_support_sha256(development_bytes,used,digest))return false;
  uint8_t recovery_bytes[256] = "ARRECOVERY-R1";
  memcpy(recovery_bytes + 14, digest, sizeof(digest));
  used = 14 + sizeof(digest);
  bool recovery_native = true;
  for (unsigned i = 0; i < kArRegionalRecovery_Count; ++i) {
    const ArRegionalRecoveryDescriptor *rule = ArRegionalRecovery_Descriptor((ArRegionalRecoveryRule)i);
    const uint16_t pending = rule->value[requested->recovery.source[i]];
    const uint16_t active = rule->value[effective->recovery.source[i]];
    const size_t length = strlen(rule->key);
    if (length > 255 || length + 5 > sizeof(recovery_bytes) - used) return false;
    recovery_bytes[used++] = (uint8_t)length;
    memcpy(recovery_bytes + used, rule->key, length); used += length;
    ByteOrder_WriteLe16(recovery_bytes + used, pending);
    ByteOrder_WriteLe16(recovery_bytes + used + 2, active); used += 4;
    recovery_native &= pending == rule->value[kArRegionalSource_US] && active == rule->value[kArRegionalSource_US];
  }
  if (!recovery_native && !sr_support_sha256(recovery_bytes, used, digest)) return false;
  uint8_t quake_bytes[256] = "ARQUAKE-R1";
  memcpy(quake_bytes + 10, digest, sizeof(digest)); used = 10 + sizeof(digest);
  bool quake_native = true;
  for (unsigned i = 0; i < kArRegionalQuake_Count; ++i) {
    const ArRegionalQuakeDescriptor *rule = ArRegionalQuake_Descriptor((ArRegionalQuakeRule)i);
    const size_t length = strlen(rule->key);
    if (length > 255 || length + 5 > sizeof(quake_bytes) - used) return false;
    quake_bytes[used++] = (uint8_t)length;
    memcpy(quake_bytes + used, rule->key, length); used += length;
    ByteOrder_WriteLe16(quake_bytes + used, quake_requested.random[i]);
    ByteOrder_WriteLe16(quake_bytes + used + 2, quake_effective.random[i]); used += 4;
    quake_native &= !quake_requested.random[i] && !quake_effective.random[i];
  }
  if (!quake_native && !sr_support_sha256(quake_bytes, used, digest)) return false;
  if (!score_requested || !score_effective) {
    uint8_t score_bytes[48] = "ARSCOREPAGE-R1";
    memcpy(score_bytes + 14, digest, sizeof(digest));
    score_bytes[46] = score_requested; score_bytes[47] = score_effective;
    if (!sr_support_sha256(score_bytes, sizeof(score_bytes), digest)) return false;
  }
  if (menu_requested || menu_effective) {
    uint8_t menu_bytes[50] = "ARMENURETURN-R1";
    memcpy(menu_bytes + 16, digest, sizeof(digest));
    menu_bytes[48] = menu_requested; menu_bytes[49] = menu_effective;
    if (!sr_support_sha256(menu_bytes, sizeof(menu_bytes), digest)) return false;
  }
  const bool speed_native = speed_requested == 9 && speed_effective == 9;
  if (!speed_native) {
    uint8_t speed_bytes[50] = "ARSPEEDRANGE-R1";
    memcpy(speed_bytes + 16, digest, sizeof(digest));
    speed_bytes[48] = (uint8_t)speed_requested; speed_bytes[49] = (uint8_t)speed_effective;
    if (!sr_support_sha256(speed_bytes, sizeof(speed_bytes), digest)) return false;
  }
  if (gesture_requested || gesture_effective) {
    uint8_t gesture_bytes[52] = "ARMAGICGESTURE-R1";
    memcpy(gesture_bytes+18,digest,sizeof(digest));
    gesture_bytes[50]=gesture_requested; gesture_bytes[51]=gesture_effective;
    if (!sr_support_sha256(gesture_bytes,sizeof(gesture_bytes),digest)) return false;
  }
  if (seeds_requested || seeds_effective) {
    uint8_t seed_bytes[50] = "ARLAIRSEEDS-R1";
    memcpy(seed_bytes+16,digest,sizeof(digest));
    seed_bytes[48]=seeds_requested; seed_bytes[49]=seeds_effective;
    if (!sr_support_sha256(seed_bytes,sizeof(seed_bytes),digest)) return false;
  }
  if (house_requested || house_effective) {
    uint8_t house_bytes[50] = "ARHOUSECREDIT-R1";
    memcpy(house_bytes+16,digest,sizeof(digest));
    house_bytes[48]=house_requested; house_bytes[49]=house_effective;
    if (!sr_support_sha256(house_bytes,sizeof(house_bytes),digest)) return false;
  }
  uint8_t score_bytes[192]="ARSCORESTOCK-R1";
  memcpy(score_bytes+16,digest,sizeof(digest)); used=16+sizeof(digest);
  bool score_feedback_native=true;
  /* Keep the v15 arithmetic identity unchanged when phase remains US. */
  for (unsigned i=0; i<kArRegionalScore_Phase; ++i) {
    const ArRegionalScoreDescriptor *rule=ArRegionalScore_Descriptor((ArRegionalScoreRule)i);
    const size_t length=strlen(rule->key);
    if (length>255 || length+5>sizeof(score_bytes)-used) return false;
    score_bytes[used++]=(uint8_t)length;
    memcpy(score_bytes+used,rule->key,length); used+=length;
    ByteOrder_WriteLe16(score_bytes+used,score_pending.japanese[i]);
    ByteOrder_WriteLe16(score_bytes+used+2,score_active.japanese[i]); used+=4;
    score_feedback_native &= !score_pending.japanese[i] && !score_active.japanese[i];
  }
  if (!score_feedback_native && !sr_support_sha256(score_bytes,used,digest)) return false;
  const bool phase_pending=score_pending.japanese[kArRegionalScore_Phase];
  const bool phase_active=score_active.japanese[kArRegionalScore_Phase];
  if (phase_pending || phase_active) {
    uint8_t phase_bytes[50]="ARSCOREPHASE-R1";
    memcpy(phase_bytes+16,digest,sizeof(digest));
    phase_bytes[48]=phase_pending; phase_bytes[49]=phase_active;
    if (!sr_support_sha256(phase_bytes,sizeof(phase_bytes),digest)) return false;
    score_feedback_native=false;
  }
  if (lives_requested || lives_effective) {
    uint8_t lives_bytes[50]="ARLIFEDISPLAY-R1";
    memcpy(lives_bytes+16,digest,sizeof(digest));
    lives_bytes[48]=lives_requested; lives_bytes[49]=lives_effective;
    if (!sr_support_sha256(lives_bytes,sizeof(lives_bytes),digest)) return false;
  }
  bool sources_native=true;
  _Static_assert(kArRegionalSourceItem_Count==2,"ARSOURCES-R1 has two independent collection leaves");
  uint8_t source_bytes[52]="ARSOURCES-R1";
  memcpy(source_bytes+16,digest,sizeof(digest));
  for(unsigned i=0;i<kArRegionalSourceItem_Count;++i) {
    source_bytes[48+2*i]=sources_requested.automatic[i];
    source_bytes[49+2*i]=sources_effective.automatic[i];
    sources_native &= sources_requested.automatic[i] && sources_effective.automatic[i];
  }
  if (!sources_native && !sr_support_sha256(source_bytes,sizeof(source_bytes),digest)) return false;
  const bool skull_native=skull_requested==90 && skull_effective==90;
  if (!skull_native) {
    uint8_t skull_bytes[52]="ARSKULLWAIT-R1";
    memcpy(skull_bytes+16,digest,sizeof(digest));
    ByteOrder_WriteLe16(skull_bytes+48,skull_requested);
    ByteOrder_WriteLe16(skull_bytes+50,skull_effective);
    if (!sr_support_sha256(skull_bytes,sizeof(skull_bytes),digest)) return false;
  }
  bool story_native=true;
  _Static_assert(kArRegionalStory_Count==3,"ARSTORY-R1 has three prerequisite leaves");
  uint8_t story_bytes[60]="ARSTORY-R1";
  memcpy(story_bytes+16,digest,sizeof(digest));
  for (unsigned i=0;i<kArRegionalStory_Count;++i) {
    const unsigned native=ArRegionalStory_Descriptor((ArRegionalStoryRule)i)->value[kArRegionalSource_US];
    story_native &= story_requested.value[i]==native && story_effective.value[i]==native;
    ByteOrder_WriteLe16(story_bytes+48+4*i,story_requested.value[i]);
    ByteOrder_WriteLe16(story_bytes+50+4*i,story_effective.value[i]);
  }
  if (!story_native && !sr_support_sha256(story_bytes,sizeof(story_bytes),digest)) return false;
  const bool reload_native=requested->lair_reloads!=kArRegionalSource_Japan &&
      effective->lair_reloads!=kArRegionalSource_Japan;
  if (!reload_native) {
    uint8_t reload_bytes[50]="ARRELOADPOL-R1";
    memcpy(reload_bytes+16,digest,32);
    reload_bytes[48]=requested->lair_reloads==kArRegionalSource_Japan;
    reload_bytes[49]=effective->lair_reloads==kArRegionalSource_Japan;
    if (!sr_support_sha256(reload_bytes,sizeof(reload_bytes),digest)) return false;
  }
  bool status_native=true;
  _Static_assert(kArRegionalTownStatus_Count==5,"ARTOWNSTATUS-R1 has five reporting leaves");
  uint8_t status_bytes[58]="ARTOWNSTATUS-R1";
  memcpy(status_bytes+16,digest,sizeof(digest));
  for (unsigned i=0;i<kArRegionalTownStatus_Count;++i) {
    status_bytes[48+2*i]=(uint8_t)status_requested.japanese[i];
    status_bytes[49+2*i]=(uint8_t)status_effective.japanese[i];
    status_native &= !status_requested.japanese[i] && !status_effective.japanese[i];
  }
  if (!status_native && !sr_support_sha256(status_bytes,sizeof(status_bytes),digest)) return false;
  if (level_requested || level_effective) {
    uint8_t level_bytes[50]="ARLEVELGOALS-R1";
    memcpy(level_bytes+16,digest,sizeof(digest));
    level_bytes[48]=level_requested;level_bytes[49]=level_effective;
    if (!sr_support_sha256(level_bytes,sizeof(level_bytes),digest)) return false;
  }
  if (combat_requested || combat_effective) {
    uint8_t combat_bytes[52]="ARSIMCOMBAT-R1";
    memcpy(combat_bytes+16,digest,sizeof(digest));
    ByteOrder_WriteLe16(combat_bytes+48,combat_requested);ByteOrder_WriteLe16(combat_bytes+50,combat_effective);
    if (!sr_support_sha256(combat_bytes,sizeof(combat_bytes),digest)) return false;
  }
  if (ai_requested || ai_effective) {
    uint8_t ai_bytes[52]="ARSIMAI-R1";
    memcpy(ai_bytes+16,digest,sizeof(digest));
    ByteOrder_WriteLe16(ai_bytes+48,ai_requested);ByteOrder_WriteLe16(ai_bytes+50,ai_effective);
    if (!sr_support_sha256(ai_bytes,sizeof(ai_bytes),digest)) return false;
  }
  if (construction_requested || construction_effective) {
    uint8_t construction_bytes[50]="ARBUILDPRICE-R1";
    memcpy(construction_bytes+16,digest,sizeof(digest));
    construction_bytes[48]=construction_requested;construction_bytes[49]=construction_effective;
    if (!sr_support_sha256(construction_bytes,sizeof(construction_bytes),digest)) return false;
  }
  uint8_t support_bytes[48+4*kArRegionalSupport_Count]="ARSUPPORT-R1";
  memcpy(support_bytes+16,digest,sizeof(digest));
  bool support_native=true;
  for (unsigned i=0;i<kArRegionalSupport_Count;++i) {
    ByteOrder_WriteLe16(support_bytes+48+i*4,support_requested.amount[i]);
    ByteOrder_WriteLe16(support_bytes+50+i*4,support_effective.amount[i]);
    const unsigned native=ArRegionalSupport_Descriptor((ArRegionalSupportRule)i)->amount[kArRegionalSource_US];
    support_native &= support_requested.amount[i]==native && support_effective.amount[i]==native;
  }
  if (!support_native && !sr_support_sha256(support_bytes,sizeof(support_bytes),digest)) return false;
  memcpy(out,digest,sizeof(digest));
  *baseline = costs_native && timers_native && !retry_requested && !retry_effective && wait_native && fish_native && development_native && recovery_native && quake_native && score_requested && score_effective && !menu_requested && !menu_effective && speed_native && !gesture_requested && !gesture_effective && !seeds_requested && !seeds_effective && !house_requested && !house_effective && score_feedback_native;
  *baseline &= !lives_requested && !lives_effective && sources_native && skull_native && story_native && reload_native && status_native;
  *baseline &= !level_requested && !level_effective && !combat_requested && !combat_effective;
  *baseline &= !ai_requested && !ai_effective;
  *baseline &= !construction_requested && !construction_effective;
  *baseline &= support_native;
  if(arrival_requested || arrival_effective) {
    uint8_t arrival_bytes[50]="ARARRIVAL-R1";
    memcpy(arrival_bytes+16,digest,32);arrival_bytes[48]=arrival_requested;arrival_bytes[49]=arrival_effective;
    if(!sr_support_sha256(arrival_bytes,sizeof(arrival_bytes),out))return false;
    *baseline=false;
  }
  return true;
}

bool ArRegionalArrivalLock_Fingerprint(const uint8_t previous[32],ArRegionalSource requested,
    ArRegionalSource effective,bool locked,uint8_t out[32]) {
  bool req,eff;
  if(!previous || !out || !ArRegionalArrival_Resolve(requested,&req) || !ArRegionalArrival_Resolve(effective,&eff))return false;
  if(!req && !eff){memmove(out,previous,32);return true;}
  uint8_t bytes[49]="ARARRLOCK-R1";memcpy(bytes+16,previous,32);bytes[48]=locked;
  return sr_support_sha256(bytes,sizeof(bytes),out);
}

bool ArRegionalSimActors_Fingerprint(const uint8_t previous[32],const ArRegionalSimActors *actors,
                                    uint8_t out[32],bool *baseline) {
  if (!previous || !out || !baseline || !ArRegionalSimActors_Valid(actors)) return false;
  bool native=true,ai_native=true;
  for (unsigned i=0;i<24;++i) { native &= !actors->cached[i].combat;ai_native &= !actors->cached[i].ai; }
  for (unsigned i=0;i<4;++i) { native &= !actors->active[i].combat;ai_native &= !actors->active[i].ai; }
  native &= ai_native;
  if (native) { memmove(out,previous,32);*baseline=true;return true; }
  uint8_t bytes[48+kArRegionalSimActorsEncodedBytes]="ARSIMACTOR-R1";
  if (!ai_native) memcpy(bytes,"ARSIMACTOR-R2",13);
  memcpy(bytes+16,previous,32);
  const size_t encoded=ai_native?kArRegionalSimActorsV1EncodedBytes:kArRegionalSimActorsEncodedBytes;
  if (!ArRegionalSimActors_EncodeVersion(actors,bytes+48,encoded,ai_native?1:2) || !sr_support_sha256(bytes,48+encoded,out)) return false;
  *baseline=false;return true;
}
