#include "actraiser/regional/actraiser_regional_runtime.h"
#include "actraiser/regional/actraiser_regional_editor.h"
#include "regional/session/regional_session_action.h"
#include "actraiser/actraiser_stage_placements.h"
#include "actraiser/regional/actraiser_regional_media.h"

#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser/actraiser_miracle.h"
#include "actraiser/actraiser_scroll_cast.h"
#include "actraiser/actraiser_retry.h"
#include "actraiser/actraiser_town_wait.h"
#include "actraiser/actraiser_fishing.h"
#include "actraiser/actraiser_development.h"
#include "actraiser/actraiser_recovery.h"
#include "actraiser/actraiser_quake.h"
#include "actraiser/actraiser_report_command.h"
#include "actraiser/actraiser_speed_selector.h"
#include "actraiser/actraiser_magic_gesture.h"
#include "actraiser/actraiser_lair_history.h"
#include "actraiser/actraiser_lair_reloads.h"
#include "actraiser/actraiser_town_status_runtime.h"
#include "actraiser/actraiser_construction_runtime.h"
#include "actraiser/regional/actraiser_population_conversion.h"
#include "actraiser/actraiser_arrival_runtime.h"
#include "actraiser/actraiser_level_goals_runtime.h"
#include "actraiser/actraiser_score_feedback.h"
#include "actraiser/actraiser_lives_display.h"
#include "actraiser/actraiser_sources.h"
#include "actraiser/actraiser_story_prerequisites.h"
#include "actraiser/actraiser_native_call.h"
#include "actraiser/actraiser_localization_runtime.h"
#include "actraiser/regional/actraiser_regional_settings.h"
#include "input_replay.h"
#include "randomizer.h"
#include "byte_order.h"
#include "regional/regional_randomizer.h"
#include "regional/session/regional_fingerprint.h"
#include "regional/session/regional_lair_fingerprint.h"
#include "snesrecomp/support/digest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Game-thread owner of campaign lifetime, published snapshots and native
 * continuations. Settings validation is in actraiser_regional_editor.c;
 * room activation and persistence belong to regional/session. Semantic
 * group membership belongs to regional/regional_profiles.c; this owner
 * selects the native transaction boundary, not the menu's grouping. */

extern RecompReturn bank_02_A622_M1X0(CpuState *cpu);
extern RecompReturn bank_03_A83A_M1X0(CpuState *cpu);
extern RecompReturn ActRaiser_WaitForVblank(CpuState *cpu);
extern RecompReturn bank_01_97E5_M1X0(CpuState *cpu);
extern RecompReturn bank_01_9840_M0X0(CpuState *cpu);
extern RecompReturn bank_01_899B_M1X0(CpuState *cpu);
extern RecompReturn bank_01_8AF5_M1X0(CpuState *cpu);
extern RecompReturn bank_01_8C98_M1X0(CpuState *cpu);
extern RecompReturn bank_03_B7C6_M0X0(CpuState *cpu);
extern RecompReturn bank_03_B7C6_M1X0(CpuState *cpu);
extern RecompReturn bank_03_B6BF_M0X0(CpuState *cpu);
extern RecompReturn bank_03_B6BF_M1X0(CpuState *cpu);
extern RecompReturn bank_03_B4A6_M0X0(CpuState *cpu);
extern RecompReturn bank_03_B4A6_M1X0(CpuState *cpu);
extern RecompReturn bank_03_BA42_M0X0(CpuState *cpu);
extern RecompReturn bank_03_BA42_M1X0(CpuState *cpu);
extern RecompReturn bank_03_BADD_M0X0(CpuState *cpu);
extern RecompReturn bank_03_BADD_M1X0(CpuState *cpu);
extern RecompReturn bank_03_D095_M0X0(CpuState *cpu);
extern RecompReturn bank_03_D095_M1X0(CpuState *cpu);
extern RecompReturn bank_00_85B7_M0X0(CpuState *cpu);
extern RecompReturn bank_02_C280_M0X0(CpuState *cpu);
static bool s_lives_delegate;
extern RecompReturn bank_01_9EE7_M1X0(CpuState *cpu);
static bool s_skull_active, s_skull_delegate;
extern RecompReturn bank_03_EB35_M1X0(CpuState *cpu);
static bool s_story_compass_delegate;
static uint16_t s_skull_frames;
static ArRegionalCampaign s_campaign;
static ArRegionalProfileCache s_profile_cache;
/* A title view is never an alias of live gameplay. Interactive boots restore
 * the selected slot; diagnostic/replay boots retain their recorded draft. */
static ArRegionalSession s_title_draft;
static bool s_title_open;
static bool s_title_saved;
static ActRaiserRegionalSettingsWriter s_settings_writer;
static void *s_settings_context;
static uint8_t s_title_artwork;
static void PrepareTitleDraft(void);
static ArRegionalRules s_return_rules;
static bool s_return_rules_valid;
static struct {
  ActRaiserRegionalPopulationIntent intent;
  bool delegate;
  ActRaiserRegionalPopulationPrompt prompt;
  void *context;
} s_population;
static bool PopulationPending(const ArRegionalSession *session) {
  return ActRaiserRegionalEditor_PopulationPending(&s_population.intent, session);
}
extern RecompReturn bank_01_85A2_M1X0(CpuState *cpu);
static bool s_delegate;
static ActRaiserRegionalContinuePrompt s_continue_prompt;
static void *s_continue_context;
static bool s_lair_delegate, s_lair_seed_pending, s_lair_active;
static bool s_reload_delegate, s_reload_active;
static bool s_score_active;
static ArRegionalScoreSnapshot s_score;
/* One native clear-card -> departure transaction. This is not a second
 * reward journal: every retained projection sees the one actual score. */
enum { kScoreIdle, kScoreAwaitDeparture, kScoreSettledAtCard };
static unsigned s_completion_state;
static uint16_t s_completion_scene;
bool ActRaiserRegional_ActorArtwork(unsigned area,bool activate,bool *enabled) {
  if(!enabled || area>=kArRegionalActorArtwork_Count)return false;
  if(!s_campaign.active_valid) {*enabled=false;return true;}
  if(activate)return ArRegionalSession_BeginActorArtwork(&s_campaign.active,area,enabled);
  const ArRegionalSource source=s_campaign.active.requested.actor_artwork.source[area];
  if((unsigned)source>=kArRegionalSource_Count)return false;
  *enabled=source==kArRegionalSource_Japan;return true;
}
static void ActivateLairAccounting(CpuState *cpu);
static void ActivateLairReloads(CpuState *cpu);
static ArRegionalRules s_boot_requested, s_boot_effective;
static ArRegionalLairHistory s_boot_lairs;
static ArRegionalLairReloads s_boot_reloads;
static ArRegionalSimActors s_boot_sim_actors;
static bool s_boot_valid,s_boot_arrival_locked;

/* Published only after every room-boundary activation succeeds. One aggregate
 * also guarantees title/recovery resets cannot leave an older family cached. */
static ArRegionalActionRoomSnapshot s_action;
static uint16_t s_town_art_scene;
static uint8_t s_town_artwork;
static bool s_miracle_active, s_prices_valid;
static bool s_trace;
static bool s_quake_active, s_quake_delegate;
static ArRegionalQuakeSnapshot s_quake;
static unsigned s_quake_selections[kArRegionalQuake_Count]; /* diagnostic only */
static bool s_report_active, s_report_delegate, s_report_score_page;
static bool s_report_command_active;
static bool s_speed_active, s_speed_delegate, s_speed_scale_delegate;
static uint16_t s_speed_maximum;
static ArRegionalSpellInventory s_inventory;
static bool s_inventory_icon_pending;
static uint32_t s_prices_revision;
static ArRegionalCostSnapshot s_prices, s_miracle_prices;

static bool s_prepared_new_game;
static ArRegionalRules s_prepared_rules;
bool ActRaiserRegional_Initialize(ArRegionalCampaignIdentity identity, void *context) {
  return ActRaiserRegional_InitializeSlot(0,identity,context);
}
bool ActRaiserRegional_InitializeSlot(uint32_t slot,ArRegionalCampaignIdentity identity,void *context) {
  s_prepared_new_game=false;
  Randomizer_ReleaseCampaign();
  s_profile_cache = (ArRegionalProfileCache){0};
  s_title_open=false;
  s_title_saved=false;
  s_settings_writer=NULL;s_settings_context=NULL;
  s_title_draft=(ArRegionalSession){0};
  s_town_art_scene=0;s_town_artwork=0;
  s_return_rules_valid=false;
  ArRegionalSpellInventory_Reset(&s_inventory,false);s_inventory_icon_pending=false;
  if (!identity) return false;
  s_delegate = false;
  memset(&s_population,0,sizeof(s_population));
  s_lives_delegate = false;
  s_skull_active=s_skull_delegate=false;
  s_story_compass_delegate=false;
  s_skull_frames=90;
  s_continue_prompt = NULL;
  s_continue_context = NULL;
  s_lair_delegate = s_lair_seed_pending = s_lair_active = false;
  s_reload_delegate = s_reload_active = false;
  ActRaiserTownStatusRuntime_Reset();
  ActRaiserConstructionRuntime_Reset();
  ActRaiserLevelGoalsRuntime_Reset();
  ActRaiserArrivalRuntime_Reset();
  s_score_active = false;
  s_score = (ArRegionalScoreSnapshot){0};
  s_completion_state = kScoreIdle;
  s_completion_scene = 0;
  s_boot_lairs = (ArRegionalLairHistory){0};
  s_boot_reloads = (ArRegionalLairReloads){0};
  s_boot_sim_actors = (ArRegionalSimActors){0};
  s_boot_arrival_locked=false;
  s_action=(ArRegionalActionRoomSnapshot){0};ActRaiserStagePlacements_Reset();
  s_miracle_active = s_prices_valid = false;
  s_quake_active = s_quake_delegate = false;
  s_report_active = s_report_delegate = false;
  s_report_command_active = false;
  s_speed_active = s_speed_delegate = s_speed_scale_delegate = false;
  s_speed_maximum = 9;
  s_trace = getenv("AR_REGIONAL_TRACE") != NULL;
  ArRegionalCampaign_Init(&s_campaign, slot, identity, context);
  /* Initialization may follow a previous campaign in the same process. Reset
   * every family before overlaying an available durable companion. */
  s_boot_requested = (ArRegionalRules){0};
  ArRegionalCosts_Init(&s_boot_requested.costs, kArRegionalSource_US);
  ArRegionalTimers_Init(&s_boot_requested.timers, kArRegionalSource_US);
  s_boot_requested.retry_score = kArRegionalSource_US;
  s_boot_requested.town_wait = kArRegionalSource_US;
  s_boot_requested.fishing = kArRegionalSource_US;
  ArRegionalDevelopment_Init(&s_boot_requested.development,kArRegionalSource_US);
  ArRegionalRecovery_Init(&s_boot_requested.recovery, kArRegionalSource_US);
  ArRegionalQuake_Init(&s_boot_requested.quake, kArRegionalSource_US);
  s_boot_requested.score_page = kArRegionalSource_US;
  s_boot_requested.lives_display = kArRegionalSource_US;
  s_boot_requested.skull_wait = kArRegionalSource_US;
  ArRegionalStory_Init(&s_boot_requested.story,kArRegionalSource_US);
  ArRegionalSources_Init(&s_boot_requested.sources,kArRegionalSource_US);
  s_boot_requested.menu_return = kArRegionalSource_US;
  s_boot_requested.speed_range = kArRegionalSource_US;
  s_boot_requested.magic_gesture = kArRegionalSource_US;
  s_boot_requested.lair_seeds = kArRegionalSource_US;
  s_boot_requested.lair_reloads = kArRegionalSource_US;
  ArRegionalTownStatus_Init(&s_boot_requested.town_status,kArRegionalSource_US);
  s_boot_requested.level_goals=kArRegionalSource_US;
  ArRegionalSimCombat_Init(&s_boot_requested.sim_combat,kArRegionalSource_US);
  ArRegionalSimAi_Init(&s_boot_requested.sim_ai,kArRegionalSource_US);
  ArRegionalSupport_Init(&s_boot_requested.support,kArRegionalSource_US);
  s_boot_requested.construction=kArRegionalSource_US;
  s_boot_requested.arrival=kArRegionalSource_US;
  s_boot_requested.house_credit = kArRegionalSource_US;
  ArRegionalScore_Init(&s_boot_requested.score_feedback, kArRegionalSource_US);
  s_boot_effective = s_boot_requested;
  s_boot_valid = true;
  uint8_t image[kActRaiserSramSize];
  if (SaveSystem_CopyDurableImage(image)) {
    ArRegionalSession loaded;
    SaveError error = {{0}};
    SaveCheckpointStatus status = ArRegionalSession_Load(&loaded, slot,
        SaveSystem_ActivePath(), image, &error);
    s_boot_valid = status == kSaveCheckpoint_Ready || status == kSaveCheckpoint_Missing;
    if (status == kSaveCheckpoint_Ready) {
      s_boot_requested = loaded.requested;
      s_boot_effective = loaded.effective;
      s_boot_lairs = loaded.lairs;
      s_boot_reloads = loaded.reloads;
      s_boot_sim_actors = loaded.sim_actors;
      s_boot_arrival_locked=loaded.arrival_locked;
    }
  }
  SaveCommitHost host = ArRegionalCampaign_SaveHost(&s_campaign);
  return SaveSystem_SetCommitHost(&host);
}

