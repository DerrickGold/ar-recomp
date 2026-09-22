#ifndef AR_HOST_CAMPAIGN_IDENTITY_H
#define AR_HOST_CAMPAIGN_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

/* A storage identity, not game RNG, locale or a hash of the player's name. */
bool HostCampaignIdentity_Create(void *unused, uint8_t id[16]);

#endif
