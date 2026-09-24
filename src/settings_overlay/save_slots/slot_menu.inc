/* Private save-slot controller. The host owns storage; this menu owns copies
 * of reviewed data and a scratch new-game draft. No live settings are edited. */
typedef enum SlotScreen {
  kSlotScreen_Closed, kSlotScreen_List, kSlotScreen_Setup,
  kSlotScreen_Seed, kSlotScreen_Randomizer, kSlotScreen_Regions
} SlotScreen;
typedef enum SlotPage {
  kSlotPage_Summary, kSlotPage_Regions, kSlotPage_Randomizer, kSlotPage_Count
} SlotPage;
enum { kSlotRandomPageCount=4, kSlotDetailCapacity=96, kSlotSeedDigits=9 };
static SettingsOverlaySaveSlotHooks s_slot_hooks;
static void SlotOpenDetails(void);
static struct {
  SlotScreen screen;
  SaveSlotCollection collection;
  ArRegionalSession draft;
  ActRaiserRegionalRulesView draft_view;
  int selected, top, row, tab, scroll, digit, setup_row, list_tab;
  int draft_slot;
  int parent_section, parent_row, parent_top, parent_tab;
  bool detail_focus, advanced_focus, advanced, return_to_submenu;
  bool randomized_entry, randomizer_visible, full;
  OverlayDecision decision;
  const SettingDesc *advanced_action;
  unsigned reviewed_active;
  uint64_t reviewed_fingerprint;
  char error[256];
} s_slots;

