#include "actraiser/regional/actraiser_population_conversion.h"

#include "actraiser/actraiser_level_goals.h"
#include "actraiser/actraiser_story_snapshot.h"
#include "actraiser/actraiser_town_census.h"

#include <stdio.h>
#include <string.h>

static bool PalaceBoundary(CpuState *cpu) {
  return cpu && cpu->PB==1 && cpu->DB==1 && !cpu->D && cpu->m_flag && !cpu->x_flag &&
      !cpu->emulation && !cpu->_flag_D && !(cpu->P&CPU_P_D) &&
      cpu_read16(cpu,0,0x0018)==0x0700 && cpu_read8(cpu,0,0x0216) &&
      !cpu_read8(cpu,0,0x0336) && cpu_read8(cpu,0,0x0347)!=7 &&
      !cpu_read16(cpu,0x7f,0x7beb) && !cpu_read16(cpu,0x7f,0x7be9) &&
      !cpu_read16(cpu,0x7f,0x90e9) &&
      cpu_read16(cpu,0x7f,0x7bfb)<12 && !(cpu_read16(cpu,0x7f,0x7bfb)&1);
}

static bool Candidate(ArRegionalSession *session,ArRegionalSource source,
    bool profile,ArRegionalProfileGroup group) {
  if(profile && group!=kArRegionalProfile_Population && group!=kArRegionalProfile_Gameplay)return false;
  if(!ArRegionalSession_SetPopulationProfile(session,session->revision,source))return false;
  if(!profile)return true;
  ArRegionalRules requested;
  return ArRegionalProfiles_Expand(&session->requested,group,source,&requested) &&
      ArRegionalSession_RequestRules(session,session->revision,&requested);
}

static ActRaiserPopulationResult Preview(CpuState *cpu,
    const ArRegionalCampaign *campaign, ArRegionalSource source, bool profile,
    ArRegionalProfileGroup group, ActRaiserPopulationPreview *out) {
  if (!out || !campaign || !campaign->active_valid || campaign->pending_valid || !PalaceBoundary(cpu))
    return kActRaiserPopulation_Unsafe;
  ArRegionalSession candidate=campaign->active;
  if (!Candidate(&candidate,source,profile,group))
    return kActRaiserPopulation_Unsafe;
  if (candidate.revision==campaign->active.revision) return kActRaiserPopulation_Unchanged;
  ArRegionalSupportSnapshot before,after;
  bool japanese;
  uint16_t threshold;
  if (!ArRegionalSupport_Resolve(&campaign->active.effective.support,&before) ||
      !ArRegionalSupport_Resolve(&candidate.effective.support,&after) ||
      !ArRegionalConstruction_Resolve(candidate.requested.construction,&japanese) ||
      !ArRegionalLevelGoals_Display(source==kArRegionalSource_Japan,cpu_read16(cpu,0,0x0291),&threshold))
    return kActRaiserPopulation_Unsafe;
  ActRaiserPopulationPreview next={.revision=campaign->active.revision,.source=source,
      .profile=profile,.group=group,
      .redevelop=memcmp(&before,&after,sizeof(before))!=0};
  memcpy(next.campaign,campaign->active.campaign,sizeof(next.campaign));
  if (ActRaiserTownRedevelopment_Preview(cpu,next.redevelop?63:0,japanese,&next.town)!=kActRaiserRedevelopment_Ready)
    return kActRaiserPopulation_InvalidTown;
  *out=next;return kActRaiserPopulation_Ready;
}

ActRaiserPopulationResult ActRaiserPopulation_Preview(CpuState *cpu,
    const ArRegionalCampaign *campaign,ArRegionalSource source,ActRaiserPopulationPreview *out) {
  return Preview(cpu,campaign,source,false,kArRegionalProfile_Population,out);
}
ActRaiserPopulationResult ActRaiserPopulation_PreviewProfile(CpuState *cpu,
    const ArRegionalCampaign *campaign,ArRegionalProfileGroup group,
    ArRegionalSource source,ActRaiserPopulationPreview *out) {
  return Preview(cpu,campaign,source,true,group,out);
}

/* Exact write footprint, not a save state. Restore only these game-owned ranges
 * on failure; no frames run between capture and commit/rollback. */
typedef struct Undo {
  uint8_t marks[0x1800], records[0x0c00], visuals[0x0400];
  uint8_t growth[12], support[12], populations[16], next_goal[2], queue[14];
} Undo;
static void Bytes(CpuState *cpu,bool restore,uint8_t bank,uint16_t address,uint8_t *bytes,size_t size) {
  for(size_t i=0;i<size;++i) {
    if(restore)cpu_write8(cpu,bank,address+i,bytes[i]);
    else bytes[i]=cpu_read8(cpu,bank,address+i);
  }
}
static void Backup(CpuState *cpu,Undo *undo,bool restore) {
  Bytes(cpu,restore,0x7f,0x2000,undo->marks,sizeof(undo->marks));
  Bytes(cpu,restore,0x7f,0x6be7,undo->records,sizeof(undo->records));
  Bytes(cpu,restore,0x7f,0x77e7,undo->visuals,sizeof(undo->visuals));
  Bytes(cpu,restore,0x7f,0x9efa,undo->growth,sizeof(undo->growth));
  Bytes(cpu,restore,0x7f,0x6b26,undo->support,sizeof(undo->support));
  Bytes(cpu,restore,0,0x0218,undo->populations,sizeof(undo->populations));
  Bytes(cpu,restore,0,0x0297,undo->next_goal,sizeof(undo->next_goal));
  Bytes(cpu,restore,0x7f,0x9758,undo->queue,sizeof(undo->queue));
}

