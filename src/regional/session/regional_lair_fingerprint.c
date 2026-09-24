#include "regional/session/regional_lair_fingerprint.h"
#include "snesrecomp/support/digest.h"
#include <string.h>

bool ArRegionalLairReloads_Fingerprint(const uint8_t prior[32], const ArRegionalLairReloads *history,
    ArRegionalSource requested, ArRegionalSource effective, uint8_t out[32], bool *native) {
  if (!prior || !out || !native || !ArRegionalLairReloads_Valid(history) ||
      (unsigned)requested>=kArRegionalSource_Count || (unsigned)effective>=kArRegionalSource_Count) return false;
  const bool pending=requested==kArRegionalSource_Japan, active=effective==kArRegionalSource_Japan;
  if (!pending && !active) { memmove(out,prior,32); *native=true; return true; }
  if (history->initialized_towns!=0x3f) return false;
  uint8_t bytes[50+kArRegionalLairReloadEncodedBytes]="ARLAIRDELAY-R1";
  memcpy(bytes+16,prior,32); bytes[48]=pending; bytes[49]=active;
  if (!ArRegionalLairReloads_Encode(history,bytes+50,sizeof(bytes)-50)) return false;
  if (!sr_support_sha256(bytes,sizeof(bytes),out)) return false;
  *native=false; return true;
}

bool ArRegionalLairHistory_Fingerprint(const uint8_t prior[32],
    const ArRegionalLairHistory *history,
    const ArRegionalLairAccounting *requested, const ArRegionalLairAccounting *effective,
    uint8_t out[32], bool *native) {
  unsigned pending, active;
  if (!prior || !out || !native || !ArRegionalLairHistory_Valid(history) ||
      !ArRegionalLairAccounting_Projection(requested, &pending) ||
      !ArRegionalLairAccounting_Projection(effective, &active)) return false;
  if (!pending && !active) {
    memmove(out, prior, 32);
    *native = true;
    return true;
  }
  if (history->initialized_towns != 0x3f) return false;
  uint8_t bytes[43 + kArRegionalLairHistoryEncodedBytes] = "ARLAIR-R1";
  memcpy(bytes + 9, prior, 32);
  bytes[41] = (uint8_t)pending;
  bytes[42] = (uint8_t)active;
  if (!ArRegionalLairHistory_Encode(history, bytes + 43, sizeof(bytes) - 43)) return false;
  uint8_t digest[32];
  if (!sr_support_sha256(bytes, sizeof(bytes), digest)) return false;
  memcpy(out, digest, sizeof(digest));
  *native = false;
  return true;
}