bool ActRaiserRegional_StageNewGame(const ArRegionalSession *draft) {
  if(!draft || draft->slot!=s_campaign.slot || s_title_open || s_campaign.active_valid)return false;
  uint8_t bytes[kSaveCheckpointPayloadMax];size_t size;
  if(!ArRegionalSession_Encode(draft,bytes,sizeof(bytes),&size) ||
      !Randomizer_StageTitleConfig(&draft->randomizer))return false;
  s_prepared_rules=draft->requested;s_prepared_new_game=true;
  s_boot_requested=s_boot_effective=draft->requested;
  return true;
}

void ActRaiserRegional_SetSettingsWriter(ActRaiserRegionalSettingsWriter writer,void *context) {
  s_settings_writer=writer;s_settings_context=context;
}

bool ActRaiserRegional_CopySupport(ArRegionalSupportSnapshot *snapshot) {
  const ArRegionalSupportPolicy native={{0}};
  return ArRegionalSupport_Resolve(s_campaign.active_valid?&s_campaign.active.effective.support:&native,snapshot);
}

ArRegionalActionMotionSnapshot ActRaiserRegional_ActionMotionSnapshot(void) { return s_action.motion; }
bool ActRaiserRegional_ScoreLivesEnabled(void) {return s_action.score_lives;}
bool ActRaiserRegional_BeginActionStart(ArRegionalActionStartSnapshot *snapshot) {
  const ArRegionalActionStartPolicy native={{0}};
  return s_campaign.active_valid?ArRegionalSession_BeginActionStart(&s_campaign.active,snapshot):
      ArRegionalActionStart_Resolve(&native,snapshot);
}
bool ActRaiserRegional_StartInventory(void) {
  bool enabled=false;
  if(s_campaign.active_valid && !ArRegionalSession_BeginInventory(&s_campaign.active,&enabled))return false;
  ArRegionalSpellInventory_Reset(&s_inventory,enabled);s_inventory_icon_pending=enabled;
  return true;
}
ActRaiserInventoryView ActRaiserRegional_InventoryView(void) {
  return (ActRaiserInventoryView){s_inventory.count,s_inventory.icon,s_inventory.casting,
      s_inventory.enabled,s_inventory_icon_pending};
}
bool ActRaiserRegional_PushSpell(unsigned spell) {
  if(!ArRegionalSpellInventory_Push(&s_inventory,spell))return false;
  s_inventory_icon_pending=true;return true;
}
bool ActRaiserRegional_BeginSpell(uint8_t *spell) {
  return ArRegionalSpellInventory_BeginCast(&s_inventory,spell);
}
bool ActRaiserRegional_FinishSpell(void) {
  if(!ArRegionalSpellInventory_FinishCast(&s_inventory))return false;
  s_inventory_icon_pending=true;return true;
}
void ActRaiserRegional_InventoryIconUploaded(void) {s_inventory_icon_pending=false;}
ArRegionalEmitterSnapshot ActRaiserRegional_EmitterSnapshot(void) { return s_action.emitters; }
bool ActRaiserRegional_DoubleStatueVolley(void) { return s_action.statue_volley; }
ArRegionalBossSnapshot ActRaiserRegional_BossSnapshot(void) { return s_action.bosses; }
ArRegionalDifficultySnapshot ActRaiserRegional_DifficultySnapshot(void) { return s_action.difficulty; }
ArRegionalCollisionSnapshot ActRaiserRegional_CollisionSnapshot(void) { return s_action.collision; }
ArRegionalPlatformSkullSnapshot ActRaiserRegional_PlatformSkullSnapshot(void) { return s_action.platform_skull; }
ArRegionalCastHoldSnapshot ActRaiserRegional_CastHoldSnapshot(void) { return s_action.cast_hold; }
ArRegionalFireSnapshot ActRaiserRegional_FireSnapshot(void) { return s_action.fire_enemy; }
bool ActRaiserRegional_ActorStatsEnabled(void) { return s_action.actor_stats.changed; }
bool ActRaiserRegional_ActorStats(uint16_t actor,uint16_t native_hp,uint16_t native_attack,uint16_t *hp,uint16_t *attack) {
  return ArRegionalActorStats_Apply(&s_action.actor_stats,actor,native_hp,native_attack,hp,attack);
}
uint16_t ActRaiserRegional_ActorChildStat(unsigned rule) {
  if(rule<kArRegionalActorStat_BaseCount || rule>=kArRegionalActorStat_Count)return UINT16_MAX;
  const uint8_t value=s_action.actor_stats.value[rule];
  return value?value:UINT16_MAX;
}

bool ActRaiserRegional_ArrivalSnapshot(bool latch,bool continuing,bool *japanese) {
  if(!s_campaign.active_valid || !japanese)return false;
  if(latch)return ArRegionalSession_BeginArrival(&s_campaign.active,continuing,japanese);
  return ArRegionalArrival_Resolve(s_campaign.active.arrival_locked?s_campaign.active.effective.arrival:
      s_campaign.active.requested.arrival,japanese);
}

void ActRaiserRegional_SetPopulationPrompt(ActRaiserRegionalPopulationPrompt prompt,void *context) {
  s_population.prompt=prompt;s_population.context=context;
}

bool ActRaiser_RegionalPopulationEntry(CpuState *cpu) {
  if(s_population.delegate){s_population.delegate=false;return false;}
  return s_population.intent.pending && s_campaign.active_valid && InputReplay_PolicyChangesAllowed() &&
      cpu && cpu->PB==1 && cpu->DB==1 && cpu->m_flag && !cpu->x_flag && !cpu->D && !cpu->emulation;
}

static bool PopulationRecoveryPath(char *path,size_t capacity) {
  uint8_t id[16];
  if(!s_campaign.identity || !s_campaign.identity(s_campaign.identity_context,id))return false;
  SaveError error={{0}};
  return SaveSystem_RecoveryPath(id,path,capacity,&error);
}

RecompReturn ActRaiser_RegionalPopulation(CpuState *cpu) {
  const ArRegionalSource source=s_population.intent.source;
  const bool gameplay_profile=s_population.intent.profile && s_population.intent.group==kArRegionalProfile_Gameplay;
  const bool current=PopulationPending(&s_campaign.active) && InputReplay_PolicyChangesAllowed();
  s_population.intent.pending=false; /* A failed/cancelled request is never replayed. */
  ActRaiserPopulationPreview preview;
  ActRaiserPopulationResult result=kActRaiserPopulation_Stale;
  if(current)result=s_population.intent.profile?
      ActRaiserPopulation_PreviewProfile(cpu,&s_campaign,s_population.intent.group,source,&preview):
      ActRaiserPopulation_Preview(cpu,&s_campaign,source,&preview);
  if(result==kActRaiserPopulation_Ready && s_population.prompt) {
    if(s_population.prompt(s_population.context,kActRaiserRegionalPopulation_Confirm,source,gameplay_profile,preview.town.removed)) {
      SaveError error={{0}};char directory[512];
      if(PopulationRecoveryPath(directory,sizeof(directory))) {
        result=ActRaiserPopulation_Commit(cpu,&s_campaign,&preview,directory,&error);
        fprintf(stderr,"[regional] population conversion result=%d recovery=%s%s%s\n",
            result,directory,error.message[0]?" error=":"",error.message);
      } else result=kActRaiserPopulation_RecoveryFailed;
      const ActRaiserRegionalPopulationNotice notice=result==kActRaiserPopulation_Committed?
          kActRaiserRegionalPopulation_Complete:result==kActRaiserPopulation_NamePending?
          kActRaiserRegionalPopulation_NamePending:kActRaiserRegionalPopulation_Failed;
      (void)s_population.prompt(s_population.context,notice,source,gameplay_profile,preview.town.removed);
    }
  } else if(result!=kActRaiserPopulation_Unchanged && s_population.prompt) {
    fprintf(stderr,"[regional] population conversion not started: result=%d source=%d\n",result,source);
    const uint16_t none[6]={0};
    (void)s_population.prompt(s_population.context,kActRaiserRegionalPopulation_Failed,source,gameplay_profile,none);
  }
  /* Original LDY and selector continuation retain their exact CPU/stack ABI. */
  s_population.delegate=true;
  const RecompReturn native=bank_01_85A2_M1X0(cpu);
  s_population.delegate=false;
  return native;
}

bool ActRaiserRegional_CopyPrices(ArRegionalCostSnapshot *prices) {
  if (!prices) return false;
  if (s_miracle_active) { *prices = s_miracle_prices; return true; }
  if (!s_campaign.active_valid) {
    ArRegionalCostPolicy baseline;
    ArRegionalCosts_Init(&baseline, kArRegionalSource_US);
    return ArRegionalCosts_Resolve(&baseline, prices);
  }
  if (!s_prices_valid || s_prices_revision != s_campaign.active.revision) {
    if (!ArRegionalCosts_Resolve(&s_campaign.active.requested.costs, &s_prices)) return false;
    s_prices_revision = s_campaign.active.revision;
    s_prices_valid = true;
  }
  *prices = s_prices;
  return true;
}

static ArRegionalSession *EditableSession(void) {
  return s_title_open?&s_title_draft:s_campaign.active_valid?&s_campaign.active:NULL;
}

bool ActRaiserRegional_ModeEntry(bool activate,uint8_t *snapshot) {
  if(!snapshot)return false;
  ArRegionalSession *session=EditableSession();
  if(!session) {*snapshot=0;return true;}
  return activate?ArRegionalSession_BeginModeEntry(session,snapshot):
      ArRegionalMode_Resolve(&session->requested.mode_entry,snapshot);
}

bool ActRaiserRegional_ReturnToTitle(void) {
  if(s_title_open || !s_campaign.active_valid)return false;
  s_population.intent.pending=false;
  s_return_rules=s_campaign.active.requested;s_return_rules_valid=true;
  // Mode access/return routing must not reset the independently chosen difficulty.
  s_town_art_scene=0;s_town_artwork=0;
  ArRegionalSpellInventory_Reset(&s_inventory,false);s_inventory_icon_pending=false;
  s_action=(ArRegionalActionRoomSnapshot){0};ActRaiserStagePlacements_Reset();
  return true;
}

bool ActRaiserRegional_CopyRulesView(ActRaiserRegionalRulesView *out) {
  ArRegionalSession *session=EditableSession();
  if (!out || !session) return false;
  *out = (ActRaiserRegionalRulesView){.revision = session->revision,
      .requested = session->requested, .effective = session->effective,
      .new_game = s_title_open && !s_title_saved,
      .persistent = s_settings_writer != NULL,
      .editable = InputReplay_PolicyChangesAllowed(), .miracle_in_progress = s_miracle_active};
  out->lair_history_ready = session->lairs.initialized_towns == 0x3f &&
      !session->lairs.diverged_towns;
  out->lair_reload_ready = session->reloads.initialized_towns==0x3f &&
      !session->reloads.diverged_towns;
  out->lair_history_estimated = session->lairs.approximate_towns != 0;
  out->lair_reload_estimated = session->reloads.approximate_towns != 0;
  memcpy(out->campaign, session->campaign, sizeof(out->campaign));
  out->population_pending=!s_title_open && PopulationPending(session);
  out->pending_population=s_population.intent.source;
  out->pending_profile=s_population.intent.profile;
  out->pending_profile_group=s_population.intent.group;
  out->arrival_locked=session->arrival_locked;
  out->artwork_available=ActRaiserRegionalMedia_AvailableArtwork();
  out->sequences_available=ActRaiserRegionalMedia_AvailableSequences();
  out->actor_artwork_available=ActRaiserRegionalMedia_ActorArt()!=NULL;
  if (!ArRegionalProfiles_RefreshCache(&s_profile_cache, &out->requested, &out->effective))
    return false;
  memcpy(out->profiles, s_profile_cache.profiles, sizeof(out->profiles));
  memcpy(out->active_profiles, s_profile_cache.active_profiles, sizeof(out->active_profiles));
  out->pending_groups = s_profile_cache.pending_groups;
  ActRaiserRegionalSettings_DescribeChoices(&out->requested, &out->effective, out->choices);
  return true;
}

