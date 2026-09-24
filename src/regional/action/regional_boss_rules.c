#include "regional/action/regional_boss_rules.h"
#include <stddef.h>
static const ArRegionalBossDescriptor kRules[] = {
  {"minotaur_idle_delay", {47,15,47},1},
  {"minotaur_throw_end_delay", {3,2,3},9},
  {"minotaur_throw_windup_delay", {23,23,15},6},
  {"minotaur_jump_windup_delay", {19,19,15},35},
  {"minotaur_axe_offset", {72,48,72},0},
  {"wizard_post_spread_pause", {31,0,31},0},
  {"ice_dragon_rematch_windup", {118,106,118},0},
  {"tanzra_closing_delay", {31,3,31},33},
  {"tanzra_second_form_clock", {1,0,1},0},
  {"tanzra_upper_turn_delay", {10,11,10},27},
  {"tanzra_minion_turn", {16,16,8},0},
  {"antlion_encounter_x", {2432,2304,2432},0},
  {"antlion_post_volley_strategy", {0,1,0},0},
  {"aitos_dragon_projectile_delay", {0,0,15},1},
  {"aitos_dragon_projectile_flight", {1,1,5},0},
  {"viper_lightning_choice_program", {0,1,2},0},
  {"viper_lightning_delay", {21,21,17},1},
  {"viper_rematch_lightning_delay", {10,10,8},1},
  {"viper_floor_descent", {22,22,15},0},
  {"pharaoh_landing", {40,24,40},0},
  {"pharaoh_rematch_landing", {56,24,56},0},
  {"pharaoh_repeating_heads", {0,1,0},0},
  {"plant_retracting_cycle", {0,1,1},0},
  {"plant_open_sequence", {8,8,16},0},
  {"plant_high_shot_windup", {7,7,23},2},
  {"plant_low_shot_windup", {7,7,23},2},
  {"northwall_throw_sequence", {50,50,15},0},
  {"northwall_impact_sequence", {42,42,20},0},
  {"northwall_projectile_offset", {8,8,16},0},
  {"northwall_impact_offset", {0,0,2},0},
  {"plant_body_geometry", {208,192,208},0},
};
_Static_assert(kArRegionalBoss_Count>0 && kArRegionalBoss_Count<=32,"boss snapshot capacity");
_Static_assert(sizeof(kRules)/sizeof(kRules[0])==kArRegionalBoss_Count,"describe every boss rule");
const ArRegionalBossDescriptor *ArRegionalBoss_Descriptor(unsigned rule) {
  return rule<kArRegionalBoss_Count?&kRules[rule]:NULL;
}
bool ArRegionalBoss_Init(ArRegionalBossPolicy *policy,ArRegionalSource source) {
  if(!policy || (unsigned)source>=kArRegionalSource_Count)return false;
  for(unsigned i=0;i<kArRegionalBoss_Count;++i)policy->source[i]=source;
  return true;
}
bool ArRegionalBoss_Resolve(const ArRegionalBossPolicy *policy,ArRegionalBossSnapshot *snapshot) {
  if(!policy || !snapshot)return false;
  uint64_t result=0;
  for(unsigned i=0;i<kArRegionalBoss_Count;++i) {
    if((unsigned)policy->source[i]>=kArRegionalSource_Count)return false;
    const uint16_t value=kRules[i].value[policy->source[i]];
    unsigned canonical=0;while(kRules[i].value[canonical]!=value)++canonical;
    result|=(uint64_t)canonical<<(2*i);
  }
  *snapshot=result;return true;
}
uint16_t ArRegionalBoss_Value(ArRegionalBossSnapshot snapshot,unsigned rule) {
  const uint64_t known=UINT64_MAX>>(64-2*kArRegionalBoss_Count);
  if(rule>=kArRegionalBoss_Count || (snapshot&~known) ||
      (snapshot&(snapshot>>1)&UINT64_C(0x5555555555555555)))return UINT16_MAX;
  const unsigned source=(unsigned)((snapshot>>(2*rule))&3);
  return source<kArRegionalSource_Count?kRules[rule].value[source]:UINT16_MAX;
}
bool ArRegionalBoss_GroupSource(const ArRegionalBossPolicy *policy,ArRegionalSource *source) {
  uint64_t snapshot;if(!source || !ArRegionalBoss_Resolve(policy,&snapshot))return false;
  for(unsigned region=0;region<kArRegionalSource_Count;++region) {
    bool match=true;
    for(unsigned i=0;i<kArRegionalBoss_Count;++i)match&=ArRegionalBoss_Value(snapshot,i)==kRules[i].value[region];
    if(match){*source=(ArRegionalSource)region;return true;}
  }
  return false;
}
bool ArRegionalBoss_MinoRow(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    uint16_t *duration,int16_t dx,int16_t dy) {
  if(!duration || dx || dy)return false;
  unsigned rule;
  if(state==0 && !row)rule=kArRegionalBoss_MinoIdle;
  else if(state==2 && row==4)rule=kArRegionalBoss_MinoThrowEnd;
  else if(state==1 && !row)rule=kArRegionalBoss_MinoThrowWindup;
  else if(state==4 && !row)rule=kArRegionalBoss_MinoJumpWindup;
  else return false;
  const uint16_t next=ArRegionalBoss_Value(snapshot,rule);
  if(next==UINT16_MAX || *duration!=kRules[rule].value[0])return false;
  *duration=next;return true;
}
bool ArRegionalBoss_IceSkip(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,unsigned *next) {
  if(!next || ArRegionalBoss_Value(snapshot,kArRegionalBoss_IceWindup)!=106 ||
      !((state==17 && row==5) || (state==18 && row==6)))return false;
  *next=row+2;return true;
}
bool ArRegionalBoss_PharaohSkip(ArRegionalBossSnapshot snapshot,bool rematch,
    unsigned state,unsigned row,unsigned *next) {
  if(!next || state!=11 || row!=10 || ArRegionalBoss_Value(snapshot,
      rematch?kArRegionalBoss_PharaohRematchLanding:kArRegionalBoss_PharaohLanding)!=24)return false;
  *next=11;return true;
}
bool ArRegionalBoss_PlantPhase(ArRegionalBossSnapshot snapshot,unsigned previous,ArRegionalPlantPhase *next) {
  if(!next || ArRegionalBoss_Value(snapshot,kArRegionalBoss_PlantCycle)!=1)return false;
  switch(previous) {
    case 0:case 20:*next=(ArRegionalPlantPhase){1,1};return true;
    case 1:*next=(ArRegionalPlantPhase){2,5};return true;
    case 2:*next=(ArRegionalPlantPhase){3,1};return true;
    case 3:*next=(ArRegionalPlantPhase){20,1};return true;
    default:return false;
  }
}
bool ArRegionalBoss_PlantOpenRow(ArRegionalBossSnapshot snapshot,unsigned row,unsigned *native_row,uint8_t *visual) {
  if(!native_row || !visual || row>4 || ArRegionalBoss_Value(snapshot,kArRegionalBoss_PlantOpen)!=16)return false;
  static const uint8_t rows[]={0,0,1,0,2},poses[]={2,3,4,3,255};
  *native_row=rows[row];*visual=poses[row];return true;
}
bool ArRegionalBoss_PlantWindup(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    uint8_t visual,uint16_t *duration,int16_t dx,int16_t dy) {
  if(!duration || *duration!=7 || row || dx || dy ||
      !((state==4 && visual==5) || (state==6 && visual==6)))return false;
  const unsigned rule=state==6?kArRegionalBoss_PlantHighWindup:kArRegionalBoss_PlantLowWindup;
  if(ArRegionalBoss_Value(snapshot,rule)!=23)return false;
  *duration=23;return true;
}
bool ArRegionalBoss_NorthwallRow(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    unsigned *native_row,uint16_t *duration,unsigned *expansion,bool *end) {
  if(!native_row || !duration || !expansion || !end)return false;
  if(state==1 && row<=10 && ArRegionalBoss_Value(snapshot,kArRegionalBoss_NorthwallImpact)==20) {
    *native_row=row==10?6:row>5?5:row;
    *duration=1;*expansion=row>=6 && row<10?row-5:0;*end=row==10;return true;
  }
  if(state==2 && row<=7 && ArRegionalBoss_Value(snapshot,kArRegionalBoss_NorthwallThrow)==15) {
    *native_row=row;*duration=row==3?2:1;*expansion=0;*end=row==7;return true;
  }
  return false;
}
bool ArRegionalBoss_TanzraRow(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    uint16_t *duration,int16_t dx,int16_t dy) {
  if(!duration || dy)return false;
  unsigned rule;
  if(state==10 && row==7 && !dx)rule=kArRegionalBoss_TanzraClosing;
  else if(state==48 && !row && dx==-4)rule=kArRegionalBoss_TanzraUpperTurn;
  else return false;
  const uint16_t value=ArRegionalBoss_Value(snapshot,rule);
  if(value==UINT16_MAX || *duration!=kRules[rule].value[0])return false;
  *duration=value;return true;
}
bool ArRegionalBoss_TanzraMinionSkip(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,unsigned *next) {
  if(!next || ArRegionalBoss_Value(snapshot,kArRegionalBoss_TanzraMinionTurn)!=8 || state!=22 || row)return false;
  *next=2;return true;
}
bool ArRegionalBoss_DragonRow(ArRegionalBossSnapshot snapshot,unsigned state,unsigned row,
    uint8_t visual,uint16_t *duration,int16_t dx,int16_t dy) {
  if(!duration || *duration || row || (state!=1 && state!=2) ||
      visual!=34-state || dx!=-3 || dy!=(state==1?1:-1))return false;
  const uint16_t next=ArRegionalBoss_Value(snapshot,kArRegionalBoss_DragonProjectileDelay);
  if(next!=15)return false;
  *duration=next;return true;
}
bool ArRegionalBoss_ViperRow(ArRegionalBossSnapshot snapshot,bool rematch,unsigned state,unsigned row,
    uint8_t visual,uint16_t *duration,int16_t dx,int16_t *dy) {
  if(!duration || !dy)return false;
  if(state>=4 && state<=6 && !row) {
    const unsigned rule=rematch?kArRegionalBoss_ViperRematchLightning:kArRegionalBoss_ViperLightning;
    static const uint8_t visuals[]={17,25,26};
    const int velocity=rematch?8:4;
    if(visual!=visuals[state-4] || *duration!=(rematch?10:21) ||
        dx!=((int)state-6)*velocity/2 || *dy!=velocity)return false;
    const uint16_t next=ArRegionalBoss_Value(snapshot,rule);
    if(next!=(rematch?8:17))return false;
    *duration=next;return true;
  }
  if(!rematch && (state==8 || state==9) && row>=6 && row<=9 && !dx &&
      visual==(state==8?27:16) && ArRegionalBoss_Value(snapshot,kArRegionalBoss_ViperFloor)==15) {
    static const uint8_t us_delay[]={1,1,1,15},us_dy[]={1,2,4,6},pal_delay[]={2,1,1,7},pal_dy[]={2,4,8,10};
    const unsigned i=row-6;if(*duration!=us_delay[i] || *dy!=us_dy[i])return false;
    *duration=pal_delay[i];*dy=pal_dy[i];return true;
  }
  return false;
}
