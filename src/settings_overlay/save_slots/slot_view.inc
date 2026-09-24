/* Native overlay presentation: game-font rows, shared panel art and input
 * hints. Smaller text is reserved for timestamps, tabs and explanatory copy. */
#include "slot_menu.inc"
#include "slot_details.inc"

static void SlotTextRow(const MenuLayout *layout,int x,int y,int width,
    const char *label,const char *value,bool selected,bool metadata) {
  if(selected) {
    FillLogicalRect(layout,x-6,y-3,width+12,15,kHighlight);
    FillLogicalRect(layout,x-6,y-3,2,15,kSelectYellow);
  }
  int cell=metadata?kDebugGlyphWidth:kGlyphSize;
  int value_chars=CappedTextLength(value,width/(cell*2));
  int label_chars=(width-value_chars*cell-(value_chars?12:0))/cell;
  if(metadata) {
    DrawSmallTextN(layout,x,y,label,label_chars,kMutedText);
    DrawSmallTextN(layout,x+width-value_chars*cell,y,value,value_chars,kSteelBlue);
  } else {
    DrawTextN(layout,x,y,label,label_chars,selected?kText_Value:kText_Normal);
    DrawTextN(layout,x+width-value_chars*cell,y,value,value_chars,kText_Value);
  }
}
static void SlotTabs(const MenuLayout *layout,int x,int y,int width,const char *const *tabs,int count) {
  int total=0;
  for(int i=0;i<count;++i)total+=SmallTextWidth(Ui(tabs[i]))+14;
  if(total<=width) {
    for(int i=0;i<count;++i) {
      int span=SmallTextWidth(Ui(tabs[i]))+12;
      if(i==s_slots.tab) {
        FillLogicalRect(layout,x,y-3,span,14,kHighlight);
        FillLogicalRect(layout,x,y+10,span,1,kSelectYellow);
      }
      DrawSmallText(layout,x+6,y,Ui(tabs[i]),i==s_slots.tab?kSelectYellow:kSteelBlue);
      x+=span+2;
    }
  } else {
    char page[24];snprintf(page,sizeof(page),"< %d/%d >",s_slots.tab+1,count);
    int page_width=SmallTextWidth(page);
    DrawSmallTextN(layout,x,y,Ui(tabs[s_slots.tab]),(width-page_width-12)/kDebugGlyphWidth,kSelectYellow);
    DrawSmallText(layout,x+width-page_width,y,page,kSteelBlue);
  }
}
static int SlotWrappedText(const MenuLayout *layout,int x,int y,const char *text,
    int columns,int limit,bool draw,bool primary) {
  size_t remaining=strlen(text);
  int lines=0;
  while(remaining && lines<limit) {
    ArInterfaceTextLine slice;
    char buffer[kArInterfaceTextMaximumBytes+1];
    if(!ArInterfaceText_WrapLine(text,remaining,columns,sizeof(buffer)-1,&slice) || !slice.consumed)break;
    memcpy(buffer,text,slice.bytes);buffer[slice.bytes]=0;
    if(draw) {
      if(primary)DrawTextN(layout,x,y+lines*12,buffer,columns,kText_Normal);
      else DrawSmallText(layout,x,y+lines*kSmallLineHeight,buffer,kSteelBlue);
    }
    text+=slice.consumed;remaining-=slice.consumed;++lines;
  }
  return lines;
}
static void SlotDrawConfirmation(const MenuLayout *layout) {
  const OverlayDecision *decision=&s_slots.decision;
  int width=layout->logical_width<448?layout->logical_width-32:416;
  int title_lines=SlotWrappedText(layout,0,0,decision->title,(width-32)/kGlyphSize,2,false,true);
  int body_lines=SlotWrappedText(layout,0,0,decision->body,(width-32)/kDebugGlyphWidth,10,false,false);
  int height=title_lines*12+body_lines*kSmallLineHeight+80;
  if(height>layout->logical_height-32)height=layout->logical_height-32;
  int x=(layout->logical_width-width)/2,y=(layout->logical_height-height)/2;
  FillLogicalRect(layout,0,0,layout->logical_width,layout->logical_height,ARGB(180,0,0,0));
  DrawDialogPanel(layout,x,y,width,height);
  SlotWrappedText(layout,x+16,y+16,decision->title,(width-32)/kGlyphSize,2,true,true);
  SlotWrappedText(layout,x+16,y+24+title_lines*12,decision->body,(width-32)/kDebugGlyphWidth,
      (height-title_lines*12-80)/kSmallLineHeight,true,false);
  for(int i=0;i<2;++i) {
    bool selected=decision->accept_selected==(i==0);
    SlotTextRow(layout,x+18,y+height-43+i*20,width-36,
        Ui(i?"overlay.decision.cancel":decision->accept),"",selected,false);
  }
}
static void SlotDrawList(const MenuLayout *layout,int width,int height) {
  int advanced_y=height-12;
  int visible=(advanced_y-46)/32;
  if(visible<1)visible=1;
  if(s_slots.selected<s_slots.top)s_slots.top=s_slots.selected;
  if(s_slots.selected>=s_slots.top+visible)s_slots.top=s_slots.selected-visible+1;
  DrawDialogPanel(layout,8,8,width,height);
  DrawTextN(layout,20,20,Ui("slots.title"),(width-24)/kGlyphSize,kText_Normal);
  for(int i=s_slots.top;i<kSaveSlotCount && i<s_slots.top+visible;++i) {
    const SaveSlotDetails *slot=&s_slots.collection.slots[i];
    int y=42+(i-s_slots.top)*32;
    bool selected=i==s_slots.selected && !s_slots.advanced_focus;
    if(selected) {
      FillLogicalRect(layout,14,y-4,width-22,29,s_slots.detail_focus?kPanel:kHighlight);
      if(!s_slots.detail_focus)FillLogicalRect(layout,14,y-4,2,29,kSelectYellow);
    }
    const char *name=Ui(slot->state==kSaveSlot_Unavailable?"slots.unavailable":slot->prepared?"slots.prepared":"slots.empty");
    if(slot->state==kSaveSlot_Ready)name=slot->summary.name[0]?slot->summary.name:Ui("slots.unknown");
    char label[128],note[64];
    snprintf(label,sizeof(label),"%02u %s",i+1,name);
    DrawTextN(layout,22,y,label,(width-34)/kGlyphSize,selected?kText_Normal:kText_Dim);
    if(slot->state==kSaveSlot_Ready)SlotRelativeTime(slot->saved_at,note,sizeof(note));
    else snprintf(note,sizeof(note),"%s",Ui(slot->state==kSaveSlot_Empty?"slots.not_saved":"slots.recovery"));
    bool active=i==(int)s_slots.collection.active;
    int cells=(width-34)/kDebugGlyphWidth;
    int active_cells=active?CappedTextLength(Ui("slots.active"),8):0;
    DrawSmallTextN(layout,22,y+13,note,cells-(active?active_cells+1:0),kMutedText);
    if(active)DrawSmallTextN(layout,22+(cells-active_cells)*kDebugGlyphWidth,y+13,Ui("slots.active"),active_cells,kSteelBlue);
  }
  DrawScrollBar(layout,8+width-12,38,visible*32,kSaveSlotCount,visible,s_slots.top,kSteelBlue);
  FillLogicalRect(layout,20,advanced_y-9,width-24,1,kSteelDim);
  SlotTextRow(layout,22,advanced_y,width-36,Ui("slots.advanced"),">",s_slots.advanced_focus,false);
}
static void SlotMenuRender(const MenuLayout *layout) {
  if(s_slots.decision.result==kOverlayDecision_Pending){SlotDrawConfirmation(layout);return;}
  int width=layout->logical_width,height=layout->logical_height,top_height=height-80;
  bool list=s_slots.screen==kSlotScreen_List;
  int left=8,panel_width=width-16;
  if(list) {
    int list_width=152;
    left=list_width+16;panel_width=width-left-8;
    SlotDrawList(layout,list_width,top_height);
  }
  DrawDialogPanel(layout,left,8,panel_width,top_height);
  if(list && s_slots.detail_focus)FillLogicalRect(layout,left+6,16,2,top_height-16,kSelectYellow);
  int icon_section=0;
  for(int i=0;i<kSectionCount;++i)if(kSections[i].icon==kOverlayIcon_Save)icon_section=i;
  DrawSectionIcon(layout,left+12,16,16,icon_section,true,255);
  const SaveSlotDetails *selected=&s_slots.collection.slots[s_slots.selected];
  char heading[320];
  snprintf(heading,sizeof(heading),"%02u  %s",s_slots.selected+1,
      list?(selected->state==kSaveSlot_Ready && selected->summary.name[0]?selected->summary.name:
          Ui(selected->prepared?"slots.prepared":"slots.manager")):Ui("slots.new_game"));
  DrawTextN(layout,left+34,20,heading,(panel_width-46)/kGlyphSize,kText_Normal);
  int x=left+12,content_width=panel_width-28;
  if(list) {
    const char *tabs[]={"slots.summary","slots.regions_tab","slots.randomizer"};
    SlotTabs(layout,x,40,content_width,tabs,SlotPageCount());
    int action_y=top_height-14;
    int content_y=65;
    if(s_slots.tab==kSlotPage_Regions && s_slots.selected!=(int)s_slots.collection.active &&
        s_slots.collection.slots[s_slots.collection.active].state==kSaveSlot_Ready) {
      char baseline[96],number[16];snprintf(number,sizeof(number),"%u",s_slots.collection.active+1);
      ArUiTextArgument arg={"slot",number};
      ArUiCatalog_Format(baseline,sizeof(baseline),Ui("slots.compared"),&arg,1);
      DrawSmallTextN(layout,x,57,baseline,content_width/kDebugGlyphWidth,kMutedText);
      content_y=72;
    }
    if(selected->state==kSaveSlot_Unavailable) {
      const char *error=selected->error.message[0]?selected->error.message:Ui("slots.recovery");
      DrawWrappedSmallText(layout,x,content_y,error,content_width/kDebugGlyphWidth,(action_y-content_y-8)/kSmallLineHeight,kSteelBlue);
    } else if(selected->state==kSaveSlot_Empty && !selected->prepared) {
      DrawWrappedSmallText(layout,x,content_y,Ui("slots.empty.help"),content_width/kDebugGlyphWidth,(action_y-content_y-8)/kSmallLineHeight,kSteelBlue);
    } else {
      SlotDetailLine lines[kSlotDetailCapacity];
      int count=SlotDetails(lines,false),visible=(action_y-content_y-8)/16;
      if(visible<1)visible=1;
      if(s_slots.scroll>count-visible)s_slots.scroll=count>visible?count-visible:0;
      for(int i=s_slots.scroll;i<count && i<s_slots.scroll+visible;++i) {
        bool metadata=!strcmp(lines[i].label,Ui("slots.last_saved")) || !strcmp(lines[i].label,Ui("slots.file_time"));
        SlotTextRow(layout,x,content_y+(i-s_slots.scroll)*16,content_width,lines[i].label,lines[i].value,false,metadata);
      }
      DrawScrollBar(layout,left+panel_width-10,content_y,visible*16,count,visible,s_slots.scroll,kSteelBlue);
    }
    const char *action="slots.switch";
    bool enabled=s_slots.collection.writable;
    if(selected->state==kSaveSlot_Unavailable){action="slots.unavailable";enabled=false;}
    else if(selected->state==kSaveSlot_Empty)action=selected->prepared?"slots.review_setup":"slots.new_game";
    else if(s_slots.selected==(int)s_slots.collection.active){action="slots.already_active";enabled=false;}
    FillLogicalRect(layout,x,action_y-8,content_width,1,kSteelDim);
    SlotTextRow(layout,x,action_y,content_width,Ui(action),"",s_slots.detail_focus && enabled,false);
  } else if(s_slots.screen==kSlotScreen_Seed) {
    DrawWrappedSmallText(layout,x,44,Ui("slots.seed.help"),content_width/kDebugGlyphWidth,3,kSteelBlue);
    char seed[16];snprintf(seed,sizeof(seed),"%09u",s_slots.draft.randomizer.seed);
    DrawTextN(layout,x,84,seed,kSlotSeedDigits,kText_Value);
    DrawSmallText(layout,x+s_slots.digit*kGlyphSize,96,"^",kSelectYellow);
  } else {
    const char *random_tabs[]={"overlay.tab.seed","overlay.tab.enemies","overlay.tab.items","overlay.tab.simulation"};
    const char *region_tabs[]={"overlay.region.tabs.presets","overlay.region.tabs.action","overlay.region.tabs.towns","overlay.region.tabs.controls","overlay.region.tabs.presentation"};
    if(s_slots.screen==kSlotScreen_Randomizer)SlotTabs(layout,x,40,content_width,random_tabs,kSlotRandomPageCount);
    else if(s_slots.screen==kSlotScreen_Regions)SlotTabs(layout,x,40,content_width,region_tabs,kOverlayRegionPage_Count);
    else DrawSmallTextN(layout,x,40,Ui("slots.setup"),content_width/kDebugGlyphWidth,kSteelBlue);
    int count=SlotRowCount(),visible=(top_height-56)/20;
    if(visible<1)visible=1;
    if(s_slots.row<s_slots.scroll)s_slots.scroll=s_slots.row;
    if(s_slots.row>=s_slots.scroll+visible)s_slots.scroll=s_slots.row-visible+1;
    bool view_valid=s_slots.screen!=kSlotScreen_Regions ||
        (s_slot_hooks.view && s_slot_hooks.view(&s_slots.draft,&s_slots.draft_view));
    for(int i=s_slots.scroll;i<count && i<s_slots.scroll+visible;++i) {
      const char *label="";char value[128]="";
      if(s_slots.screen==kSlotScreen_Setup) {
        const char *labels[]={"slots.type","slots.seed","slots.new_seed","slots.options","slots.starting_regions","slots.start"};
        int code=SlotSetupCode(i);label=Ui(labels[code]);
        if(code==0)snprintf(value,sizeof(value),"%s",Ui(s_slots.draft.randomizer.enabled?"slots.randomized":"slots.standard"));
        else if(code==1)snprintf(value,sizeof(value),"%u",s_slots.draft.randomizer.seed);
        else if(code==3 || code==4)snprintf(value,sizeof(value),">");
      } else if(s_slots.screen==kSlotScreen_Randomizer) {
        const SlotRandomRow *row=SlotRandomAt(i);
        const SettingDesc *desc=row?Settings_Find(row->key):NULL;
        if(!desc)continue;
        label=SettingsOverlay_LocalizedLabel(InterfaceLocale(),desc);
        SlotRandomText(&s_slots.draft.randomizer,row,value,sizeof(value));
      } else {
        const OverlayRegionRow *row=OverlayRegionMenu_Row(s_slots.tab,i);
        label=OverlayRegionMenu_Label(InterfaceLocale(),row);
        if(view_valid)SlotRegionValue(&s_slots.draft_view,row,false,value,sizeof(value));
        else snprintf(value,sizeof(value),"%s",Ui("slots.unknown"));
      }
      SlotTextRow(layout,x,62+(i-s_slots.scroll)*20,content_width,label,value,i==s_slots.row,false);
    }
    DrawScrollBar(layout,left+panel_width-10,58,visible*20,count,visible,s_slots.scroll,kSteelBlue);
  }
  int bottom=height-64;
  DrawDialogPanel(layout,8,bottom,width-16,56);
  const char *action=list?(s_slots.detail_focus?"slots.use":"slots.select"):"slots.draft.help";
  if(list) {
    if(s_slots.advanced_focus)action="slots.advanced.help";
    else if(s_slots.full)action="slots.full";
    else if(selected->state==kSaveSlot_Unavailable)action="slots.recovery";
    else if(selected->prepared && selected->state==kSaveSlot_Empty)action="slots.prepared.help";
    else if(s_slots.detail_focus && s_slots.selected==(int)s_slots.collection.active)action="slots.active.help";
    else if(selected->approximate_time && s_slots.tab==kSlotPage_Summary)action="slots.file_time.help";
  }
  const char *message=s_slots.error[0]?s_slots.error:s_slots.collection.error.message[0]?s_slots.collection.error.message:Ui(action);
  DrawSmallTextPreview(layout,20,bottom+10,message,(width-40)/kDebugGlyphWidth,2,s_slots.error[0]?kGameGold:kSteelBlue);
  char move[64],tabs[64],confirm[64],back[64],details[64];
  InputClass device=SettingsOverlay_MenuInputDevice();
  OverlayMenuInput_PairHint(move,sizeof(move),kMenuNav_Up,kMenuNav_Down,device);
  OverlayMenuInput_PairHint(tabs,sizeof(tabs),kMenuNav_TabPrev,kMenuNav_TabNext,device);
  OverlayMenuInput_Hint(confirm,sizeof(confirm),kMenuNav_Confirm,device);
  OverlayMenuInput_Hint(back,sizeof(back),kMenuNav_Back,device);
  OverlayMenuInput_Hint(details,sizeof(details),kMenuNav_Details,device);
  MenuHints hints={0};
  AddMenuHint(&hints,move,Ui(s_slots.screen==kSlotScreen_Seed?"overlay.hint.adjust":
      list && !s_slots.detail_focus?"slots.select_hint":"overlay.hint.scroll"));
  if(s_slots.screen==kSlotScreen_Seed) {
    OverlayMenuInput_PairHint(tabs,sizeof(tabs),kMenuNav_Left,kMenuNav_Right,device);
    AddMenuHint(&hints,tabs,Ui("slots.select_hint"));
  }
  if((list && !s_slots.advanced_focus) || s_slots.screen==kSlotScreen_Regions || s_slots.screen==kSlotScreen_Randomizer)
    AddMenuHint(&hints,tabs,Ui("overlay.hint.page"));
  AddMenuHint(&hints,confirm,Ui("slots.choose"));AddMenuHint(&hints,back,Ui("overlay.hint.back"));
  if((list && !s_slots.advanced_focus) || s_slots.screen==kSlotScreen_Regions || s_slots.screen==kSlotScreen_Randomizer)
    AddMenuHint(&hints,details,Ui("overlay.hint.details"));
  DrawMenuHints(layout,20,bottom+37,width-40,&hints,kSteelBlue,kMutedText);
}
