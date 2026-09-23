#include "regional_campaign.h"

#include <stdio.h>
#include <string.h>

static bool Fail(SaveError *error, const char *message) {
  if (error) snprintf(error->message, sizeof(error->message), "%s", message);
  return false;
}

void ArRegionalCampaign_Init(ArRegionalCampaign *campaign, uint32_t slot,
                             ArRegionalCampaignIdentity identity, void *context) {
  if (campaign) *campaign = (ArRegionalCampaign){.slot = slot,
      .identity = identity, .identity_context = context};
}

static bool Create(ArRegionalCampaign *campaign, const ArRegionalCostPolicy *defaults,
                    ArRegionalSession *session, SaveError *error) {
  uint8_t id[16];
  if (!campaign || !campaign->identity ||
      !campaign->identity(campaign->identity_context, id))
    return Fail(error, "cannot create a regional campaign identity");
  if (!ArRegionalSession_NewGame(session, campaign->slot, id, defaults))
    return Fail(error, "invalid regional campaign defaults or identity");
  return true;
}

bool ArRegionalCampaign_NewGame(ArRegionalCampaign *campaign,
    const ArRegionalCostPolicy *defaults, SaveError *error) {
  if (error) error->message[0] = 0;
  ArRegionalSession next;
  if (!Create(campaign, defaults, &next, error)) return false;
  campaign->active = next;
  campaign->active_valid = true;
  return true;
}

static bool LoadOrAdopt(ArRegionalCampaign *campaign, const char *path,
    const uint8_t *image, ArRegionalSession *next, SaveError *error) {
  SaveCheckpointStatus status = ArRegionalSession_Load(next, campaign->slot, path, image, error);
  if (status == kSaveCheckpoint_Ready) return true;
  if (status != kSaveCheckpoint_Missing) return false;
  ArRegionalCostPolicy baseline;
  ArRegionalCosts_Init(&baseline, kArRegionalSource_US);
  return Create(campaign, &baseline, next, error);
}

bool ArRegionalCampaign_Continue(ArRegionalCampaign *campaign,
    const char *path, const uint8_t *image, SaveError *error) {
  if (error) error->message[0] = 0;
  if (!campaign || !path || !image) return Fail(error, "no durable campaign to continue");
  /* Failure must not leave a prior campaign eligible for a new save. */
  campaign->active_valid = false;
  ArRegionalSession next;
  if (!LoadOrAdopt(campaign, path, image, &next, error)) return false;
  campaign->active = next;
  campaign->active_valid = true;
  return true;
}

static bool Prepare(void *context, SaveError *error) {
  ArRegionalCampaign *campaign = context;
  if (!campaign || !campaign->active_valid)
    return Fail(error, "story save has no confirmed regional campaign");
  campaign->pending = campaign->active;
  campaign->pending_valid = true;
  return true;
}

static bool Commit(void *context, SaveFileFormat format, const char *path,
    const uint8_t *expected, const uint8_t *image, SaveCommitKind kind,
    const char *import_path, SaveError *error) {
  ArRegionalCampaign *campaign = context;
  if (!campaign) return Fail(error, "missing regional campaign owner");
  ArRegionalSession session;
  if (kind == kSaveCommit_Story) {
    if (!campaign->pending_valid) return Fail(error, "story checkpoint was not prepared");
    session = campaign->pending;
  } else if (kind == kSaveCommit_Import) {
    if (!import_path || !LoadOrAdopt(campaign, import_path, image, &session, error)) return false;
  } else {
    /* Completion markers/editor changes belong to the durable campaign, even
     * when a different unsaved New Game is currently running. */
    if (expected) {
      if (!LoadOrAdopt(campaign, path, expected, &session, error)) return false;
    } else {
      ArRegionalCostPolicy baseline;
      ArRegionalCosts_Init(&baseline, kArRegionalSource_US);
      if (!Create(campaign, &baseline, &session, error)) return false;
    }
  }
  if (!ArRegionalSession_Save(&session, format, path, expected, image, error)) return false;
  if (kind == kSaveCommit_Story) campaign->pending_valid = false;
  return true;
}

static void Reloaded(void *context) {
  ArRegionalCampaign *campaign = context;
  campaign->active_valid = campaign->pending_valid = false;
}

SaveCommitHost ArRegionalCampaign_SaveHost(ArRegionalCampaign *campaign) {
  if (!campaign) return (SaveCommitHost){0};
  return (SaveCommitHost){.context = campaign, .prepare_story = Prepare,
      .commit = Commit, .reloaded = Reloaded};
}
