#include "regional/session/regional_campaign.h"

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

bool ArRegionalCampaign_AcknowledgeLairHistory(ArRegionalCampaign *campaign,
    SaveFileFormat format, const char *path, const uint8_t *image,
    const uint16_t remaining[kArRegionalLairCount], SaveError *error) {
  if (error) error->message[0] = 0;
  if (!campaign) return Fail(error, "no campaign owner for history acknowledgement");
  campaign->active_valid = false;
  if (!path || !image || !remaining || !Save_ChecksumValid(image) || campaign->pending_valid)
    return Fail(error, "no unchanged durable save for history acknowledgement");
  ArRegionalSession next;
  if (!LoadOrAdopt(campaign, path, image, &next, error)) return false;
  const ArRegionalLairAccounting native = {0};
  if (next.lairs.diverged_towns)
    return Fail(error, "retained lair history needs recovery; it has not been replaced");
  const bool changed = next.lairs.initialized_towns != 0x3f;
  for (unsigned town = 0; town < kArRegionalLairTowns; ++town) {
    if (!(next.lairs.initialized_towns & (1u << town))) {
      if (!ArRegionalLairHistory_AdoptTown(&next.lairs, town, kArRegionalSource_US,
                                          remaining + town * kArRegionalLairsPerTown))
        return Fail(error, "cannot initialize the missing lair history");
    } else {
      for (unsigned n = 0; n < kArRegionalLairsPerTown; ++n) {
        const unsigned lair = town * kArRegionalLairsPerTown + n;
        uint16_t retained;
        if (!ArRegionalLairHistory_Read(&next.lairs, &native, lair, &retained) ||
            retained != remaining[lair])
          return Fail(error, "retained lair history does not match this save; preserved");
      }
    }
  }
  if (changed && !ArRegionalSession_Save(&next, format, path, image, image, error)) return false;
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

bool ArRegionalCampaign_SaveSettings(const ArRegionalSession *before,
    const ArRegionalSession *after, SaveFileFormat format, const char *path,
    const uint8_t *image, SaveError *error) {
  if (!before || !after || before->slot != after->slot ||
      memcmp(before->campaign, after->campaign, sizeof(before->campaign)))
    return Fail(error, "regional settings belong to a different campaign");
  ArRegionalSession saved;
  SaveCheckpointStatus status = ArRegionalSession_Load(&saved, before->slot, path, image, error);
  if (status == kSaveCheckpoint_Missing) {
    const ArRegionalCostPolicy baseline = {{0}};
    if (!ArRegionalSession_NewGame(&saved, before->slot, before->campaign, &baseline))
      return Fail(error, "cannot prepare legacy campaign settings");
  } else if (status != kSaveCheckpoint_Ready) return false;
  if (memcmp(saved.campaign, before->campaign, sizeof(saved.campaign)) ||
      memcmp(&saved.requested, &before->requested, sizeof(saved.requested)))
    return Fail(error, "saved campaign changed; reload it before editing regional settings");
  if (!ArRegionalSession_RequestRules(&saved, saved.revision, &after->requested))
    return Fail(error, "these rules require saved history or a confirmed town redevelopment");
  return ArRegionalSession_Save(&saved, format, path, image, image, error);
}

static bool Commit(void *context, SaveFileFormat format, const char *path,
    const uint8_t *expected, const uint8_t *image, SaveCommitKind kind,
    const SaveImportSource *import_source, SaveError *error) {
  ArRegionalCampaign *campaign = context;
  if (!campaign) return Fail(error, "missing regional campaign owner");
  ArRegionalSession session;
  if (kind == kSaveCommit_Story) {
    if (!campaign->pending_valid) return Fail(error, "story checkpoint was not prepared");
    session = campaign->pending;
  } else if (kind == kSaveCommit_StorySnapshot) {
    if (!campaign->active_valid || campaign->pending_valid)
      return Fail(error, "story snapshot has no quiescent regional campaign");
    session = campaign->active;
  } else if (kind == kSaveCommit_Import) {
    if(!import_source)return Fail(error,"missing campaign import source");
    if(import_source->archive && import_source->payload_size) {
      if(ArRegionalSession_Decode(import_source->payload,import_source->payload_size,&session)!=kSaveCheckpoint_Ready)
        return Fail(error,"invalid or unsupported archived campaign");
      /* Slot placement changes, but campaign identity, region history and
       * the exact randomizer recipe remain the archived campaign's own. */
      session.slot=campaign->slot;
    } else if(import_source->archive) {
      ArRegionalCostPolicy baseline;ArRegionalCosts_Init(&baseline,kArRegionalSource_US);
      if(!Create(campaign,&baseline,&session,error))return false;
    } else if(!LoadOrAdopt(campaign,import_source->path,image,&session,error))return false;
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

static bool CopyRecovery(void *context, const char *source_path,
    const char *destination_path, const uint8_t *image, SaveError *error) {
  const ArRegionalCampaign *campaign = context;
  if (!campaign) return Fail(error, "missing regional recovery owner");
  ArRegionalSession saved;
  const SaveCheckpointStatus status =
      ArRegionalSession_Load(&saved, campaign->slot, source_path, image, error);
  /* Preserve a genuinely legacy save as legacy. Never manufacture a new
   * campaign identity or copy settings from an unsaved active campaign. */
  if (status == kSaveCheckpoint_Missing)
    return Save_WriteFile(kSaveFileFormat_NativeSrm, destination_path, image, error);
  if (status != kSaveCheckpoint_Ready) return false;
  return ArRegionalSession_Save(&saved, kSaveFileFormat_NativeSrm,
                                destination_path, NULL, image, error);
}

static bool ReadCampaign(void *context,const char *path,const uint8_t *image,
    void *payload,size_t capacity,size_t *size,SaveError *error) {
  const ArRegionalCampaign *campaign=context;
  ArRegionalSession session;
  SaveCheckpointStatus status=ArRegionalSession_Load(&session,campaign->slot,path,image,error);
  if(status==kSaveCheckpoint_Missing){*size=0;return true;}
  if(status!=kSaveCheckpoint_Ready)return false;
  if(!ArRegionalSession_Encode(&session,payload,capacity,size))return Fail(error,"cannot archive campaign metadata");
  return true;
}

SaveCommitHost ArRegionalCampaign_SaveHost(ArRegionalCampaign *campaign) {
  if (!campaign) return (SaveCommitHost){0};
  return (SaveCommitHost){.context = campaign, .prepare_story = Prepare,
      .commit = Commit, .reloaded = Reloaded, .read_campaign = ReadCampaign, .copy_recovery = CopyRecovery};
}
