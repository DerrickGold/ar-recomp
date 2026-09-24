/* Read-only formatting of copied campaign details. */
#include <time.h>

static void SlotFormatTime(uint64_t timestamp,char *out,size_t cap) {
  time_t value=(time_t)timestamp;struct tm *local=localtime(&value);
  if(!timestamp || !local || !strftime(out,cap,"%Y-%m-%d %H:%M",local))snprintf(out,cap,"%s",Ui("slots.unknown"));
}
static void SlotRelativeTime(uint64_t timestamp,char *out,size_t cap) {
  time_t now=time(NULL);uint64_t elapsed=now>0 && (uint64_t)now>timestamp?(uint64_t)now-timestamp:0;
  char count[32];const char *key;
  if(!timestamp){snprintf(out,cap,"%s",Ui("slots.unknown"));return;}
  if(now>0 && timestamp>(uint64_t)now){SlotFormatTime(timestamp,out,cap);return;}
  if(elapsed<60){snprintf(out,cap,"%s",Ui("slots.just_saved"));return;}
  uint64_t n=elapsed/60;key="slots.minutes";
  if(n>=60){n/=60;key="slots.hours";if(n>=24){n/=24;key="slots.days";}}
  snprintf(count,sizeof(count),"%llu",(unsigned long long)n);ArUiTextArgument arg={"n",count};
  ArUiCatalog_Format(out,cap,Ui(key),&arg,1);
}
typedef struct SlotDetailLine {char label[160],value[256];} SlotDetailLine;
static void SlotRegionValue(const ActRaiserRegionalRulesView *view,const OverlayRegionRow *row,
    bool effective,char *out,size_t capacity) {
  SettingsOverlayRegionBadge badge;
  if(!OverlayRegionMenu_Value(InterfaceLocale(),view,row,effective,out,capacity,&badge))
    snprintf(out,capacity,"%s",Ui("slots.unknown"));
}
static int SlotDetails(SlotDetailLine lines[kSlotDetailCapacity],bool expanded) {
  const SaveSlotDetails *slot=&s_slots.collection.slots[s_slots.selected];int n=0;
#define SLOT_LINE(label_,fmt_,...) do {snprintf(lines[n].label,sizeof(lines[n].label),"%s",label_);snprintf(lines[n++].value,sizeof(lines[0].value),fmt_,__VA_ARGS__);} while(0)
  if(slot->state!=kSaveSlot_Ready && !slot->prepared)return 0;
  if(s_slots.tab==kSlotPage_Summary) {
    if(slot->state==kSaveSlot_Ready) {
    char time_text[64];SlotFormatTime(slot->saved_at,time_text,sizeof(time_text));
    SLOT_LINE(Ui(slot->approximate_time?"slots.file_time":"slots.last_saved"),"%s",time_text);
    if(slot->summary.level<0)SLOT_LINE(Ui("slots.level"),"%s",Ui("slots.unknown"));
    else SLOT_LINE(Ui("slots.level"),"%d",slot->summary.level);
    if(slot->summary.acts_cleared<0)SLOT_LINE(Ui("slots.acts"),"%s",Ui("slots.unknown"));
    else SLOT_LINE(Ui("slots.acts"),"%d / 12",slot->summary.acts_cleared);
    const char *heim=slot->summary.death_heim==4?"slots.cleared":slot->summary.death_heim==1?"slots.unlocked":slot->summary.death_heim==0?"slots.locked":"slots.unknown";
    SLOT_LINE("Death Heim","%s",Ui(heim));
    } else SLOT_LINE(Ui("slots.prepared"),"%s",Ui("slots.not_saved"));
    const char *sources[]={"US","JP","EU",Ui("overlay.region.custom")};
    SLOT_LINE(Ui("slots.gameplay"),"%s",sources[SlotProfileSource(&slot->regions,kArRegionalProfile_Gameplay)]);
    SLOT_LINE(Ui("slots.presentation"),"%s",sources[SlotProfileSource(&slot->regions,kArRegionalProfile_Presentation)]);
    SLOT_LINE(Ui("slots.difficulty"),"%s",OverlayRegionMenu_DifficultyLabel(InterfaceLocale(),ActRaiserRegionalSettings_DifficultyChoice(&slot->regions,false)));
    if(SlotRandomizerVisible()) {
      if(slot->legacy)SLOT_LINE(Ui("slots.randomizer"),"%s",Ui("slots.legacy"));
      else if(slot->randomizer.enabled)SLOT_LINE(Ui("slots.seed"),"%u",slot->randomizer.seed);
      else SLOT_LINE(Ui("slots.randomizer"),"%s",Ui("slots.off"));
    }
    const char *towns[]={"Fillmore","Bloodpool","Kasandora","Aitos","Marahna","Northwall"};
    for(int t=0;expanded && slot->state==kSaveSlot_Ready && t<6;++t) {
      int state=slot->summary.towns[t];
      const char *key=state==0?"slots.act1":state==2?"slots.act1_done":state==3?"slots.act2":state==4?"slots.cleared":"slots.unknown";
      SLOT_LINE(towns[t],"%s",Ui(key));
    }
  } else if(s_slots.tab==kSlotPage_Regions) {
    const SaveSlotDetails *active=&s_slots.collection.slots[s_slots.collection.active];
    bool comparison=active->state==kSaveSlot_Ready && s_slots.selected!=(int)s_slots.collection.active;
    for(int page=1;page<kOverlayRegionPage_Count && n<kSlotDetailCapacity-1;++page)for(unsigned i=0;i<OverlayRegionMenu_Count(page) && n<kSlotDetailCapacity-1;++i) {
      const OverlayRegionRow *row=OverlayRegionMenu_Row(page,i);
      if(row->kind==kOverlayRegionRow_Preset)continue;
      char before[100],after[100];
      SlotRegionValue(&slot->regions,row,false,after,sizeof(after));
      bool pending=row->kind==kOverlayRegionRow_Difficulty?
        ActRaiserRegionalSettings_DifficultyChoice(&slot->regions,false)!=ActRaiserRegionalSettings_DifficultyChoice(&slot->regions,true):
        OverlayRegionMenu_RowSource(&slot->regions,row,false)!=OverlayRegionMenu_RowSource(&slot->regions,row,true);
      if(comparison) {
        SlotRegionValue(&active->regions,row,false,before,sizeof(before));
        if(!strcmp(before,after) && !pending)continue;
        SLOT_LINE(OverlayRegionMenu_Label(InterfaceLocale(),row),"%s > %s",before,after);
      } else SLOT_LINE(OverlayRegionMenu_Label(InterfaceLocale(),row),"%s",after);
      if(pending && n<kSlotDetailCapacity-1) {
        SlotRegionValue(&slot->regions,row,true,before,sizeof(before));
        SLOT_LINE(Ui("slots.pending"),"%s > %s",before,after);
      }
    }
    if(!n)SLOT_LINE(Ui("slots.regions"),"%s",Ui("slots.same"));
  } else if(SlotRandomizerVisible()) {
    if(slot->legacy){SLOT_LINE(Ui("slots.randomizer"),"%s",Ui("slots.legacy"));return n;}
    SLOT_LINE(Ui("slots.randomizer"),"%s",Ui(slot->randomizer.enabled?"slots.on":"slots.off"));
    SLOT_LINE(Ui("slots.seed"),"%u",slot->randomizer.seed);
    SLOT_LINE(Ui("slots.generator"),"%u",slot->randomizer.generator);
    for(unsigned i=0;i<sizeof(kSlotRandomRows)/sizeof(kSlotRandomRows[0]);++i) {
      const SlotRandomRow *r=&kSlotRandomRows[i];const SettingDesc *desc=Settings_Find(r->key);if(!desc)continue;
      char text[128];SlotRandomText(&slot->randomizer,r,text,sizeof(text));
      SLOT_LINE(SettingsOverlay_LocalizedLabel(InterfaceLocale(),desc),"%s",text);
    }
  }
#undef SLOT_LINE
  return n;
}
static void SlotOpenDetails(void) {
  s_details.body[0]=0;
  if(s_slots.screen==kSlotScreen_Regions && s_slot_hooks.view) {
    if(!s_slot_hooks.view(&s_slots.draft,&s_slots.draft_view))return;
    const OverlayRegionRow *row=OverlayRegionMenu_Row(s_slots.tab,s_slots.row);
    if(!OverlayRegionMenu_Description(InterfaceLocale(),&s_slots.draft_view,row,s_details.body,sizeof(s_details.body)))return;
    snprintf(s_details.title,sizeof(s_details.title),"%s",OverlayRegionMenu_Label(InterfaceLocale(),row));
  } else if(s_slots.screen==kSlotScreen_Randomizer) {
    const SlotRandomRow *row=SlotRandomAt(s_slots.row);
    const SettingDesc *desc=row?Settings_Find(row->key):NULL;
    if(!desc)return;
    snprintf(s_details.title,sizeof(s_details.title),"%s",SettingsOverlay_LocalizedLabel(InterfaceLocale(),desc));
    snprintf(s_details.body,sizeof(s_details.body),"%s",SettingsOverlay_LocalizedHelp(InterfaceLocale(),desc));
  } else if(s_slots.screen==kSlotScreen_List && !s_slots.advanced_focus) {
    SlotDetailLine lines[kSlotDetailCapacity];int count=SlotDetails(lines,true);size_t used=0;
    snprintf(s_details.title,sizeof(s_details.title),"%s",Ui("slots.manager"));
    for(int i=0;i<count;++i) {
      int n=snprintf(s_details.body+used,sizeof(s_details.body)-used,"%s: %s\n",lines[i].label,lines[i].value);
      if(n<0 || (size_t)n>=sizeof(s_details.body)-used)break;
      used+=(size_t)n;
    }
  }
  if(!s_details.body[0])return;
  s_details.open=true;s_details.top_line=s_details.total_lines=0;s_details.visible_lines=1;
}