static void RetireVisuals(CpuState *cpu,const Undo *undo,const ActRaiserTownRedevelopmentPlan *town) {
  const unsigned current=cpu_read16(cpu,0x7f,0x7bfb)/2;
  if (town->affected_towns&(1u<<current)) {
    for(unsigned slot=0;slot<128;++slot) {
      const unsigned record=current*512+slot*4;
      if ((undo->records[record+2]&128) && !cpu_read8(cpu,0x7f,0x6be7+record+2))
        for(unsigned i=0;i<8;++i)cpu_write8(cpu,0x7f,0x77e7+slot*8+i,0);
    }
  }
  /* These are completed/unused while in the Palace, not an in-flight batch.
   * Retire stale identities; next $82DB rebuilds all seven queue entries. */
  for(unsigned i=0;i<7;++i)cpu_write16(cpu,0x7f,0x9758+2*i,0xffff);
}

static bool Refresh(CpuState *cpu,const ArRegionalSession *candidate,bool census) {
  if(census) {
    ArRegionalSupportSnapshot snapshot;
    if(!ArRegionalSupport_Resolve(&candidate->effective.support,&snapshot))return false;
    for(unsigned town=0;town<6;++town)if(!ActRaiserTownCensus_Refresh(cpu,town,&snapshot))return false;
    /* Semantic equivalent of $03:8E10; no CPU scratch or award side effects. */
    uint16_t total=0;
    for(unsigned town=0;town<6;++town)total+=cpu_read16(cpu,0,0x021c+town*2);
    cpu_write16(cpu,0,0x0218,total);
    cpu_write16(cpu,0,0x021a,cpu_read16(cpu,0,0x021c+cpu_read16(cpu,0x7f,0x7bfb)));
  }
  return ActRaiserLevelGoals_RefreshDisplay(cpu,candidate->effective.level_goals==kArRegionalSource_Japan);
}

ActRaiserPopulationResult ActRaiserPopulation_Commit(CpuState *cpu,
    ArRegionalCampaign *campaign,const ActRaiserPopulationPreview *confirmed,
    const char *directory,SaveError *error) {
  if(error)error->message[0]=0;
  if(!confirmed || !campaign || !campaign->active_valid || confirmed->revision!=campaign->active.revision ||
      memcmp(confirmed->campaign,campaign->active.campaign,16))return kActRaiserPopulation_Stale;
  ActRaiserPopulationPreview fresh;
  const ActRaiserPopulationResult preview=Preview(cpu,campaign,confirmed->source,
      confirmed->profile,confirmed->group,&fresh);
  if(preview!=kActRaiserPopulation_Ready)return preview;
  if(fresh.redevelop!=confirmed->redevelop || fresh.town.fingerprint!=confirmed->town.fingerprint ||
      fresh.town.affected_towns!=confirmed->town.affected_towns ||
      memcmp(fresh.town.removed,confirmed->town.removed,sizeof(fresh.town.removed)) ||
      memcmp(fresh.town.growth_credit,confirmed->town.growth_credit,sizeof(fresh.town.growth_credit)))
    return kActRaiserPopulation_Stale;
  const ArRegionalSession previous=campaign->active;
  ArRegionalSession candidate=previous;
  if(!Candidate(&candidate,confirmed->source,confirmed->profile,confirmed->group))return kActRaiserPopulation_Stale;
  uint8_t image[kActRaiserSramSize];
  if(!ActRaiserStorySnapshot_Capture(cpu,image) ||
      SaveSystem_CommitStorySnapshot(image,error)!=kSaveStorySnapshot_Committed)
    return kActRaiserPopulation_CheckpointFailed;
  if(!SaveSystem_CreateRecoveryCopy(directory,error))return kActRaiserPopulation_RecoveryFailed;
  Undo undo;Backup(cpu,&undo,false);
  if(ActRaiserTownRedevelopment_Apply(cpu,&fresh.town)!=kActRaiserRedevelopment_Ready) {
    if(error)snprintf(error->message,sizeof(error->message),"town changed after recovery checkpoint");
    return kActRaiserPopulation_Stale;
  }
  if(fresh.town.affected_towns)RetireVisuals(cpu,&undo,&fresh.town);
  if(!Refresh(cpu,&candidate,fresh.redevelop) || !ActRaiserStorySnapshot_Capture(cpu,image)) {
    Backup(cpu,&undo,true);return kActRaiserPopulation_RolledBack;
  }
  campaign->active=candidate;
  const SaveStorySnapshotResult result=SaveSystem_CommitStorySnapshot(image,error);
  if(result==kSaveStorySnapshot_NotCommitted) {
    campaign->active=previous;Backup(cpu,&undo,true);return kActRaiserPopulation_RolledBack;
  }
  return result==kSaveStorySnapshot_NamePending?kActRaiserPopulation_NamePending:kActRaiserPopulation_Committed;
}
