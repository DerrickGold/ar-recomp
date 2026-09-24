#include "save_slot_manager.h"
#include "actraiser/regional/actraiser_regional_editor.h"
#include "regional/regional_randomizer.h"
#include <stdio.h>
#include <string.h>

bool SaveSlotManager_View(const ArRegionalSession *s,bool draft,ActRaiserRegionalRulesView *out) {
  if(!s || !out)return false;
  ActRaiserRegionalRulesView view=(ActRaiserRegionalRulesView){.revision=s->revision,.requested=s->requested,
    .effective=s->effective,.new_game=draft,.editable=draft,
    .lair_history_ready=s->lairs.initialized_towns==63 && !s->lairs.diverged_towns,
    .lair_reload_ready=s->reloads.initialized_towns==63 && !s->reloads.diverged_towns,
    .lair_history_estimated=s->lairs.approximate_towns!=0,.lair_reload_estimated=s->reloads.approximate_towns!=0,
    .arrival_locked=s->arrival_locked};
  memcpy(view.campaign,s->campaign,16);
  if(!ArRegionalProfiles_Describe(&s->requested,view.profiles) ||
      !ArRegionalProfiles_Describe(&s->effective,view.active_profiles) ||
      !ArRegionalProfiles_Changes(&s->requested,&s->effective,&view.pending_groups))return false;
  ActRaiserRegionalSettings_DescribeChoices(&s->requested,&s->effective,view.choices);
  *out=view;
  return true;
}
bool SaveSlotManager_Inspect(const SaveSlots *s,unsigned slot,SaveSlotDetails *out) {
  if(!out)return false;
  *out=(SaveSlotDetails){.state=kSaveSlot_Unavailable};
  if(!s || slot>=kSaveSlotCount) {
    snprintf(out->error.message,sizeof(out->error.message),"Invalid save slot.");
    return false;
  }
  SaveSlotInspection inspection;
  if(!SaveSlots_Inspect(s,slot,&inspection)){out->error=inspection.error;return false;}
  out->fingerprint=inspection.fingerprint;out->prepared=s->records[slot].prepared;
  if(inspection.state==kSaveSlot_Empty) {
    if(out->prepared) {
      ArRegionalSession draft;
      if(!SaveSlotManager_ReadDraft(s,slot,&draft,&out->error) ||
          !SaveSlotManager_View(&draft,true,&out->regions))return false;
      out->randomizer=draft.randomizer;
    }
    out->state=kSaveSlot_Empty;return true;
  }
  ArRegionalSession session;
  SaveCheckpointStatus status=ArRegionalSession_Load(&session,slot,inspection.path,inspection.image,&out->error);
  if(status==kSaveCheckpoint_Missing) {
    if(s->records[slot].checkpoint_required) {
      snprintf(out->error.message,sizeof(out->error.message),"The required campaign checkpoint is missing. Restore it before loading.");
      return false;
    }
    const uint8_t id[16]={1};const ArRegionalCostPolicy baseline={{0}};
    if(!ArRegionalSession_NewGame(&session,slot,id,&baseline))return false;
    out->legacy=true;
  } else if(status!=kSaveCheckpoint_Ready)return false;
  out->legacy=out->legacy || !session.randomizer.generator;
  if(!Save_ReadSummary(inspection.path,inspection.image,&out->summary) ||
      !SaveSlotManager_View(&session,false,&out->regions)) {
    snprintf(out->error.message,sizeof(out->error.message),"Cannot interpret this save's campaign details.");
    return false;
  }
  out->randomizer=session.randomizer;out->saved_at=inspection.modified_at;
  out->approximate_time=inspection.approximate_time;out->state=kSaveSlot_Ready;
  return true;
}
bool SaveSlotManager_Draft(ArRegionalSession *out,unsigned slot,const uint8_t id[16],
    const ArRegionalRules *rules,const RandomizerConfig *recipe) {
  if(!out || !rules || !recipe || !recipe->generator || slot>=kSaveSlotCount)return false;
  ArRegionalSession next;const ArRegionalCostPolicy baseline={{0}};
  if(!ArRegionalSession_NewGame(&next,slot,id,&baseline))return false;
  next.requested=*rules;next.effective=*rules;next.randomizer=*recipe;
  for(unsigned town=0;town<6;++town)if(!ArRegionalLairHistory_InitTown(&next.lairs,town))return false;
  if(!ArRegionalLairReloads_Init(&next.reloads))return false;
  ArRegionalRules rolled;
  if(!ArRegionalRandomizer_Choose(rules,recipe,&rolled))return false;
  uint8_t bytes[kSaveSlotDraftCapacity];size_t size;
  if(!ArRegionalSession_Encode(&next,bytes,sizeof(bytes),&size))return false;
  *out=next;return true;
}
bool SaveSlotManager_Edit(ArRegionalSession *draft,const OverlayRegionRow *row,int choice) {
  if(!draft || !row)return false;
  ArRegionalSession candidate=*draft;ActRaiserRegionalRulesView view;
  if(!SaveSlotManager_View(&candidate,true,&view))return false;
  ActRaiserRegionalPopulationIntent intent={0};
  ActRaiserRegionalEditContext context={&candidate,&intent,true,true};
  ActRaiserRegionalEditResult result=row->kind==kOverlayRegionRow_Preset
    ?ActRaiserRegionalEditor_RequestProfile(&context,&view,row->group,(ArRegionalSource)choice)
    :row->kind==kOverlayRegionRow_Difficulty
      ?ActRaiserRegionalEditor_RequestDifficultyChoice(&context,&view,(ArRegionalDifficultyChoice)choice)
      :ActRaiserRegionalEditor_RequestRules(&context,&view,row->setting,(ArRegionalSource)choice);
  if(result!=kActRaiserRegionalEdit_Applied && result!=kActRaiserRegionalEdit_Unchanged)return false;
  candidate.effective=candidate.requested;*draft=candidate;return true;
}
bool SaveSlotManager_ReadDraft(const SaveSlots *s,unsigned slot,ArRegionalSession *out,SaveError *e) {
  if(!out)return false;
  uint8_t bytes[kSaveSlotDraftCapacity];size_t size=0;ArRegionalSession draft,validated;
  if(!SaveSlots_ReadDraft(s,slot,bytes,sizeof(bytes),&size,e))return false;
  if(ArRegionalSession_Decode(bytes,size,&draft)!=kSaveCheckpoint_Ready || draft.slot!=slot ||
      !SaveSlotManager_Draft(&validated,slot,draft.campaign,&draft.requested,&draft.randomizer)) {
    if(e)snprintf(e->message,sizeof(e->message),"The prepared new game is damaged or unsupported.");
    return false;
  }
  *out=validated;return true;
}