void SettingsOverlay_SetSaveSlotHooks(const SettingsOverlaySaveSlotHooks *hooks) {
  s_slot_hooks=hooks?*hooks:(SettingsOverlaySaveSlotHooks){0};
}
static bool SlotMenuAvailable(void) { return s_slot_hooks.scan!=NULL; }
static bool SlotMenuActive(void) { return s_slots.screen!=kSlotScreen_Closed; }
static bool SlotRandomizerVisible(void) { return g_settings.show_debug_settings; }
static int SlotPageCount(void) { return SlotRandomizerVisible()?kSlotPage_Count:kSlotPage_Randomizer; }
static void SlotTabState(int *active_tab,int *tab_count) {
  int count=s_slots.screen==kSlotScreen_List?SlotPageCount():
      s_slots.screen==kSlotScreen_Regions?kOverlayRegionPage_Count:
      s_slots.screen==kSlotScreen_Randomizer?kSlotRandomPageCount:1;
  if(active_tab)*active_tab=count>1?s_slots.tab:0;
  if(tab_count)*tab_count=count;
}
static void SlotMenuRefresh(void) {
  if(!SlotMenuActive() || s_slots.randomizer_visible==SlotRandomizerVisible())return;
  s_slots.randomizer_visible=SlotRandomizerVisible();
  /* A live gate change must also dismiss already-open seed/help dialogs.
   * Persisted campaigns keep their recipe; only a fresh scratch draft defaults
   * to standard play when its experimental controls become unavailable. */
  s_details.open=false;
  s_slots.decision=(OverlayDecision){0};
  if(!SlotRandomizerVisible()) {
    s_slots.randomized_entry=false;
    if(s_slots.draft_slot>=0 && !s_slots.collection.slots[s_slots.draft_slot].prepared)
      s_slots.draft.randomizer.enabled=false;
    if(s_slots.list_tab>=SlotPageCount())s_slots.list_tab=kSlotPage_Summary;
    if(s_slots.screen==kSlotScreen_List && s_slots.tab>=SlotPageCount()) {
      s_slots.tab=kSlotPage_Summary;s_slots.scroll=0;
    }
    if(s_slots.screen==kSlotScreen_Seed || s_slots.screen==kSlotScreen_Randomizer)
      s_slots.screen=kSlotScreen_Setup;
  }
  s_slots.setup_row=0;
  if(s_slots.screen==kSlotScreen_Setup)s_slots.row=s_slots.tab=s_slots.scroll=0;
}
static bool SlotDecisionActive(void) {return s_slots.decision.result==kOverlayDecision_Pending;}
static const char *SlotAdvancedTitle(void) {
  static char title[160];
  if(!s_slots.advanced)return NULL;
  char number[16];snprintf(number,sizeof(number),"%u",s_slots.collection.active+1);
  const SaveSlotDetails *slot=&s_slots.collection.slots[s_slots.collection.active];
  ArUiTextArgument args[]={{"slot",number},{"name",slot->summary.name[0]?slot->summary.name:Ui("slots.new_game")}};
  ArUiCatalog_Format(title,sizeof(title),Ui("slots.advanced.target"),args,2);
  return title;
}
static bool SlotConfirmAdvancedAction(const SettingDesc *desc) {
  if(!s_slots.advanced || (desc->action!=kSettingAction_SaveImport &&
      desc->action!=kSettingAction_SaveApplyPersist && desc->action!=kSettingAction_SaveApplySession))return false;
  s_slots.advanced_action=desc;
  s_slots.reviewed_active=s_slots.collection.active;
  s_slots.reviewed_fingerprint=s_slots.collection.slots[s_slots.reviewed_active].fingerprint;
  char number[16];snprintf(number,sizeof(number),"%u",s_slots.reviewed_active+1);
  const SaveSlotDetails *slot=&s_slots.collection.slots[s_slots.reviewed_active];
  ArUiTextArgument args[]={{"slot",number},{"name",slot->summary.name[0]?slot->summary.name:Ui("slots.new_game")},
      {"action",SettingsOverlay_LocalizedLabel(InterfaceLocale(),desc)}};
  s_slots.decision=(OverlayDecision){.result=kOverlayDecision_Pending,.body_text=true};
  ArUiCatalog_Format(s_slots.decision.title,sizeof(s_slots.decision.title),Ui("slots.advanced.confirm"),args,3);
  snprintf(s_slots.decision.body,sizeof(s_slots.decision.body),"%s",Ui(desc->action==kSettingAction_SaveImport?
      "slots.advanced.import":"slots.advanced.edit"));
  snprintf(s_slots.decision.accept,sizeof(s_slots.decision.accept),"%s",desc->action==kSettingAction_SaveImport?
      "slots.advanced.replace":"slots.advanced.apply");
  return true;
}
static void SlotMenuClose(void) {
  s_slots.screen=kSlotScreen_Closed;
  s_slots.advanced=false;
  s_slots.advanced_action=NULL;
  s_slots.decision=(OverlayDecision){0};
  s_status[0]=0;
}
static bool SlotScanCollection(void) {
  SaveSlotCollection collection;
  if(!s_slot_hooks.scan || !s_slot_hooks.scan(&collection) || collection.active>=kSaveSlotCount)
    return false;
  s_slots.collection=collection;
  s_slots.full=true;
  for(int i=0;i<kSaveSlotCount;++i)
    if(collection.slots[i].state==kSaveSlot_Empty && !collection.slots[i].prepared)
      s_slots.full=false;
  return true;
}
bool SettingsOverlay_OpenSaveSlots(bool randomized) {
  if(!s_open || !SlotMenuAvailable() || (randomized && !SlotRandomizerVisible()))return false;
  if(s_slots.advanced && !randomized)return SlotReturnFromAdvanced();
  memset(&s_slots,0,sizeof(s_slots));
  s_slots.parent_section=s_section;
  s_slots.parent_row=s_row;
  s_slots.parent_top=s_top_row;
  s_slots.parent_tab=s_tab[s_section];
  s_slots.draft_slot=-1;
  s_slots.return_to_submenu=s_submenu_open;
  if(!SlotScanCollection())return false;
  s_slots.screen=kSlotScreen_List;
  s_slots.selected=s_slots.collection.active;
  s_slots.randomized_entry=randomized;
  s_slots.randomizer_visible=SlotRandomizerVisible();
  s_status[0]=0;
  if(randomized) {
    for(int i=0;i<kSaveSlotCount;++i) {
      if(s_slots.collection.slots[i].state==kSaveSlot_Empty && !s_slots.collection.slots[i].prepared) {
        s_slots.selected=i;
        break;
      }
    }
  }
  return true;
}
static void SlotOpenAdvanced(void) {
  s_slots.screen=kSlotScreen_Closed;
  s_slots.advanced=true;
  for(int section=0;section<kSectionCount;++section)
    if(kSections[section].icon==kOverlayIcon_Save)s_section=section;
  s_submenu_open=true;
  s_row=s_top_row=0;
  s_tab[s_section]=kSaveEditorPage_Actions;
  s_status[0]=0;
  SettingsOverlay_Refresh();
}
static bool SlotReturnFromAdvanced(void) {
  if(!s_slots.advanced)return false;
  s_slots.advanced=false;
  s_slots.advanced_action=NULL;
  s_slots.screen=kSlotScreen_List;
  s_status[0]=0;
  /* Import/editor actions may have changed the save since entering Advanced. */
  if(!SlotScanCollection())snprintf(s_slots.error,sizeof(s_slots.error),"%s",Ui("slots.read_failed"));
  return true;
}

