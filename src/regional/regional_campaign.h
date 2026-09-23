#ifndef AR_REGIONAL_CAMPAIGN_H
#define AR_REGIONAL_CAMPAIGN_H

#include "regional_session.h"

typedef bool (*ArRegionalCampaignIdentity)(void *context, uint8_t id[16]);

/* Caller-owned campaign coordinator. Pending is captured at native completion,
 * not at the later host poll: starting another unsaved campaign cannot attach
 * its settings to the previous campaign's finished save. */
typedef struct ArRegionalCampaign {
  uint32_t slot;
  ArRegionalCampaignIdentity identity;
  void *identity_context;
  ArRegionalSession active, pending;
  bool active_valid, pending_valid;
} ArRegionalCampaign;

void ArRegionalCampaign_Init(ArRegionalCampaign *campaign, uint32_t slot,
                             ArRegionalCampaignIdentity identity, void *context);
bool ArRegionalCampaign_NewGame(ArRegionalCampaign *campaign,
    const ArRegionalCostPolicy *defaults, SaveError *error);
/* image is the last durable native image, not a session-edited shadow.
 * Legacy saves adopt US rules in memory only; this does NOT acknowledge/initialize
 * historical lair accounting. Corrupt/unknown/mismatched metadata blocks entry. */
bool ArRegionalCampaign_Continue(ArRegionalCampaign *campaign,
    const char *path, const uint8_t image[kActRaiserSramSize], SaveError *error);
SaveCommitHost ArRegionalCampaign_SaveHost(ArRegionalCampaign *campaign);

#endif