/* The runtime supplies current authority on every call. UI copies cannot
 * grant replay permission or select another campaign/session. */
static ActRaiserRegionalEditContext CurrentEditor(void) {
  return (ActRaiserRegionalEditContext){
      .session = EditableSession(), .population = &s_population.intent,
      .new_game = s_title_open && !s_title_saved, .editable = InputReplay_PolicyChangesAllowed()};
}

/* Persist before publishing the copy. UI failure never leaves a changed live
 * request; the storage owner commits choices, not live history/activation. */
static ActRaiserRegionalEditResult FinishSettingsEdit(ActRaiserRegionalEditResult result,
    const ActRaiserRegionalEditContext *edit,ArRegionalSession *candidate,
    const ActRaiserRegionalPopulationIntent *population) {
  if (result!=kActRaiserRegionalEdit_Applied && result!=kActRaiserRegionalEdit_Unchanged &&
      result!=kActRaiserRegionalEdit_Deferred)return result;
  if (s_settings_writer && s_title_open && s_title_saved && result==kActRaiserRegionalEdit_Deferred)
    return kActRaiserRegionalEdit_RequiresGame;
  if (s_settings_writer && result==kActRaiserRegionalEdit_Applied) {
    SaveError error={{0}};
    if (!s_settings_writer(s_settings_context,edit->session,candidate,&error)) {
      fprintf(stderr,"[regional] settings not saved: %s\n",error.message);
      return kActRaiserRegionalEdit_SaveFailed;
    }
  }
  *edit->session=*candidate;*edit->population=*population;
  return result;
}

/* One transaction wrapper for the four public edit shapes. The pure editor
 * continues to own validation, preview and town-redevelopment decisions. */
typedef enum SettingsEditKind { kSettingsRules,kSettingsProfile,kSettingsDifficulty,kSettingsDifficultyChoice } SettingsEditKind;
static ActRaiserRegionalEditResult RequestSettings(const ActRaiserRegionalRulesView *view,
    SettingsEditKind kind,unsigned key,unsigned choice) {
  const ActRaiserRegionalEditContext edit=CurrentEditor();
  if(!edit.session)return kActRaiserRegionalEdit_Invalid;
  ArRegionalSession candidate=*edit.session;
  ActRaiserRegionalPopulationIntent population=*edit.population;
  ActRaiserRegionalEditContext scratch=edit;scratch.session=&candidate;scratch.population=&population;
  ActRaiserRegionalEditResult result;
  switch(kind) {
    case kSettingsRules: result=ActRaiserRegionalEditor_RequestRules(&scratch,view,key,choice);break;
    case kSettingsProfile: result=ActRaiserRegionalEditor_RequestProfile(&scratch,view,key,choice);break;
    case kSettingsDifficulty: result=ActRaiserRegionalEditor_RequestDifficulty(&scratch,view,choice);break;
    default: result=ActRaiserRegionalEditor_RequestDifficultyChoice(&scratch,view,choice);break;
  }
  return FinishSettingsEdit(result,&edit,&candidate,&population);
}

ActRaiserRegionalEditResult ActRaiserRegional_PreviewRules(
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group,
    ArRegionalSource source, ActRaiserRegionalEditImpact *out) {
  const ActRaiserRegionalEditContext edit = CurrentEditor();
  const ActRaiserRegionalEditResult result=ActRaiserRegionalEditor_PreviewRules(&edit,view,group,source,out);
  return s_settings_writer && s_title_open && s_title_saved && result==kActRaiserRegionalEdit_Deferred
      ? kActRaiserRegionalEdit_RequiresGame : result;
}
ActRaiserRegionalEditResult ActRaiserRegional_RequestDifficultyChoice(
    const ActRaiserRegionalRulesView *view, ArRegionalDifficultyChoice choice) {
  return RequestSettings(view,kSettingsDifficultyChoice,0,choice);
}

ActRaiserRegionalEditResult ActRaiserRegional_RequestProfile(
    const ActRaiserRegionalRulesView *view, ArRegionalProfileGroup group, ArRegionalSource source) {
  return RequestSettings(view,kSettingsProfile,group,source);
}

ActRaiserRegionalEditResult ActRaiserRegional_PreviewProfile(
    const ActRaiserRegionalRulesView *view, ArRegionalProfileGroup group,
    ArRegionalSource source, ActRaiserRegionalEditImpact *out) {
  const ActRaiserRegionalEditContext edit = CurrentEditor();
  const ActRaiserRegionalEditResult result=ActRaiserRegionalEditor_PreviewProfile(&edit,view,group,source,out);
  return s_settings_writer && s_title_open && s_title_saved && result==kActRaiserRegionalEdit_Deferred
      ? kActRaiserRegionalEdit_RequiresGame : result;
}

ActRaiserRegionalEditResult ActRaiserRegional_RequestRules(
    const ActRaiserRegionalRulesView *view, ActRaiserRegionalSettingGroup group, ArRegionalSource source) {
  return RequestSettings(view,kSettingsRules,group,source);
}

ActRaiserRegionalEditResult ActRaiserRegional_RequestDifficulty(
    const ActRaiserRegionalRulesView *view, ArRegionalDifficulty level) {
  return RequestSettings(view,kSettingsDifficulty,0,level);
}

uint8_t ActRaiserRegional_HazardSnapshot(void) { return s_action.hazards; }
uint8_t ActRaiserRegional_TerrainSnapshot(void) { return s_action.terrain; }
uint8_t ActRaiserRegional_MosaicSnapshot(void) { return s_action.mosaic; }
uint8_t ActRaiserRegional_PoseSnapshot(void) { return s_action.poses; }
uint8_t ActRaiserRegional_ArtworkSnapshot(void) { return s_action.artwork; }
bool ActRaiserRegional_BeginTitleArtwork(uint8_t *mask) {
  if(!mask)return false;
  if(!s_title_open)PrepareTitleDraft();
  uint8_t next=0;
  if(!ArRegionalSession_BeginTitleArtwork(&s_title_draft,&next))return false;
  s_title_artwork=next;*mask=next;return true;
}
uint8_t ActRaiserRegional_TitleArtworkSnapshot(void) { return s_title_open?s_title_artwork:0; }
uint8_t ActRaiserRegional_TownArtworkSnapshot(uint16_t scene) {
  return s_campaign.active_valid && scene==s_town_art_scene?s_town_artwork:0;
}
bool ActRaiserRegional_BeginTownArtwork(uint16_t scene,uint8_t *mask) {
  if(!mask || (scene&255) || scene<0x0100 || scene>0x0600)return false;
  uint8_t next=0;
  if(s_campaign.active_valid && !ArRegionalSession_BeginTownArtwork(&s_campaign.active,&next))return false;
  s_town_art_scene=scene;s_town_artwork=next;*mask=next;return true;
}
bool ActRaiserRegional_PlacementSnapshot(ArRegionalPlacementPolicy *policy,ArRegionalDifficulty *difficulty) {
  if(!policy || !difficulty)return false;
  *policy=s_action.placements;*difficulty=s_action.placement_difficulty;return true;
}

bool ActRaiserRegional_BeginSceneMusic(uint8_t *profile) {
  if(!profile)return false;
  if(!s_campaign.active_valid) {*profile=0;return true;}
  return ArRegionalSession_BeginMusic(&s_campaign.active,profile);
}
bool ActRaiserRegional_BeginSongSequence(unsigned rule,bool *enabled) {
  if(!enabled || rule>=kArRegionalSequence_Count)return false;
  if(!s_campaign.active_valid) {*enabled=false;return true;}
  return ArRegionalSession_BeginSequence(&s_campaign.active,rule,enabled);
}

bool ActRaiserRegional_BeginActionRoom(uint8_t profile, uint16_t native_bcd, uint16_t *out_bcd) {
  if (!out_bcd) return false;
  if (!s_campaign.active_valid) { s_action=(ArRegionalActionRoomSnapshot){0};ActRaiserStagePlacements_Reset();*out_bcd = native_bcd; return true; }
  if (!ArRegionalSession_BeginActionRoom(&s_campaign.active, profile, native_bcd, out_bcd, &s_action))
    return false;
  ActRaiserStagePlacements_Reset();
  ArRegionalSpellInventory_Interrupt(&s_inventory);
  s_inventory_icon_pending=s_inventory.enabled;
  /* Only an accepted retry/new-room initialization abandons an interrupted
   * clear sequence; an invalid request must not erase its one-shot marker. */
  s_completion_state = kScoreIdle;
  s_completion_scene = 0;
  if (s_trace) fprintf(stderr, "[regional] room profile=%02x time=%04x->%04x\n",
                       profile, native_bcd, *out_bcd);
  return true;
}

bool ActRaiserRegional_MiracleEntry(const CpuState *cpu) {
  return s_campaign.active_valid && !s_miracle_active && cpu &&
      ActRaiserMiracle_Entry(cpu, (uint8_t)cpu->A);
}

RecompReturn ActRaiserRegional_RunMiracle(CpuState *cpu) {
  if (!ActRaiserRegional_MiracleEntry(cpu) ||
      !ArRegionalSession_BeginCosts(&s_campaign.active, kArRegionalCostGroup_Miracles,
                                   &s_miracle_prices))
    ActRaiserHleFatal("Cannot capture regional miracle transaction");
  s_miracle_active = true;
  const unsigned action=(uint8_t)cpu->A;
  if (s_trace) {
    ArRegionalCostRule rule;
    ActRaiserMiracle_Rule(action,&rule);
    fprintf(stderr,"[regional] miracle action=%u price=%u SP=%u begin\n",
            action,s_miracle_prices.price[rule],cpu_read16(cpu,0,0x0282));
  }
  const RecompReturn result = ActRaiserMiracle_Run(cpu, (uint8_t)cpu->A, &s_miracle_prices);
  s_miracle_active = false;
  if (s_trace) fprintf(stderr,"[regional] miracle action=%u return=%u SP=%u\n",
                       action,(unsigned)result,cpu_read16(cpu,0,0x0282));
  return result;
}