/* Typed access deliberately avoids offsets and field-size casts: the recipe
 * has bool, byte and uint16 fields, while settings descriptors use int. */
typedef enum SlotRandomOption {
  kSlotRandom_Action, kSlotRandom_Towns, kSlotRandom_HP, kSlotRandom_Attack,
  kSlotRandom_Enemies, kSlotRandom_Scope, kSlotRandom_Drops, kSlotRandom_Statues,
  kSlotRandom_Lairs, kSlotRandom_LairTypes
} SlotRandomOption;
typedef struct SlotRandomRow { const char *key; SlotRandomOption option; int tab; } SlotRandomRow;
static const SlotRandomRow kSlotRandomRows[]={
  {"rando_regional_action",kSlotRandom_Action,0}, {"rando_regional_towns",kSlotRandom_Towns,0},
  {"rando_enemy_hp",kSlotRandom_HP,1}, {"rando_enemy_atk",kSlotRandom_Attack,1},
  {"rando_enemy_types",kSlotRandom_Enemies,1}, {"rando_enemy_scope",kSlotRandom_Scope,1},
  {"rando_statue_drops",kSlotRandom_Drops,2}, {"rando_statue_spots",kSlotRandom_Statues,2},
  {"rando_lair_spots",kSlotRandom_Lairs,3}, {"rando_lair_types",kSlotRandom_LairTypes,3},
};
static const SlotRandomRow *SlotRandomAt(int index) {
  for(unsigned i=0;i<sizeof(kSlotRandomRows)/sizeof(kSlotRandomRows[0]);++i)
    if(kSlotRandomRows[i].tab==s_slots.tab && index--==0)return &kSlotRandomRows[i];
  return NULL;
}
static int SlotRecipeValue(const RandomizerConfig *recipe,SlotRandomOption option) {
  switch(option) {
    case kSlotRandom_Action:return recipe->regional_action;
    case kSlotRandom_Towns:return recipe->regional_towns;
    case kSlotRandom_HP:return recipe->hp_percent;
    case kSlotRandom_Attack:return recipe->attack_percent;
    case kSlotRandom_Enemies:return recipe->enemy_types;
    case kSlotRandom_Scope:return recipe->enemy_scope;
    case kSlotRandom_Drops:return recipe->statue_drops;
    case kSlotRandom_Statues:return recipe->statue_spots;
    case kSlotRandom_Lairs:return recipe->lair_spots;
    case kSlotRandom_LairTypes:return recipe->lair_types;
  }
  return 0;
}
static void SlotRecipeSet(RandomizerConfig *recipe,SlotRandomOption option,int value) {
  switch(option) {
    case kSlotRandom_Action:recipe->regional_action=value;break;
    case kSlotRandom_Towns:recipe->regional_towns=value;break;
    case kSlotRandom_HP:recipe->hp_percent=value;break;
    case kSlotRandom_Attack:recipe->attack_percent=value;break;
    case kSlotRandom_Enemies:recipe->enemy_types=value;break;
    case kSlotRandom_Scope:recipe->enemy_scope=value;break;
    case kSlotRandom_Drops:recipe->statue_drops=value;break;
    case kSlotRandom_Statues:recipe->statue_spots=value;break;
    case kSlotRandom_Lairs:recipe->lair_spots=value;break;
    case kSlotRandom_LairTypes:recipe->lair_types=value;break;
  }
}
static void SlotRandomText(const RandomizerConfig *recipe,const SlotRandomRow *row,char *out,size_t capacity) {
  const SettingDesc *desc=row?Settings_Find(row->key):NULL;
  if(!desc){snprintf(out,capacity,"%s",Ui("slots.unknown"));return;}
  int value=SlotRecipeValue(recipe,row->option);
  if(row->option==kSlotRandom_HP || row->option==kSlotRandom_Attack) {
    snprintf(out,capacity,"%d%%",value);
    return;
  }
  bool boolean=value!=0;
  SettingDesc copy=*desc;
  copy.field=desc->type==kSettingType_Bool?(void *)&boolean:(void *)&value;
  SettingsOverlay_LocalizedValue(InterfaceLocale(),&copy,out,capacity);
}
static void SlotRandomChange(const SlotRandomRow *row,int direction) {
  const SettingDesc *desc=row?Settings_Find(row->key):NULL;
  if(!desc)return;
  int value=SlotRecipeValue(&s_slots.draft.randomizer,row->option)+direction*(int)desc->step;
  if(value<desc->minval)value=(int)desc->maxval;
  if(value>desc->maxval)value=(int)desc->minval;
  RandomizerConfig candidate=s_slots.draft.randomizer;
  SlotRecipeSet(&candidate,row->option,value);
  if(RandomizerConfig_Valid(&candidate))s_slots.draft.randomizer=candidate;
}
static int SlotProfileSource(const ActRaiserRegionalRulesView *view,ArRegionalProfileGroup group) {
  int source=view->profiles[group].source;
  return source>=0 && source<=kArRegionalSource_Count?source:kArRegionalSource_Count;
}
static int SlotSetupCode(int row) {
  if(!SlotRandomizerVisible())return row>=0 && row<2?row+4:-1;
  if(s_slots.draft.randomizer.enabled)return row;
  const int standard[]={0,4,5};
  return row>=0 && row<3?standard[row]:-1;
}
static int SlotRowCount(void) {
  if(s_slots.screen==kSlotScreen_Setup)return !SlotRandomizerVisible()?2:s_slots.draft.randomizer.enabled?6:3;
  if(s_slots.screen==kSlotScreen_Regions)return (int)OverlayRegionMenu_Count((OverlayRegionPage)s_slots.tab);
  if(s_slots.screen==kSlotScreen_Randomizer){int n=0;while(SlotRandomAt(n))++n;return n;}
  return 1;
}
static const char *SlotMenuKey(void) {
  if(s_slots.decision.result==kOverlayDecision_Pending)return "slot_confirm";
  if(s_slots.screen==kSlotScreen_Seed)return "slot_seed";
  if(s_slots.screen==kSlotScreen_List) {
    if(s_slots.advanced_focus)return "slot_advanced";
    return s_slots.detail_focus?"slot_use":"slot_list";
  }
  if(s_slots.screen==kSlotScreen_Randomizer){const SlotRandomRow *r=SlotRandomAt(s_slots.row);return r?r->key:"";}
  if(s_slots.screen==kSlotScreen_Regions){const OverlayRegionRow *r=OverlayRegionMenu_Row(s_slots.tab,s_slots.row);return r?r->key:"";}
  const char *keys[]={"slot_type","slot_seed","slot_new_seed","slot_options","slot_regions","slot_start"};
  int n=SlotSetupCode(s_slots.row);
  return n>=0 && n<6?keys[n]:"";
}
static void SlotDraftOpen(void) {
  SaveError error={{0}};
  if(!s_slots.collection.writable || !s_slot_hooks.draft ||
      (s_slots.draft_slot!=s_slots.selected && !s_slot_hooks.draft(s_slots.selected,&s_slots.draft,&error))) {
    snprintf(s_slots.error,sizeof(s_slots.error),"%s",error.message[0]?error.message:Ui("slots.draft_failed"));
    return;
  }
  if(s_slots.draft_slot!=s_slots.selected) {
    if(!s_slots.collection.slots[s_slots.selected].prepared) {
      if(!SlotRandomizerVisible())s_slots.draft.randomizer.enabled=false;
      else if(s_slots.randomized_entry)s_slots.draft.randomizer.enabled=true;
    }
    s_slots.draft_slot=s_slots.selected;
  }
  s_slots.list_tab=s_slots.tab;
  s_slots.screen=kSlotScreen_Setup;
  s_slots.row=s_slots.tab=s_slots.scroll=0;
  s_slots.error[0]=0;
}
static void SlotConfirm(void) {
  s_slots.advanced_action=NULL;
  if(!s_slots.collection.writable)return;
  const SaveSlotDetails *slot=&s_slots.collection.slots[s_slots.selected];
  bool create=s_slots.screen==kSlotScreen_Setup;
  if(!create && (slot->state!=kSaveSlot_Ready || s_slots.selected==(int)s_slots.collection.active))return;
  s_slots.decision=(OverlayDecision){.result=kOverlayDecision_Pending,.body_text=true};
  snprintf(s_slots.decision.accept,sizeof(s_slots.decision.accept),"%s",create?"slots.start_restart":"slots.switch_restart");
  char number[16],seed[16];
  snprintf(number,sizeof(number),"%u",s_slots.selected+1);
  snprintf(seed,sizeof(seed),"%u",s_slots.draft.randomizer.seed);
  ArUiTextArgument args[]={{"slot",number},{"seed",seed},{"name",slot->summary.name[0]?slot->summary.name:Ui("slots.unknown")}};
  ArUiCatalog_Format(s_slots.decision.title,sizeof(s_slots.decision.title),
      Ui(create?"slots.confirm.create_title":"slots.confirm.switch_title"),args,3);
  const char *key=create?(SlotRandomizerVisible() && s_slots.draft.randomizer.enabled?"slots.confirm.random":"slots.confirm.new"):"slots.confirm.load";
  ArUiCatalog_Format(s_slots.decision.body,sizeof(s_slots.decision.body),Ui(key),args,3);
}
static void SlotReturnToSetup(void) {
  s_slots.screen=kSlotScreen_Setup;
  s_slots.row=s_slots.setup_row;
  s_slots.tab=s_slots.scroll=0;
}
static void SlotEnterSetupPage(SlotScreen screen) {
  s_slots.setup_row=s_slots.row;
  s_slots.screen=screen;
  s_slots.row=s_slots.tab=s_slots.scroll=s_slots.digit=0;
}
static bool SlotMenuNav(MenuNav nav,bool repeat) {
  if(!SlotMenuActive() && !SlotDecisionActive())return false;
  if(s_slots.decision.result==kOverlayDecision_Pending) {
    if(repeat)return true;
    if(nav==kMenuNav_Up || nav==kMenuNav_Down || nav==kMenuNav_Left || nav==kMenuNav_Right)
      s_slots.decision.accept_selected=!s_slots.decision.accept_selected;
    else if(nav==kMenuNav_Back || nav==kMenuNav_Close){s_slots.decision.result=kOverlayDecision_None;s_slots.advanced_action=NULL;}
    else if(nav==kMenuNav_Confirm) {
      bool accept=s_slots.decision.accept_selected;
      if(!accept)s_slots.advanced_action=NULL;
      s_slots.decision.result=kOverlayDecision_None;
      if(accept && s_slots.advanced_action) {
        const SettingDesc *action=s_slots.advanced_action;s_slots.advanced_action=NULL;
        if(!SlotScanCollection() || !s_slots.collection.writable ||
            s_slots.collection.active!=s_slots.reviewed_active ||
            s_slots.collection.slots[s_slots.reviewed_active].state==kSaveSlot_Unavailable ||
            s_slots.collection.slots[s_slots.reviewed_active].fingerprint!=s_slots.reviewed_fingerprint) {
          SetStatus(Ui("slots.advanced.changed"));return true;
        }
        bool success=Settings_InvokeAction(action);
        if(s_open) {
          (void)SlotScanCollection();
          SetStatus(Ui(success?"overlay.status.action_complete":"overlay.status.action_failed"));
        }
      } else if(accept) {
        SaveError error={{0}};
        if(!s_slot_hooks.start || !s_slot_hooks.start(s_slots.selected,
            s_slots.collection.slots[s_slots.selected].fingerprint,
            s_slots.screen==kSlotScreen_Setup?&s_slots.draft:NULL,&error)) {
          snprintf(s_slots.error,sizeof(s_slots.error),"%s",error.message[0]?error.message:Ui("slots.start_failed"));
          /* A failed restart may have successfully stored the prepared draft.
           * Re-review its fingerprint so retrying does not use stale metadata. */
          (void)SlotScanCollection();
        }
      }
    }
    return true;
  }
  if(nav==kMenuNav_Close && !repeat){SettingsOverlay_Close();return true;}
  if(nav==kMenuNav_Details && !repeat){SlotOpenDetails();return true;}
  if(nav==kMenuNav_Back && !repeat) {
    s_slots.error[0]=0;
    if(s_slots.screen==kSlotScreen_List) {
      if(s_slots.detail_focus){s_slots.detail_focus=false;s_slots.scroll=0;}
      else {
        s_submenu_open=s_slots.return_to_submenu;
        s_section=s_slots.parent_section;
        s_row=s_slots.parent_row;
        s_top_row=s_slots.parent_top;
        s_tab[s_section]=s_slots.parent_tab;
        SlotMenuClose();
        SettingsOverlay_Refresh();
      }
    } else if(s_slots.screen==kSlotScreen_Setup) {
      s_slots.screen=kSlotScreen_List;
      s_slots.tab=s_slots.list_tab;
      s_slots.scroll=0;
    } else SlotReturnToSetup();
    return true;
  }
  int direction=(nav==kMenuNav_Up || nav==kMenuNav_Left || nav==kMenuNav_TabPrev)?-1:1;
  if(s_slots.screen==kSlotScreen_List) {
    if(nav==kMenuNav_Up || nav==kMenuNav_Down) {
      if(s_slots.detail_focus) {
        if(direction>0 || s_slots.scroll>0)s_slots.scroll+=direction;
      } else {
        int item=s_slots.advanced_focus?kSaveSlotCount:s_slots.selected;
        item=(item+direction+kSaveSlotCount+1)%(kSaveSlotCount+1);
        s_slots.advanced_focus=item==kSaveSlotCount;
        if(!s_slots.advanced_focus)s_slots.selected=item;
        s_slots.scroll=0;
        s_slots.error[0]=0;
      }
    } else if(nav==kMenuNav_Left || nav==kMenuNav_Right || nav==kMenuNav_TabPrev || nav==kMenuNav_TabNext) {
      if(!s_slots.advanced_focus){int count=SlotPageCount();s_slots.tab=(s_slots.tab+direction+count)%count;s_slots.scroll=0;}
    } else if(nav==kMenuNav_Confirm && !repeat) {
      if(s_slots.advanced_focus)SlotOpenAdvanced();
      else if(s_slots.collection.slots[s_slots.selected].state==kSaveSlot_Empty)SlotDraftOpen();
      else if(!s_slots.detail_focus)s_slots.detail_focus=true;
      else SlotConfirm();
    }
    return true;
  }
  if(s_slots.screen==kSlotScreen_Seed) {
    if(nav==kMenuNav_Left || nav==kMenuNav_Right)s_slots.digit=(s_slots.digit+direction+kSlotSeedDigits)%kSlotSeedDigits;
    else if(nav==kMenuNav_Up || nav==kMenuNav_Down) {
      uint32_t place=1;
      for(int i=kSlotSeedDigits-1;i>s_slots.digit;--i)place*=10;
      int digit=(s_slots.draft.randomizer.seed/place)%10;
      int next=(digit-direction+10)%10;
      s_slots.draft.randomizer.seed=(uint32_t)((int64_t)s_slots.draft.randomizer.seed+(next-digit)*(int64_t)place);
    } else if(nav==kMenuNav_Confirm && !repeat)SlotReturnToSetup();
    return true;
  }
  if(nav==kMenuNav_Up || nav==kMenuNav_Down) {
    int count=SlotRowCount();
    if(count>0)s_slots.row=(s_slots.row+direction+count)%count;
  } else if(nav==kMenuNav_TabPrev || nav==kMenuNav_TabNext) {
    if(s_slots.screen==kSlotScreen_Randomizer || s_slots.screen==kSlotScreen_Regions) {
      int count=s_slots.screen==kSlotScreen_Regions?kOverlayRegionPage_Count:kSlotRandomPageCount;
      s_slots.tab=(s_slots.tab+direction+count)%count;
      s_slots.row=s_slots.scroll=0;
    }
  } else if(nav==kMenuNav_Left || nav==kMenuNav_Right || (nav==kMenuNav_Confirm && !repeat)) {
    if(s_slots.screen==kSlotScreen_Setup) {
      int row=SlotSetupCode(s_slots.row);
      if(row==0 && SlotRandomizerVisible())s_slots.draft.randomizer.enabled=!s_slots.draft.randomizer.enabled;
      else if(nav==kMenuNav_Confirm) {
        if(row==1)SlotEnterSetupPage(kSlotScreen_Seed);
        else if(row==2)s_slots.draft.randomizer.seed=Randomizer_NewSeed();
        else if(row==3)SlotEnterSetupPage(kSlotScreen_Randomizer);
        else if(row==4)SlotEnterSetupPage(kSlotScreen_Regions);
        else if(row==5)SlotConfirm();
      }
    } else if(s_slots.screen==kSlotScreen_Randomizer)SlotRandomChange(SlotRandomAt(s_slots.row),direction);
    else if(s_slots.screen==kSlotScreen_Regions && s_slot_hooks.view && s_slot_hooks.edit) {
      const OverlayRegionRow *row=OverlayRegionMenu_Row(s_slots.tab,s_slots.row);
      if(!row || !s_slot_hooks.view(&s_slots.draft,&s_slots.draft_view))return true;
      int current=row->kind==kOverlayRegionRow_Difficulty?ActRaiserRegionalSettings_DifficultyChoice(&s_slots.draft_view,false):
        OverlayRegionMenu_RowSource(&s_slots.draft_view,row,false);
      int count=row->kind==kOverlayRegionRow_Difficulty?kArRegionalDifficultyChoice_Count:row->binary?2:3;
      if(!s_slot_hooks.edit(&s_slots.draft,row,(current+direction+count)%count))
        snprintf(s_slots.error,sizeof(s_slots.error),"%s",Ui("slots.incompatible"));
      else s_slots.error[0]=0;
    }
  }
  return true;
}
