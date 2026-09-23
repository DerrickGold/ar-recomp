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
      !ArRegionalTimers_Valid(&requested->timers) ||
      !ArRegionalTimers_Valid(&effective->timers)) return false;
  bool retry_requested, retry_effective;
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
  memcpy(out,digest,sizeof(digest));
  *baseline = costs_native && timers_native && !retry_requested && !retry_effective && wait_native && fish_native && development_native && recovery_native && quake_native && score_requested && score_effective && !menu_requested && !menu_effective && speed_native && !gesture_requested && !gesture_effective;
  return true;
}