bool ActRaiserRegional_ReplayDigest(void *unused, uint8_t out[32], bool *baseline) {
  (void)unused;
  if (!out || !baseline || (!s_campaign.active_valid && !s_boot_valid)) return false;
  const ArRegionalRules *requested = s_campaign.active_valid ? &s_campaign.active.requested : &s_boot_requested;
  const ArRegionalRules *effective = s_campaign.active_valid ? &s_campaign.active.effective : &s_boot_effective;
  const ArRegionalLairHistory *history = s_campaign.active_valid ? &s_campaign.active.lairs : &s_boot_lairs;
  const ArRegionalLairAccounting pending = ArRegionalRules_LairAccounting(requested), active = ArRegionalRules_LairAccounting(effective);
  const ArRegionalLairReloads *reloads=s_campaign.active_valid?&s_campaign.active.reloads:&s_boot_reloads;
  const ArRegionalSimActors *actors=s_campaign.active_valid?&s_campaign.active.sim_actors:&s_boot_sim_actors;
  uint8_t digest[32]; bool rules_native, lairs_native, reloads_native, actors_native, placements_native;
  if (!ArRegionalRules_Fingerprint(requested,effective,digest,&rules_native) ||
      !ArRegionalLairHistory_Fingerprint(digest,history,&pending,&active,digest,&lairs_native) ||
      !ArRegionalLairReloads_Fingerprint(digest,reloads,requested->lair_reloads,effective->lair_reloads,
                                        digest,&reloads_native) ||
      !ArRegionalSimActors_Fingerprint(digest,actors,digest,&actors_native) ||
      !ArRegionalArrivalLock_Fingerprint(digest,requested->arrival,effective->arrival,
          s_campaign.active_valid?s_campaign.active.arrival_locked:s_boot_arrival_locked,digest)) return false;
  if (s_campaign.active_valid && s_completion_state!=kScoreIdle && (!rules_native || !lairs_native)) {
    uint8_t completion[51]="ARSCORECLEAR-R1";
    memcpy(completion+16,digest,32);
    completion[48]=(uint8_t)s_completion_state;
    completion[49]=(uint8_t)s_completion_scene;
    completion[50]=(uint8_t)(s_completion_scene>>8);
    if (!sr_support_sha256(completion,sizeof(completion),digest)) return false;
  }
  if(!ArRegionalSpellInventory_Fingerprint(digest,&s_inventory,digest) ||
      !ActRaiserStagePlacements_Fingerprint(digest,digest,&placements_native))return false;
  /* The runner hashes hardware state, not this host-owned spawn policy. Its
   * version also distinguishes regional-base-first scaling from older runs.
   * Leave the identity case byte-for-byte compatible with existing replays. */
  const RandomizerStatScale scale=Randomizer_AppliedStatScale();
  const bool stats_native=scale.hp_percent==100 && scale.attack_percent==100;
  if(!stats_native) {
    uint8_t stats[56]="ARSTATBASE-R1";
    memcpy(stats+16,digest,32);
    ByteOrder_WriteLe32(stats+48,(uint32_t)scale.hp_percent);
    ByteOrder_WriteLe32(stats+52,(uint32_t)scale.attack_percent);
    if(!sr_support_sha256(stats,sizeof(stats),digest))return false;
  }
  const RandomizerConfig recipe=Randomizer_CurrentConfig();
  if(recipe.enabled) {
    uint8_t randomized[48+kRandomizerConfigBytes]="ARRANDSTATE-R1";
    memcpy(randomized+16,digest,32);
    if(!RandomizerConfig_Encode(&recipe,randomized+48) ||
        !sr_support_sha256(randomized,sizeof(randomized),digest))return false;
  }
  memcpy(out,digest,sizeof(digest));
  *baseline = rules_native && lairs_native && reloads_native && actors_native && placements_native && stats_native && !s_inventory.enabled && !recipe.enabled;
  return true;
}

bool ActRaiserRegional_ReportCommandEntry(const CpuState *cpu) {
  return s_campaign.active_valid && !s_report_command_active && cpu &&
      ActRaiserReportCommand_Entry(cpu, (uint8_t)cpu->A);
}
RecompReturn ActRaiserRegional_RunReportCommand(CpuState *cpu) {
  bool keep_open;
  if (!ActRaiserRegional_ReportCommandEntry(cpu) ||
      !ArRegionalSession_BeginMenuReturn(&s_campaign.active, &keep_open))
    ActRaiserHleFatal("Cannot capture regional command return");
  const unsigned action = (uint8_t)cpu->A;
  s_report_command_active = true;
  if (s_trace) fprintf(stderr, "[regional] report command=%u keep-open=%u begin\n", action, keep_open);
  const RecompReturn result = ActRaiserReportCommand_Run(cpu, action, keep_open);
  s_report_command_active = false;
  if (s_trace) fprintf(stderr, "[regional] report command=%u return=%u\n", action, (unsigned)result);
  return result;
}

bool ActRaiser_RegionalScrollEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserScrollCast_Entry(cpu) &&
      cpu_read16(cpu, 0, 0x02ac) <= 4;
}

bool ActRaiser_RegionalRetryEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserRetry_Entry(cpu);
}

bool ActRaiser_RegionalTownWaitEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserTownWait_Entry(cpu);
}

bool ActRaiser_RegionalFishingEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserFishing_Entry(cpu);
}

bool ActRaiser_RegionalRecoveryCycleEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserRecovery_CycleEntry(cpu);
}
bool ActRaiser_RegionalRecoveryDrainEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserRecovery_DrainEntry(cpu);
}
bool ActRaiser_RegionalRecoveryMotionEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserRecovery_MotionEntry(cpu);
}
static ArRegionalRecoverySnapshot CaptureRecovery(CpuState *cpu) {
  ArRegionalRecoverySnapshot snapshot;
  const bool pending = memcmp(&s_campaign.active.requested.recovery,
      &s_campaign.active.effective.recovery, sizeof(s_campaign.active.requested.recovery)) != 0;
  unsigned changed = 0;
  if (!(pending ? ArRegionalSession_BeginRecovery(&s_campaign.active, &snapshot, &changed) :
        ArRegionalRecovery_Resolve(&s_campaign.active.effective.recovery, &snapshot)))
    ActRaiserHleFatal("Cannot capture regional recovery policy");
  if (changed) ActRaiserRecovery_Reconcile(cpu, changed);
  if (s_trace && pending) fprintf(stderr, "[regional] recovery cycle-sp=%u angel-calls=%u retired=%u\n",
      snapshot.cycle_sp, snapshot.angel_calls, changed);
  return snapshot;
}
static RecompReturn RecoveryTransfer(RecompReturn result, uint32_t target, uint32_t source) {
  if (result != RECOMP_RETURN_NORMAL)
    return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
  if (!cpu_hle_tailcall_request(target, source))
    ActRaiserHleFatal("Regional recovery requires a generated wrapper");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalRecoveryCycle(CpuState *cpu) {
  if (!ActRaiser_RegionalRecoveryCycleEntry(cpu)) ActRaiserHleFatal("Invalid regional recovery cycle");
  const ArRegionalRecoverySnapshot snapshot = CaptureRecovery(cpu);
  return RecoveryTransfer(ActRaiserRecovery_Cycle(cpu, &snapshot), 0x038298, 0x038271);
}
RecompReturn ActRaiser_RegionalRecoveryDrain(CpuState *cpu) {
  if (!ActRaiser_RegionalRecoveryDrainEntry(cpu)) ActRaiserHleFatal("Invalid regional recovery drain");
  const ArRegionalRecoverySnapshot snapshot = CaptureRecovery(cpu);
  return RecoveryTransfer(ActRaiserRecovery_Drain(cpu, &snapshot), 0x01b281, 0x01b257);
}
static RecompReturn RecoveryMotion(CpuState *cpu, bool stopped) {
  if (!ActRaiser_RegionalRecoveryMotionEntry(cpu)) ActRaiserHleFatal("Invalid regional recovery motion");
  const ArRegionalRecoverySnapshot snapshot = CaptureRecovery(cpu);
  ActRaiserRecovery_Motion(cpu, &snapshot, stopped);
  return RecoveryTransfer(RECOMP_RETURN_NORMAL, stopped ? 0x019c3c : 0x019c32,
                          stopped ? 0x019c34 : 0x019c30);
}
RecompReturn ActRaiser_RegionalRecoveryMoving(CpuState *cpu) { return RecoveryMotion(cpu, false); }
RecompReturn ActRaiser_RegionalRecoveryStopped(CpuState *cpu) { return RecoveryMotion(cpu, true); }

bool ActRaiser_RegionalDevelopmentEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserDevelopment_MasterEntry(cpu);
}
bool ActRaiser_RegionalEffectEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserDevelopment_EffectEntry(cpu);
}
RecompReturn ActRaiser_RegionalDevelopment(CpuState *cpu) {
  if(!ActRaiser_RegionalDevelopmentEntry(cpu))ActRaiserHleFatal("Invalid regional development entry");
  ActivateLairAccounting(cpu);
  ActivateLairReloads(cpu);
  ArRegionalDevelopmentSnapshot snapshot;
  const bool new_cycle=cpu_read16(cpu,0,0x0347)!=7 &&
      !cpu_read16(cpu,0x7f,0x91fe) && !cpu_read16(cpu,0x7f,0x9200);
  const bool pending=memcmp(&s_campaign.active.requested.development,&s_campaign.active.effective.development,
                            sizeof(s_campaign.active.requested.development))!=0;
  if(!(new_cycle ? ArRegionalSession_BeginDevelopment(&s_campaign.active,&snapshot) :
       ArRegionalDevelopment_Resolve(&s_campaign.active.effective.development,&snapshot)))
    ActRaiserHleFatal("Cannot capture regional development cycle");
  if(s_trace && new_cycle && pending)fprintf(stderr,"[regional] development divider=%u cycle=%u effects=%u\n",
      snapshot.service_divider,snapshot.long_cycle,snapshot.effect_divider);
  return ActRaiserDevelopment_Master(cpu,&snapshot);
}
static RecompReturn RegionalEffect(CpuState *cpu,bool world_actors) {
  ArRegionalDevelopmentSnapshot snapshot;
  if(!ActRaiser_RegionalEffectEntry(cpu) ||
      !ArRegionalDevelopment_Resolve(&s_campaign.active.effective.development,&snapshot))
    ActRaiserHleFatal("Cannot capture regional effect service");
  return ActRaiserDevelopment_Effect(cpu,&snapshot,world_actors);
}
RecompReturn ActRaiser_RegionalEffect(CpuState *cpu) {return RegionalEffect(cpu,true);}
RecompReturn ActRaiser_RegionalEffectVisuals(CpuState *cpu) {return RegionalEffect(cpu,false);}

RecompReturn ActRaiser_RegionalFishing(CpuState *cpu) {
  uint16_t target, continuation;
  bool reconcile;
  if (!ActRaiser_RegionalFishingEntry(cpu) ||
      !ArRegionalSession_BeginFishing(&s_campaign.active, &target, &reconcile))
    ActRaiserHleFatal("Cannot capture regional fishing target");
  const RecompReturn result = ActRaiserFishing_Prefix(cpu, (uint8_t)target, reconcile, &continuation);
  if (result != RECOMP_RETURN_NORMAL)
    return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
  if (s_trace && (reconcile || continuation == 0xe895))
    fprintf(stderr, "[regional] fishing progress=%u target=%u complete=%u\n",
            cpu_read8(cpu,0x7f,0x916e), target, continuation == 0xe895);
  if (!cpu_hle_tailcall_request(0x030000u | continuation, 0x03e865))
    ActRaiserHleFatal("Regional fishing requires a generated wrapper");
  return RECOMP_RETURN_TAILCALL;
}

RecompReturn ActRaiser_RegionalTownWait(CpuState *cpu) {
  if (!ActRaiser_RegionalTownWaitEntry(cpu)) ActRaiserHleFatal("Invalid regional town wait");
  const unsigned town = cpu_read16(cpu, 0x7f, 0x7bfb);
  uint16_t reload = 0; /* Unused until expiry; no per-tick policy resolution. */
  if (cpu_read16(cpu, 0x7f, (uint16_t)(0x7ce1 + town)) == 1) {
    if (!ArRegionalSession_BeginTownWait(&s_campaign.active, &reload))
      ActRaiserHleFatal("Cannot capture regional town wait policy");
    if (s_trace) fprintf(stderr, "[regional] town=%u next-wait=%u\n", town / 2, reload);
  }
  return ActRaiserTownWait_Step(cpu, reload);
}

RecompReturn ActRaiser_RegionalRetry(CpuState *cpu) {
  if (!ActRaiser_RegionalRetryEntry(cpu)) ActRaiserHleFatal("Invalid regional checkpoint retry");
  const bool retry = cpu_read16(cpu, 0, 0x032c) != 0;
  bool clear_score = false;
  if (retry && !ArRegionalSession_BeginRetryScore(&s_campaign.active, &clear_score))
    ActRaiserHleFatal("Cannot capture regional checkpoint-retry policy");
  const uint16_t before = cpu_read16(cpu, 0, 0x001f);
  const uint16_t continuation = ActRaiserRetry_Prefix(cpu, clear_score);
  if (s_trace && retry) fprintf(stderr, "[regional] checkpoint retry score=%04x->%04x\n",
                                before, cpu_read16(cpu, 0, 0x001f));
  if (!cpu_hle_tailcall_request(continuation, 0x00981c))
    ActRaiserHleFatal("Regional checkpoint retry requires a generated wrapper");
  return RECOMP_RETURN_TAILCALL;
}

RecompReturn ActRaiser_RegionalScrollCast(CpuState *cpu) {
  ArRegionalCostSnapshot quote={0};
  if (!ActRaiser_RegionalScrollEntry(cpu) ||
      (!(s_inventory.enabled && cpu_read8(cpu,0,0x349)) &&
       !ArRegionalSession_BeginCosts(&s_campaign.active, kArRegionalCostGroup_Scrolls, &quote)))
    ActRaiserHleFatal("Cannot capture regional scroll transaction");
  const unsigned before = cpu_read8(cpu, 0, 0x21);
  const uint16_t continuation = ActRaiserScrollCast_Gate(cpu, &quote);
  if (s_trace) fprintf(stderr,"[regional] scroll spell=%u stock=%u->%u target=%04x\n",
      cpu_read16(cpu,0,0x02ac),before,cpu_read8(cpu,0,0x21),continuation);
  /* The surrounding native actor dispatch has pushed data below its original
   * host frame. Preserve the wrapper's inherited context, NOT current S.
   * The runtime owns that context and the wrapper still owns its own pop. */
  if (!cpu_hle_tailcall_request(continuation, 0x009de1))
    ActRaiserHleFatal("Regional scroll transfer requires a generated wrapper");
  return RECOMP_RETURN_TAILCALL;
}

static bool QuakeEffectEntry(CpuState *cpu, bool posted) {
  if (s_quake_delegate) { s_quake_delegate = false; return false; }
  return !s_quake_active && s_campaign.active_valid && cpu && cpu->PB == 1 && cpu->DB == 1 &&
      !cpu->D && !cpu->emulation && !cpu->x_flag && cpu->m_flag == !posted &&
      (posted ? cpu_read16(cpu, 0x7f, 0x90eb) == 4 : (uint8_t)cpu->A == 4);
}
bool ActRaiser_RegionalQuakePlayerEntry(CpuState *cpu) { return QuakeEffectEntry(cpu, false); }
bool ActRaiser_RegionalQuakePostedEntry(CpuState *cpu) { return QuakeEffectEntry(cpu, true); }
static RecompReturn QuakeEffect(CpuState *cpu, bool posted) {
  if (!QuakeEffectEntry(cpu, posted) || !ArRegionalSession_BeginQuake(&s_campaign.active, &s_quake))
    ActRaiserHleFatal("Cannot capture regional earthquake transaction");
  s_quake_active = s_quake_delegate = true;
  if (s_trace) memset(s_quake_selections, 0, sizeof(s_quake_selections));
  if (s_trace) fprintf(stderr, "[regional] quake origin=%s selectors=%u%u%u%u%u begin\n",
      posted ? "posted" : "player", s_quake.random[0], s_quake.random[1], s_quake.random[2],
      s_quake.random[3], s_quake.random[4]);
  /* Delegate the complete native effect with its original return frame.
   * The one-shot recursion gate leaves descendant selector hooks active. */
  RecompReturn result = posted ? bank_01_9840_M0X0(cpu) : bank_01_97E5_M1X0(cpu);
  s_quake_active = s_quake_delegate = false;
  if (s_trace) fprintf(stderr, "[regional] quake return=%u random-selectors=%u/%u/%u/%u/%u\n",
      (unsigned)result, s_quake_selections[0], s_quake_selections[1], s_quake_selections[2],
      s_quake_selections[3], s_quake_selections[4]);
  return result;
}
RecompReturn ActRaiser_RegionalQuakePlayer(CpuState *cpu) { return QuakeEffect(cpu, false); }
RecompReturn ActRaiser_RegionalQuakePosted(CpuState *cpu) { return QuakeEffect(cpu, true); }
static bool QuakeSelectorEntry(CpuState *cpu, ArRegionalQuakeRule rule) {
  return s_quake_active && s_quake.random[rule] && ActRaiserQuake_SelectorEntry(cpu);
}
static RecompReturn QuakeSelect(CpuState *cpu, ArRegionalQuakeRule rule) {
  if (!QuakeSelectorEntry(cpu, rule)) ActRaiserHleFatal("Earthquake selector outside its captured effect");
  if (s_trace) ++s_quake_selections[rule];
  uint32_t target;
  RecompReturn result = ActRaiserQuake_Select(cpu, rule, &target);
  if (result != RECOMP_RETURN_NORMAL)
    return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
  if (!cpu_hle_tailcall_request(target, ActRaiserQuake_SelectorPC(rule)))
    ActRaiserHleFatal("Earthquake selector has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
#define QUAKE_SELECTOR(name, rule) \
  bool ActRaiser_RegionalQuake##name##Entry(CpuState *cpu) { return QuakeSelectorEntry(cpu, rule); } \
  RecompReturn ActRaiser_RegionalQuake##name(CpuState *cpu) { return QuakeSelect(cpu, rule); }
QUAKE_SELECTOR(Houses, kArRegionalQuake_Houses)
QUAKE_SELECTOR(Fields, kArRegionalQuake_Fields)
QUAKE_SELECTOR(Class3, kArRegionalQuake_Class3)
QUAKE_SELECTOR(Class4, kArRegionalQuake_Class4)
QUAKE_SELECTOR(Class5, kArRegionalQuake_Class5)
#undef QUAKE_SELECTOR

bool ActRaiserRegional_SimActorsReady(void) { return s_campaign.active_valid; }
void ActRaiserRegional_SimActorCache(bool load,unsigned town) {
  if (!s_campaign.active_valid) return;
  const bool ok=load?ArRegionalSimActors_LoadTown(&s_campaign.active.sim_actors,town):
      ArRegionalSimActors_SaveTown(&s_campaign.active.sim_actors,town);
  if (s_trace) fprintf(stderr,"[regional] SIM cache %s town=%u captured=%u\n",load?"load":"save",town,ok);
}
void ActRaiserRegional_SimActorBirth(unsigned town,unsigned slot) {
  if (!s_campaign.active_valid) return;
  const bool ok=ArRegionalSession_BeginSimActor(&s_campaign.active,town,slot);
  if (s_trace) fprintf(stderr,"[regional] SIM birth town=%u slot=%u captured=%u combat=%u ai=%u\n",town,slot,ok,
      ok?s_campaign.active.sim_actors.active[slot].combat:0,ok?s_campaign.active.sim_actors.active[slot].ai:0);
}
bool ActRaiserRegional_SimActorSnapshot(unsigned town,unsigned slot,uint16_t *snapshot) {
  ArRegionalSimActorRules rules;
  if (!snapshot || !s_campaign.active_valid || !ArRegionalSimActors_Read(&s_campaign.active.sim_actors,town,slot,&rules)) return false;
  *snapshot=rules.combat;return true;
}
bool ActRaiserRegional_SimActorAiSnapshot(unsigned town,unsigned slot,uint16_t *snapshot) {
  ArRegionalSimActorRules rules;
  if (!snapshot || !s_campaign.active_valid || !ArRegionalSimActors_Read(&s_campaign.active.sim_actors,town,slot,&rules)) return false;
  *snapshot=rules.ai;return true;
}
bool ActRaiserRegional_LevelGoalsSnapshot(bool activate,bool *japanese) {
  if (!s_campaign.active_valid || !japanese) return false;
  return activate?ArRegionalSession_BeginLevelGoals(&s_campaign.active,japanese):
      ArRegionalLevelGoals_Resolve(s_campaign.active.effective.level_goals,japanese);
}

bool ActRaiserRegional_TownStatusSnapshot(bool activate, ArRegionalTownStatusSnapshot *out) {
  if (!s_campaign.active_valid || !out) return false;
  /* Unchanged batches need only their small value snapshot, not a complete
   * retained-history validation. Session validation remains on every change. */
  if (!activate || !memcmp(&s_campaign.active.requested.town_status,
                          &s_campaign.active.effective.town_status,sizeof(s_campaign.active.effective.town_status)))
    return ArRegionalTownStatus_Resolve(&s_campaign.active.effective.town_status,out);
  return ArRegionalSession_BeginTownStatus(&s_campaign.active,out);
}

bool ActRaiserRegional_ConstructionSnapshot(bool activate, bool *japanese) {
  if (!s_campaign.active_valid || !japanese) return false;
  if (!activate || s_campaign.active.requested.construction==s_campaign.active.effective.construction)
    return ArRegionalConstruction_Resolve(s_campaign.active.effective.construction,japanese);
  return ArRegionalSession_BeginConstruction(&s_campaign.active,japanese);
}

bool ActRaiser_RegionalStoryThresholdEntry(CpuState *cpu) {
  ArRegionalStoryRule rule;
  return s_campaign.active_valid && ActRaiserStory_ThresholdEntry(cpu,&rule);
}
RecompReturn ActRaiser_RegionalStoryThreshold(CpuState *cpu) {
  ArRegionalStoryRule rule;
  ArRegionalStorySnapshot snapshot;
  if (!s_campaign.active_valid || !ActRaiserStory_ThresholdEntry(cpu,&rule) ||
      !ArRegionalSession_BeginStory(&s_campaign.active,&snapshot) ||
      !ActRaiserStory_LoadThreshold(cpu,snapshot.value[rule]) ||
      !cpu_hle_tailcall_request(0x03e142,0x03e13e))
    ActRaiserHleFatal("Cannot resume native story prerequisite comparison");
  if (s_trace) fprintf(stderr,"[regional] story rule=%u threshold=%u population=%u gf=%u\n",
      (unsigned)rule,snapshot.value[rule],cpu_read16(cpu,0x7f,0x9202),cpu_read16(cpu,0,0x88));
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_RegionalStoryCompassEntry(CpuState *cpu) {
  if (s_story_compass_delegate) { s_story_compass_delegate=false; return false; }
  return s_campaign.active_valid && ActRaiserStory_CompassEntry(cpu);
}
RecompReturn ActRaiser_RegionalStoryCompass(CpuState *cpu) {
  ArRegionalStorySnapshot snapshot;
  if (!ActRaiser_RegionalStoryCompassEntry(cpu) ||
      !ArRegionalSession_BeginStory(&s_campaign.active,&snapshot))
    ActRaiserHleFatal("Cannot capture Bloodpool event prerequisite policy");
  if (s_trace) fprintf(stderr,"[regional] failed Bloodpool Act2 clear-compass=%u\n",
      snapshot.value[kArRegionalStory_ClearCompassPrerequisite]);
  if (snapshot.value[kArRegionalStory_ClearCompassPrerequisite]) {
    s_story_compass_delegate=true;
    const RecompReturn result=bank_03_EB35_M1X0(cpu);
    s_story_compass_delegate=false;
    return result;
  }
  if (!cpu_hle_tailcall_request(0x03eb3d,0x03eb35))
    ActRaiserHleFatal("Cannot resume native Bloodpool event rejection");
  return RECOMP_RETURN_TAILCALL;
}

static bool SkullShape(const CpuState *cpu, bool accumulator8) {
  return cpu && cpu->PB==1 && cpu->DB==1 && !cpu->D && !cpu->emulation &&
      cpu->m_flag==accumulator8 && !cpu->x_flag && !cpu->_flag_D && !(cpu->P & CPU_P_D);
}
bool ActRaiser_RegionalSkullUseEntry(CpuState *cpu) {
  if (s_skull_delegate) { s_skull_delegate=false; return false; }
  return !s_skull_active && s_campaign.active_valid && SkullShape(cpu,true);
}
RecompReturn ActRaiser_RegionalSkullUse(CpuState *cpu) {
  if (!ActRaiser_RegionalSkullUseEntry(cpu) ||
      !ArRegionalSession_BeginSkullWait(&s_campaign.active,&s_skull_frames))
    ActRaiserHleFatal("Cannot capture Magic Skull transaction");
  s_skull_active=s_skull_delegate=true;
  if (s_trace) fprintf(stderr,"[regional] Magic Skull post-effect wait=%u begin\n",s_skull_frames);
  RecompReturn result=bank_01_9EE7_M1X0(cpu);
  s_skull_active=s_skull_delegate=false;
  if (s_trace) fprintf(stderr,"[regional] Magic Skull return=%u\n",(unsigned)result);
  return result;
}
bool ActRaiser_RegionalSkullSkipWaitEntry(CpuState *cpu) {
  return s_skull_active && !s_skull_frames && SkullShape(cpu,false);
}
RecompReturn ActRaiser_RegionalSkullSkipWait(CpuState *cpu) {
  if (!ActRaiser_RegionalSkullSkipWaitEntry(cpu) || !cpu_hle_tailcall_request(0x019f87,0x019f80))
    ActRaiserHleFatal("Cannot resume native Magic Skull consumption");
  return RECOMP_RETURN_TAILCALL;
}

bool ActRaiser_RegionalSourceCollectionEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserSources_CollectionEntry(cpu);
}
RecompReturn ActRaiser_RegionalSourceCollection(CpuState *cpu) {
  ArRegionalSourcesSnapshot snapshot;
  if (!ActRaiser_RegionalSourceCollectionEntry(cpu) ||
      !ArRegionalSession_BeginSources(&s_campaign.active,&snapshot))
    ActRaiserHleFatal("Cannot capture Source collection policy");
  const uint8_t item=(uint8_t)cpu->A;
  const bool automatic=snapshot.automatic[item==5?kArRegionalSourceItem_Life:kArRegionalSourceItem_Magic];
  const uint32_t target=ActRaiserSources_CollectionRoute(cpu,automatic);
  if (s_trace) fprintf(stderr,"[regional] Source item=%u collection=%s\n",item,automatic?"automatic":"held");
  if (!cpu_hle_tailcall_request(target,0x018916))
    ActRaiserHleFatal("Source collection has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_RegionalSourceLifeKeepEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserSources_KeepCarried(cpu,5);
}
bool ActRaiser_RegionalSourceMagicKeepEntry(CpuState *cpu) {
  return s_campaign.active_valid && ActRaiserSources_KeepCarried(cpu,6);
}
static RecompReturn KeepSource(CpuState *cpu, uint8_t item) {
  if (!s_campaign.active_valid || !ActRaiserSources_KeepCarried(cpu,item))
    ActRaiserHleFatal("Source preservation outside automatic collection");
  /* The collected Source's native bonus and acknowledgement have completed.
   * Skip only removal of an older held Source. Sound, P restore, item pop and
   * native return-to-town continue normally. Explicit Use is never guarded. */
  if (s_trace) fprintf(stderr,"[regional] Source item=%u kept older held item\n",item);
  if (!cpu_hle_tailcall_request(item==5?0x019cd1:0x019cf3,item==5?0x019cce:0x019cf0))
    ActRaiserHleFatal("Source preservation has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalSourceLifeKeep(CpuState *cpu) { return KeepSource(cpu,5); }
RecompReturn ActRaiser_RegionalSourceMagicKeep(CpuState *cpu) { return KeepSource(cpu,6); }

bool ActRaiser_RegionalLivesDisplayEntry(CpuState *cpu) {
  if (s_lives_delegate) { s_lives_delegate = false; return false; }
  return s_campaign.active_valid && ActRaiserLivesDisplay_Entry(cpu) &&
      (s_campaign.active.requested.lives_display != s_campaign.active.effective.lives_display ||
       s_campaign.active.effective.lives_display == kArRegionalSource_Japan);
}
RecompReturn ActRaiser_RegionalLivesDisplay(CpuState *cpu) {
  if (!ActRaiser_RegionalLivesDisplayEntry(cpu)) ActRaiserHleFatal("Unsupported regional lives HUD entry");
  bool zero_based = s_campaign.active.effective.lives_display == kArRegionalSource_Japan;
  /* The stable per-frame path does not scan/validate the campaign history. */
  if (s_campaign.active.requested.lives_display != s_campaign.active.effective.lives_display) {
    if (!ArRegionalSession_BeginLivesDisplay(&s_campaign.active, &zero_based))
      ActRaiserHleFatal("Cannot activate regional lives HUD convention");
    if (s_trace) fprintf(stderr, "[regional] lives HUD zero-based=%u\n", zero_based);
  }
  if (!zero_based) {
    s_lives_delegate = true;
    RecompReturn result = bank_02_C280_M0X0(cpu);
    s_lives_delegate = false;
    return result;
  }
  ActRaiserLivesDisplay_DrawZeroBased(cpu);
  if (!cpu_hle_tailcall_request(0x02c2a4, 0x02c280))
    ActRaiserHleFatal("Lives HUD continuation has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}

static bool ReportShape(const CpuState *cpu) {
  return cpu && cpu->PB == 1 && cpu->DB == 1 && !cpu->D && !cpu->emulation &&
      cpu->m_flag && !cpu->x_flag;
}
bool ActRaiser_RegionalMasterReportEntry(CpuState *cpu) {
  if (s_report_delegate) { s_report_delegate = false; return false; }
  return !s_report_active && s_campaign.active_valid && ReportShape(cpu);
}
RecompReturn ActRaiser_RegionalMasterReport(CpuState *cpu) {
  if (!ActRaiser_RegionalMasterReportEntry(cpu) ||
      !ArRegionalSession_BeginScorePage(&s_campaign.active, &s_report_score_page))
    ActRaiserHleFatal("Cannot capture regional Master report");
  s_report_active = s_report_delegate = true;
  ActRaiserLevelGoalsRuntime_RefreshReport(cpu);
  if (s_trace) fprintf(stderr, "[regional] Master report score-page=%u begin\n", s_report_score_page);
  /* The native body still composes the report and owns inventory objects,
   * button release/press waits and cleanup. Only its optional branch changes. */
  RecompReturn result = bank_01_899B_M1X0(cpu);
  s_report_active = s_report_delegate = false;
  if (s_trace) fprintf(stderr, "[regional] Master report return=%u\n", (unsigned)result);
  return result;
}
bool ActRaiser_RegionalSkipScoreEntry(CpuState *cpu) {
  return s_report_active && !s_report_score_page && ReportShape(cpu);
}
RecompReturn ActRaiser_RegionalSkipScore(CpuState *cpu) {
  if (!ActRaiser_RegionalSkipScoreEntry(cpu) || !cpu_hle_tailcall_request(0x018a2f, 0x0189ee))
    ActRaiserHleFatal("Cannot resume native Master report cleanup");
  return RECOMP_RETURN_TAILCALL;
}

void ActRaiserRegional_ObserveInputRelease(CpuState *cpu) {
  /* Cheap unchanged path; no MMIO, policy walk, allocations or I/O per tick.
   * Native $A1 is debounced via $F6 and can be zero while Y is still held.
   * Observe the completed auto-joypad sample instead, without remapping it. */
  if (!cpu || !s_campaign.active_valid ||
      s_campaign.active.requested.magic_gesture == s_campaign.active.effective.magic_gesture)
    return;
  if (!ActRaiserMagicGesture_ControlsReleased(cpu_read16(cpu,0,0x4218))) return;
  bool up_attack;
  if (!ArRegionalSession_BeginMagicGesture(&s_campaign.active, true, &up_attack))
    ActRaiserHleFatal("Cannot activate regional magic gesture");
  if (s_trace) fprintf(stderr,"[regional] magic gesture=%s after release\n",
      up_attack ? "up+attack" : "A/X");
}

bool ActRaiser_RegionalMagicGestureEntry(CpuState *cpu) {
  bool up_attack;
  return s_campaign.active_valid && ActRaiserMagicGesture_Entry(cpu) &&
      ArRegionalMagicGesture_Resolve(s_campaign.active.effective.magic_gesture, &up_attack) && up_attack;
}
RecompReturn ActRaiser_RegionalMagicDedicated(CpuState *cpu) {
  if (!ActRaiser_RegionalMagicGestureEntry(cpu) ||
      !cpu_hle_tailcall_request(0x00984e,0x009843))
    ActRaiserHleFatal("Cannot bypass dedicated magic input for JP gesture");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalMagicAttack(CpuState *cpu) {
  const uint32_t target=ActRaiserMagicGesture_Attack(cpu);
  if (!cpu_hle_tailcall_request(target,0x009a6e))
    ActRaiserHleFatal("Regional ground-attack prefix has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}

bool ActRaiser_RegionalSpeedEntry(CpuState *cpu) {
  if (s_speed_delegate) { s_speed_delegate = false; return false; }
  return s_campaign.active_valid && !s_speed_active && ActRaiserSpeedSelector_Entry(cpu, false);
}
RecompReturn ActRaiser_RegionalSpeed(CpuState *cpu) {
  if (!ActRaiser_RegionalSpeedEntry(cpu) ||
      !ArRegionalSession_BeginSpeedRange(&s_campaign.active, &s_speed_maximum))
    ActRaiserHleFatal("Cannot capture regional message-speed range");
  s_speed_active = s_speed_delegate = true;
  ActRaiserLocalizationRuntime_SetMessageSpeedMaximum(s_speed_maximum);
  if (s_trace) fprintf(stderr, "[regional] speed range=0-%u stored=%u begin\n",
      s_speed_maximum, cpu_read8(cpu, 0, 0x0200));
  RecompReturn result = bank_01_8AF5_M1X0(cpu);
  s_speed_active = s_speed_delegate = s_speed_scale_delegate = false;
  if (s_trace) fprintf(stderr, "[regional] speed return=%u stored=%u\n",
      (unsigned)result, cpu_read8(cpu, 0, 0x0200));
  return result;
}
bool ActRaiser_RegionalSpeedPositionEntry(CpuState *cpu) {
  return s_speed_active && s_speed_maximum == 7 && ActRaiserSpeedSelector_Entry(cpu, true);
}
RecompReturn ActRaiser_RegionalSpeedPosition(CpuState *cpu) {
  ActRaiserSpeedSelector_Position(cpu, s_speed_maximum);
  if (!cpu_hle_tailcall_request(0x018b22, 0x018b18))
    ActRaiserHleFatal("Message-speed position has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_RegionalSpeedRightEntry(CpuState *cpu) {
  return s_speed_active && s_speed_maximum == 7 && ActRaiserSpeedSelector_Entry(cpu, false);
}
RecompReturn ActRaiser_RegionalSpeedRight(CpuState *cpu) {
  const uint32_t target = ActRaiserSpeedSelector_Right(cpu, s_speed_maximum);
  if (!cpu_hle_tailcall_request(target, 0x018b59))
    ActRaiserHleFatal("Message-speed movement has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
bool ActRaiser_RegionalSpeedScaleEntry(CpuState *cpu) {
  if (s_speed_scale_delegate) { s_speed_scale_delegate = false; return false; }
  return s_speed_active && s_speed_maximum == 7 && ActRaiserSpeedSelector_Entry(cpu, false) &&
      cpu->X == 0xfa98 && cpu_read16(cpu, 0, cpu->S + 1) == 0x8b04;
}
RecompReturn ActRaiser_RegionalSpeedScale(CpuState *cpu) {
  s_speed_scale_delegate = true;
  const RecompReturn result = bank_01_8C98_M1X0(cpu);
  s_speed_scale_delegate = false;
  if (result == RECOMP_RETURN_NORMAL) ActRaiserSpeedSelector_DrawNativeScale(cpu, s_speed_maximum);
  return result;
}

static void ReportLairDivergence(unsigned previous) {
  const unsigned changed=s_campaign.active.lairs.diverged_towns & ~previous;
  if (changed) fprintf(stderr,"[regional] unaccounted lair stock write; retained histories quarantined for towns=%02x\n",changed);
}
static void ReportReloadDivergence(unsigned previous) {
  const unsigned changed=s_campaign.active.reloads.diverged_towns & ~previous;
  if (changed) fprintf(stderr,"[regional] unaccounted lair delay write; retained delays quarantined for towns=%02x\n",changed);
}
void ActRaiserRegional_CheckLairHistory(CpuState *cpu) {
  if (!s_campaign.active_valid || s_lair_seed_pending) return;
  const unsigned before=s_campaign.active.lairs.diverged_towns;
  const ArRegionalLairAccounting active = ArRegionalRules_LairAccounting(&s_campaign.active.effective);
  ActRaiserLairHistory_Check(&s_campaign.active.lairs,cpu,&active);
  ReportLairDivergence(before);
  const unsigned reload_before=s_campaign.active.reloads.diverged_towns;
  ActRaiserLairReloads_Check(&s_campaign.active.reloads,cpu,s_campaign.active.effective.lair_reloads);
  ReportReloadDivergence(reload_before);
}
static void ActivateLairReloads(CpuState *cpu) {
  if (!s_campaign.active_valid || s_lair_seed_pending || s_reload_active || s_lair_active || s_quake_active || s_miracle_active ||
      s_campaign.active.requested.lair_reloads==s_campaign.active.effective.lair_reloads ||
      s_campaign.active.reloads.initialized_towns!=0x3f || s_campaign.active.reloads.diverged_towns ||
      !ActRaiserLairHistory_Entry(cpu)) return;
  ArRegionalSession next=s_campaign.active;
  if (!ArRegionalSession_BeginLairReloads(&next)) return;
  if (!ActRaiserLairReloads_Project(&next.reloads,cpu,s_campaign.active.effective.lair_reloads,
                                   next.effective.lair_reloads)) {
    const unsigned previous=s_campaign.active.reloads.diverged_towns;
    s_campaign.active.reloads=next.reloads;
    ReportReloadDivergence(previous);
    return;
  }
  s_campaign.active=next;
  if (s_trace) fprintf(stderr,"[regional] lair reload source=%u activated; running countdowns preserved\n",
                       (unsigned)next.effective.lair_reloads);
}
bool ActRaiser_RegionalLairReductionEntry(CpuState *cpu) {
  if (s_reload_delegate) { s_reload_delegate=false; return false; }
  return s_campaign.active_valid && !s_lair_seed_pending && !s_reload_active && s_campaign.active.reloads.initialized_towns &&
      ActRaiserLairReloads_Entry(cpu);
}
RecompReturn ActRaiser_RegionalLairReduction(CpuState *cpu) {
  const unsigned previous=s_campaign.active.reloads.diverged_towns;
  ArRegionalLairReloads candidate;
  unsigned town;
  const ArRegionalSource source=s_campaign.active.effective.lair_reloads;
  const bool captured=ActRaiserLairReloads_BeginReduction(&s_campaign.active.reloads,cpu,source,&candidate,&town);
  s_reload_active=s_reload_delegate=true;
  const RecompReturn result=cpu->m_flag?bank_03_B6BF_M1X0(cpu):bank_03_B6BF_M0X0(cpu);
  s_reload_active=s_reload_delegate=false;
  if (captured) {
    const bool matched=ActRaiserLairReloads_EndReduction(&s_campaign.active.reloads,cpu,source,&candidate,town,result);
    if (s_trace) fprintf(stderr,"[regional] native delay reduction town=%u matched=%u\n",town,matched);
  }
  ReportReloadDivergence(previous);
  return result;
}
static void ActivateLairAccounting(CpuState *cpu) {
  if (s_completion_state != kScoreIdle || s_lair_seed_pending || s_lair_active || s_quake_active || s_miracle_active || !s_campaign.active_valid ||
      ArRegionalRules_SameAccounting(&s_campaign.active.requested,&s_campaign.active.effective) ||
      s_campaign.active.lairs.initialized_towns != 0x3f || s_campaign.active.lairs.diverged_towns ||
      !ActRaiserLairHistory_ProjectionEntry(cpu)) return;
  ArRegionalSession next = s_campaign.active;
  ArRegionalLairAccounting target;
  if (!ArRegionalSession_BeginLairAccounting(&next,&target)) return;
  const ArRegionalLairAccounting current = ArRegionalRules_LairAccounting(&s_campaign.active.effective);
  const unsigned previous = s_campaign.active.lairs.diverged_towns;
  const ArRegionalLairProjectionResult result = ActRaiserLairHistory_Project(&next.lairs,cpu,&current,&target);
  if (result == kArRegionalLairProjection_Ready) {
    s_campaign.active = next;
    if (s_trace) fprintf(stderr,"[regional] retained lair projection activated: seeds=%u house=%u score=%u/%u/%u\n",
        (unsigned)target.seeds, (unsigned)target.house_credit, (unsigned)target.score_conversion,
        (unsigned)target.score_operation, (unsigned)target.score_route);
  } else {
    s_campaign.active.lairs = next.lairs; /* Preserve mismatch evidence, not the staged policy. */
    ReportLairDivergence(previous);
  }
}
bool ActRaiser_RegionalLairEntry(CpuState *cpu) {
  if (s_lair_delegate) { s_lair_delegate=false; return false; }
  return s_campaign.active_valid && !s_lair_seed_pending && s_campaign.active.lairs.initialized_towns &&
      ActRaiserLairHistory_Entry(cpu);
}
bool ActRaiser_RegionalLairSeedEntry(CpuState *cpu) {
  if (s_lair_delegate) { s_lair_delegate=false; return false; }
  return s_campaign.active_valid && s_lair_seed_pending && ActRaiserLairHistory_Entry(cpu);
}
static void TryInitializeLairs(CpuState *cpu) {
  if (!s_lair_seed_pending) return;
  ArRegionalSession next=s_campaign.active;
  /* Draft histories are exact initial values but cannot be observed/projected
   * until the native new-game initializer has established its baseline. */
  next.lairs=(ArRegionalLairHistory){0};next.reloads=(ArRegionalLairReloads){0};
  if (ActRaiserLairHistory_Initialize(&next.lairs,cpu) && ActRaiserLairReloads_Initialize(&next.reloads,cpu)) {
    s_campaign.active=next;
    s_lair_seed_pending=false;
    if (s_trace) fprintf(stderr,"[regional] exact lair histories initialized for new campaign\n");
  }
}
RecompReturn ActRaiser_RegionalLairSeed(CpuState *cpu) {
  s_lair_delegate=true;
  const RecompReturn result=cpu->m_flag ? bank_03_B7C6_M1X0(cpu) : bank_03_B7C6_M0X0(cpu);
  s_lair_delegate=false;
  if (result==RECOMP_RETURN_NORMAL) TryInitializeLairs(cpu);
  return result;
}
static RecompReturn ObserveLairs(CpuState *cpu, ActRaiserLairEvent event,
                                 ActRaiserNativeLeaf m0, ActRaiserNativeLeaf m1) {
  const unsigned previous=s_campaign.active.lairs.diverged_towns;
  ActRaiserLairCapture capture;
  const ArRegionalLairAccounting active = ArRegionalRules_LairAccounting(&s_campaign.active.effective);
  const bool observed=ActRaiserLairHistory_Begin(&s_campaign.active.lairs,cpu,event,
                                               &active,&capture);
  const bool was_active = s_lair_active;
  const bool was_score = s_score_active;
  const ArRegionalScoreSnapshot previous_score = s_score;
  if (event==kActRaiserLairEvent_Score) {
    if (!ArRegionalScore_Resolve(&s_campaign.active.effective.score_feedback,&s_score))
      ActRaiserHleFatal("Invalid regional score-feedback snapshot");
    s_score_active=true;
  }
  s_lair_active=true;
  s_lair_delegate=true;
  const RecompReturn result=cpu->m_flag ? m1(cpu) : m0(cpu);
  s_lair_delegate=false;
  s_lair_active=was_active;
  s_score_active=was_score;
  s_score=previous_score;
  if (observed) {
    const bool matched=ActRaiserLairHistory_End(&s_campaign.active.lairs,cpu,&capture,result);
    if (s_trace) fprintf(stderr,"[regional] lair event=%u town=%u candidates=%x matched=%u\n",
        (unsigned)event,capture.town,capture.candidates,matched);
  }
  ReportLairDivergence(previous);
  return result;
}
RecompReturn ActRaiser_RegionalLairKill(CpuState *cpu) {
  return ObserveLairs(cpu,kActRaiserLairEvent_Kill,bank_03_BADD_M0X0,bank_03_BADD_M1X0);
}
RecompReturn ActRaiser_RegionalLairMiracle(CpuState *cpu) {
  return ObserveLairs(cpu,kActRaiserLairEvent_Miracle,bank_03_BA42_M0X0,bank_03_BA42_M1X0);
}
RecompReturn ActRaiser_RegionalLairHouse(CpuState *cpu) {
  return ObserveLairs(cpu,kActRaiserLairEvent_House,bank_03_B4A6_M0X0,bank_03_B4A6_M1X0);
}
bool ActRaiser_RegionalHouseUnitsEntry(CpuState *cpu) {
  return ActRaiserLairHistory_Entry(cpu) && !cpu->m_flag && s_campaign.active_valid &&
      s_campaign.active.effective.house_credit == kArRegionalSource_Japan;
}
RecompReturn ActRaiser_RegionalHouseUnits(CpuState *cpu) {
  if (!ActRaiser_RegionalHouseUnitsEntry(cpu) || !ActRaiserLairHouse_Units(cpu, false))
    ActRaiserHleFatal("Unsupported regional house-credit prefix");
  if (!cpu_hle_tailcall_request(0x03b4bc, 0x03b4b8))
    ActRaiserHleFatal("Regional house-credit prefix has no native return owner");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalLairScore(CpuState *cpu) {
  /* The completed-act settlement precedes the next development tick. Activate
   * here too, before capture, so an action-mode request covers this score. */
  ActivateLairAccounting(cpu);
  return ObserveLairs(cpu,kActRaiserLairEvent_Score,bank_03_D095_M0X0,bank_03_D095_M1X0);
}
static bool ScoreEntry(CpuState *cpu, ArRegionalScoreRule rule) {
  return s_score_active && s_score.japanese[rule] && ActRaiserScoreFeedback_Entry(cpu);
}
bool ActRaiser_RegionalScoreRouteEntry(CpuState *cpu) { return ScoreEntry(cpu,kArRegionalScore_Route); }
bool ActRaiser_RegionalScoreConversionEntry(CpuState *cpu) { return ScoreEntry(cpu,kArRegionalScore_Conversion); }
bool ActRaiser_RegionalScoreSubtractEntry(CpuState *cpu) { return ScoreEntry(cpu,kArRegionalScore_Operation); }
static RecompReturn ScoreTransfer(bool valid, uint32_t target, uint32_t origin) {
  if (!valid || !cpu_hle_tailcall_request(target,origin))
    ActRaiserHleFatal("Regional score-feedback prefix violated its native contract");
  return RECOMP_RETURN_TAILCALL;
}
RecompReturn ActRaiser_RegionalScoreRoute(CpuState *cpu) {
  uint32_t target=0;
  const bool valid=ActRaiserScoreFeedback_Route(cpu,true,&target);
  return ScoreTransfer(valid,target,0x03d0b3);
}
RecompReturn ActRaiser_RegionalScoreConversion(CpuState *cpu) {
  return ScoreTransfer(ActRaiserScoreFeedback_ConvertJP(cpu),0x03d10a,0x03d0d4);
}
RecompReturn ActRaiser_RegionalScoreSubtract(CpuState *cpu) {
  return ScoreTransfer(ActRaiserScoreFeedback_Subtract(cpu),0x03b549,0x03b525);
}

static bool ScoreSceneEntry(const CpuState *cpu) {
  return s_campaign.active_valid && cpu && cpu->PB==0 && cpu->DB==0 &&
      !cpu->D && !cpu->emulation && !cpu->m_flag && !cpu->x_flag &&
      !cpu->_flag_D && !(cpu->P & CPU_P_D);
}
bool ActRaiser_RegionalScoreCardEntry(CpuState *cpu) {
  return ScoreSceneEntry(cpu);
}
RecompReturn ActRaiser_RegionalScoreCard(CpuState *cpu) {
  if (!ScoreSceneEntry(cpu)) ActRaiserHleFatal("Unsupported regional score-card entry");
  const uint16_t scene=cpu_read16(cpu,0,0x0018);
  if (s_completion_state==kScoreIdle || s_completion_scene!=scene) {
    s_completion_state=kScoreIdle;
    ActivateLairAccounting(cpu);
    ArRegionalScoreSnapshot policy;
    if (!ArRegionalSession_BeginScoreCompletion(&s_campaign.active,&policy))
      ActRaiserHleFatal("Cannot capture regional score-completion policy");
    s_completion_scene=scene;
    s_completion_state=policy.japanese[kArRegionalScore_Phase] ? kScoreSettledAtCard : kScoreAwaitDeparture;
    if (s_trace) fprintf(stderr,"[regional] clear-card score phase=%s scene=%04x score=%04x\n",
        s_completion_state==kScoreSettledAtCard?"card":"departure",scene,cpu_read16(cpu,0,0x001f));
    if (s_completion_state==kScoreSettledAtCard) {
      /* Insert the JP settlement at the US equivalent of JP00:A713, after
       * count/text publication and before the display object is finalized.
       * A754 is the audited resume boundary (preceding JSL ends at A753). */
      const RecompReturn result=ActRaiserNativeCall(cpu,bank_03_D095_M0X0,3,0xa753,true);
      if (result!=RECOMP_RETURN_NORMAL) return result;
    }
  }
  const RecompReturn result=ActRaiserNativeCall(cpu,bank_00_85B7_M0X0,0,0xa756,false);
  if (result!=RECOMP_RETURN_NORMAL) return result;
  return ScoreTransfer(true,0x00a757,0x00a754);
}
bool ActRaiser_RegionalScoreDepartureEntry(CpuState *cpu) {
  return ScoreSceneEntry(cpu);
}
RecompReturn ActRaiser_RegionalScoreDeparture(CpuState *cpu) {
  if (!ScoreSceneEntry(cpu)) ActRaiserHleFatal("Unsupported regional score-departure entry");
  const bool settled=s_completion_state==kScoreSettledAtCard &&
      s_completion_scene==cpu_read16(cpu,0,0x0018);
  if (!settled) {
    const RecompReturn result=ActRaiserNativeCall(cpu,bank_03_D095_M0X0,3,0xa310,true);
    if (result!=RECOMP_RETURN_NORMAL) return result;
  }
  if (s_trace) fprintf(stderr,"[regional] departure score settlement=%s score=%04x\n",
      settled?"already completed at card":"completed now",cpu_read16(cpu,0,0x001f));
  s_completion_state=kScoreIdle;
  s_completion_scene=0;
  ActRaiserStagePlacements_Reset();
  return ScoreTransfer(true,0x00a311,0x00a30d);
}

void ActRaiserRegional_SetContinuePrompt(ActRaiserRegionalContinuePrompt prompt, void *context) {
  s_continue_prompt = prompt;
  s_continue_context = context;
}

bool ActRaiser_RegionalContinueEntry(CpuState *cpu) {
  /* Record/replay sessions retain their recorded pre-feature path. Missing
   * history stays unavailable: no synthetic consent or companion mutation. */
  return s_continue_prompt && InputReplay_PolicyChangesAllowed() && cpu &&
      cpu->PB == 2 && cpu->DB == 2 && cpu->m_flag && !cpu->x_flag &&
      !cpu->D && !cpu->emulation && cpu_read8(cpu, 0, 0x0336) == 1;
}

static bool PrepareContinue(void) {
  s_population.intent.pending=false;
  bool acknowledged = false;
  for (;;) {
    uint8_t image[kActRaiserSramSize];
    SaveError error = {{0}};
    if (!SaveSystem_ValidateActive(&error) || !SaveSystem_CopyDurableImage(image) ||
        !ArRegionalCampaign_Continue(&s_campaign, SaveSystem_ActivePath(), image, &error)) {
      s_campaign.active_valid = false;
      fprintf(stderr, "[regional] Continue preserved saves: %s\n", error.message);
      if (!s_continue_prompt(s_continue_context, kActRaiserRegionalContinue_LoadFailed)) return false;
      continue;
    }
    if (s_campaign.active.lairs.initialized_towns == 0x3f) {
      if (s_campaign.active.reloads.initialized_towns) return true;
      /* Stock-history acknowledgement also covers old, unknowable delay
       * history. Upgrade the bound companion once; never modify SRAM. */
      const SaveFileFormat format=SaveSystem_ActiveBackend()==kSaveBackend_Ini?kSaveFileFormat_Ini:kSaveFileFormat_NativeSrm;
      if (ActRaiserLairReloads_AdoptSaved(&s_campaign.active.reloads,image) &&
          ArRegionalSession_Save(&s_campaign.active,format,SaveSystem_ActivePath(),image,image,&error) &&
          SaveSystem_ValidateActive(&error)) return true;
      s_campaign.active_valid=false;
      if (!s_continue_prompt(s_continue_context,kActRaiserRegionalContinue_SaveFailed)) return false;
      continue;
    }
    if (!acknowledged && !s_continue_prompt(s_continue_context, kActRaiserRegionalContinue_Estimate)) {
      s_campaign.active_valid = false;
      return false;
    }
    acknowledged = true;
    uint16_t stocks[kArRegionalLairCount];
    const SaveFileFormat format = SaveSystem_ActiveBackend() == kSaveBackend_Ini ?
        kSaveFileFormat_Ini : kSaveFileFormat_NativeSrm;
    if (ActRaiserLairHistory_ReadSavedStocks(image, stocks) &&
        ArRegionalCampaign_AcknowledgeLairHistory(&s_campaign, format,
            SaveSystem_ActivePath(), image, stocks, &error)) continue;
    s_campaign.active_valid = false;
    fprintf(stderr, "[regional] Continue history was not saved: %s\n", error.message);
    if (!s_continue_prompt(s_continue_context, kActRaiserRegionalContinue_SaveFailed)) return false;
  }
}

RecompReturn ActRaiser_RegionalContinue(CpuState *cpu) {
  if (PrepareContinue()) {
    if(!Randomizer_BindCampaign(&s_campaign.active.randomizer))
      ActRaiserHleFatal("Cannot restore this save's randomizer recipe; saves preserved");
    if (s_trace) fprintf(stderr, "[regional] Continue history ready before native restoration\n");
    const RecompReturn result = ActRaiserNativeCall(cpu, bank_03_A83A_M1X0, 3, 0xa7a2, true);
    if (result != RECOMP_RETURN_NORMAL) return result;
    if (!cpu_hle_tailcall_request(0x02a7a3, 0x02a79f))
      ActRaiserHleFatal("Continue acceptance has no native title owner");
    return RECOMP_RETURN_TAILCALL;
  }
  if (s_trace) fprintf(stderr, "[regional] Continue cancelled before native restoration\n");
  /* The title frame remains live. Wait for the accepting button to be released
   * so a held B/Start cannot immediately reopen a cancelled prompt. */
  do {
    const RecompReturn result = ActRaiserNativeCall(cpu, ActRaiser_WaitForVblank, 2, 0xa75d, false);
    if (result != RECOMP_RETURN_NORMAL) return result;
  } while (cpu_read8(cpu, 0, 0x4219) & 0xd0);
  if (!cpu_hle_tailcall_request(0x02a75b, 0x02a79f))
    ActRaiserHleFatal("Continue cancellation has no native title owner");
  return RECOMP_RETURN_TAILCALL;
}

bool ActRaiser_RegionalTitleEntry(CpuState *cpu) {
  if (s_delegate) {
    s_delegate = false;
    return false;
  }
  return cpu && cpu->PB == 2 && cpu->m_flag && !cpu->x_flag &&
      cpu->D == 0 && !cpu->emulation;
}

static void PrepareTitleDraft(void) {
  Randomizer_ReleaseCampaign();
  s_population.intent.pending=false;
  ArRegionalCampaign draft;
  ArRegionalCampaign_Init(&draft,s_campaign.slot,s_campaign.identity,s_campaign.identity_context);
  ArRegionalCostPolicy defaults;ArRegionalCosts_Init(&defaults,kArRegionalSource_US);
  SaveError error={{0}};
  if(!ArRegionalCampaign_NewGame(&draft,&defaults,&error))
    ActRaiserHleFatal("Cannot prepare new-game rules; saves preserved: %s",error.message);
  for(unsigned town=0;town<6;++town)
    if(!ArRegionalLairHistory_InitTown(&draft.active.lairs,town))
      ActRaiserHleFatal("Cannot prepare exact new-game lair history");
  if(!ArRegionalLairReloads_Init(&draft.active.reloads))
    ActRaiserHleFatal("Cannot prepare exact new-game lair reloads");
  if(s_return_rules_valid) {
    draft.active.requested=s_return_rules;
    /* Empty new towns need no destructive conversion. Keep coupled starting
     * support/goals coherent; stock projections still await native seeding. */
    draft.active.effective.support=s_return_rules.support;
    draft.active.effective.level_goals=s_return_rules.level_goals;
    draft.active.effective.story=s_return_rules.story;
    s_return_rules_valid=false;
  }
  if(s_prepared_new_game) {
    draft.active.requested=s_prepared_rules;
    draft.active.effective.support=s_prepared_rules.support;
    draft.active.effective.level_goals=s_prepared_rules.level_goals;
    draft.active.effective.story=s_prepared_rules.story;
    s_prepared_new_game=false;
  }
  s_title_saved=false;
  uint8_t image[kActRaiserSramSize];
  if(s_settings_writer && SaveSystem_CopyDurableImage(image)) {
    if(!SaveSystem_ValidateActive(&error) ||
        !ArRegionalCampaign_Continue(&draft,SaveSystem_ActivePath(),image,&error))
      ActRaiserHleFatal("Cannot restore title settings; saves preserved: %s",error.message);
    s_title_saved=true;
  }
  s_title_draft=draft.active;s_title_artwork=0;s_title_open=true;
}
RecompReturn ActRaiser_RegionalTitle(CpuState *cpu) {
  /* Normal entry follows the title asset script, which prepares this draft
   * before its three artwork planes. Keep direct/debug entry safe too. */
  if(!s_title_open)PrepareTitleDraft();
  /* The native title routine owns selection, checksum validation, restoration
   * and fades. Its normal return is accepted entry, including the no-save
   * path (which bypasses Continue's selection loop). Action/SIM assets have
   * not run yet; the title graphics already belong to this draft.
   * The original JSL frame remains owned by the generated routine. */
  s_delegate = true;
  RecompReturn result = bank_02_A622_M1X0(cpu);
  s_delegate = false;
  s_title_open=false;
  if (result != RECOMP_RETURN_NORMAL) return result;
  SaveError error={{0}};
  bool ok;
  const unsigned selection = cpu_read8(cpu, 0, 0x0336);
  if (selection == 1) {
    uint8_t image[kActRaiserSramSize];
    ok = SaveSystem_CopyDurableImage(image) &&
        ArRegionalCampaign_Continue(&s_campaign, SaveSystem_ActivePath(), image, &error);
    /* Record/replay deliberately skips consent and disk writes. Only an
     * already acknowledged older companion can acquire an in-memory estimate. */
    if (ok && s_campaign.active.lairs.initialized_towns==0x3f && !s_campaign.active.reloads.initialized_towns)
      ok=ActRaiserLairReloads_AdoptSaved(&s_campaign.active.reloads,image);
  } else if (selection == 0 || selection == 2) {
    if(s_title_saved) {
      /* New Game inherits choices, never the occupied slot's identity, lair
       * history or active actors. Continue remains that saved campaign. */
      const ArRegionalRules starting=s_title_draft.requested;
      ArRegionalCampaign fresh;
      ArRegionalCampaign_Init(&fresh,s_campaign.slot,s_campaign.identity,s_campaign.identity_context);
      const ArRegionalCostPolicy defaults={{0}};
      if(!ArRegionalCampaign_NewGame(&fresh,&defaults,&error))
        ActRaiserHleFatal("Cannot initialize new campaign: %s",error.message);
      for(unsigned town=0;town<6;++town)
        if(!ArRegionalLairHistory_InitTown(&fresh.active.lairs,town))ActRaiserHleFatal("Cannot initialize new lair history");
      if(!ArRegionalLairReloads_Init(&fresh.active.reloads))ActRaiserHleFatal("Cannot initialize new lair delays");
      s_title_draft=fresh.active;s_title_draft.requested=starting;
    }
    RandomizerConfig recipe;
    ArRegionalRules rules;
    if(!Randomizer_CaptureConfig(&recipe) ||
        !ArRegionalRandomizer_Choose(&s_title_draft.requested,&recipe,&rules))
      ActRaiserHleFatal("Cannot prepare seeded new-game rules; saves preserved");
    s_title_draft.randomizer=recipe;
    s_title_draft.requested=rules;
    /* New, empty towns need no reset. Numerical support and its reachable
     * goals start together; other families retain their native boundaries. */
    s_title_draft.effective.support=rules.support;
    s_title_draft.effective.level_goals=rules.level_goals;
    s_title_draft.effective.story=rules.story;
    s_campaign.active=s_title_draft;
    s_campaign.active_valid=true;
    ok=true;
  } else {
    ActRaiserHleFatal("Unknown accepted title selection: %u", selection);
  }
  if (!ok) ActRaiserHleFatal("Cannot enter regional campaign; saves preserved: %s",
                            error.message[0] ? error.message : "no durable save image");
  if(!s_campaign.active.randomizer.generator) {
    /* A historical seed cannot be inferred from SRAM or current preferences.
     * Preserve the saved regional rules; never roll a developed campaign. */
    s_campaign.active.randomizer=RandomizerConfig_Default();
    fprintf(stderr,"[randomizer] legacy save has no recorded seed; using unrandomized content\n");
  }
  if(!Randomizer_BindCampaign(&s_campaign.active.randomizer))
    ActRaiserHleFatal("Cannot bind this campaign's randomizer recipe; saves preserved");
  s_lair_seed_pending=selection!=1;
  s_town_art_scene=0;s_town_artwork=0;
  ArRegionalSpellInventory_Reset(&s_inventory,false);s_inventory_icon_pending=false;
  s_action=(ArRegionalActionRoomSnapshot){0};ActRaiserStagePlacements_Reset();
  s_completion_state=kScoreIdle;
  s_completion_scene=0;
  if (s_lair_seed_pending) TryInitializeLairs(cpu);
  else ActRaiserRegional_CheckLairHistory(cpu);
  s_prices_valid = false; /* a different campaign can have the same revision */
  fprintf(stderr, "[regional] %s campaign rules session ready\n",
          selection == 1 ? "continued" : "new");
  return result;
}
