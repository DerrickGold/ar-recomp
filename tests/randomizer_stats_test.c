#include "randomizer.h"
#include "settings.h"
#include "byte_order.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

Settings g_settings;
const SettingDesc *Settings_Find(const char *key) {(void)key;return NULL;}
SettingChangeResult Settings_SetLong(const SettingDesc *d,long value) {(void)d;(void)value;return 0;}
static uint8_t rom[0x100000],pristine[0x100000];

static unsigned Expected(unsigned base,unsigned percent) {
  if(!base)return 0;
  const unsigned scaled=(base*percent+50)/100;
  return scaled<1?1:scaled>255?255:scaled;
}
static void ExpectBasis(unsigned offset,unsigned hp,unsigned attack,unsigned hp_scale,unsigned attack_scale) {
  RandomizerSpawnStatBasis basis;
  assert(Randomizer_SpawnStatBasis(offset,rom[offset+8],rom[offset+7],&basis));
  assert(basis.hp==hp && basis.attack==attack && basis.scale.hp_percent==(int)hp_scale && basis.scale.attack_percent==(int)attack_scale);
}
static void Reject(unsigned offset,unsigned hp,unsigned attack) {
  RandomizerSpawnStatBasis basis,before;
  memset(&basis,0xa5,sizeof(basis));before=basis;
  assert(!Randomizer_SpawnStatBasis(offset,hp,attack,&basis) && !memcmp(&basis,&before,sizeof(basis)));
}
int main(void) {
  RandomizerConfig recipe=RandomizerConfig_Default(),decoded;
  recipe.enabled=true;recipe.seed=999999999;recipe.hp_percent=1000;recipe.attack_percent=10;
  recipe.enemy_types=1;recipe.statue_drops=2;recipe.lair_types=2;recipe.regional_action=recipe.regional_towns=true;
  uint8_t encoded[kRandomizerConfigBytes],copy[kRandomizerConfigBytes];
  assert(RandomizerConfig_Encode(&recipe,encoded) && RandomizerConfig_Decode(encoded,sizeof(encoded),&decoded));
  assert(RandomizerConfig_Encode(&decoded,copy) && !memcmp(encoded,copy,sizeof(copy)));
  assert(ByteOrder_ReadLe32(encoded+10)==999999999 && ByteOrder_ReadLe16(encoded+14)==1000);
  assert(!RandomizerConfig_Decode(encoded,sizeof(encoded)-1,&decoded));
  const unsigned invalid_bytes[]={0,8,9,18,19,20,21,22,23,24,25,26,27};
  for(unsigned i=0;i<sizeof(invalid_bytes)/sizeof(invalid_bytes[0]);++i) {
    memcpy(copy,encoded,sizeof(copy));copy[invalid_bytes[i]]=255;
    assert(!RandomizerConfig_Decode(copy,sizeof(copy),&decoded));
  }
  for(unsigned value=0;value<256;++value)for(unsigned percent=10;percent<=1000;++percent)
    assert(Randomizer_ScaleStat((uint8_t)value,(int)percent)==Expected(value,percent));
  assert(Randomizer_ScaleStat(255,INT_MAX)==255 && Randomizer_ScaleStat(255,INT_MIN)==1);
  assert(Randomizer_ScaleStat(0,INT_MAX)==0);
  assert(Randomizer_AppliedStatScale().hp_percent==100);
  RandomizerSpawnStatBasis basis;
  assert(Randomizer_SpawnStatBasis(0x16cf,2,3,&basis) && basis.hp==2 && basis.attack==3 && basis.scale.hp_percent==100);

  const unsigned records[]={0x16cf,0x16df,0x16ef,0x16ff};
  for(unsigned i=0;i<4;++i) {
    const unsigned at=records[i];
    ByteOrder_WriteLe16(rom+0x16af+2*i,(uint16_t)(at+0x8000));
    ByteOrder_WriteLe16(rom+at,0x4000);rom[at+2]=0x7e;rom[at+7]=3;rom[at+8]=2;
  }
  ByteOrder_WriteLe16(rom+0x16b7,0x96cf); /* alias: one application */
  rom[0x16df+7]=0;rom[0x16df+8]=255;
  ByteOrder_WriteLe16(rom+0x16ef+4,0x200); /* pickups never scaled */
  rom[0x16ff+2]=0xa9; /* direct code, never a spawn definition */
  memcpy(pristine,rom,sizeof(rom));assert(Randomizer_Init(rom,sizeof(rom)));
  g_settings=(Settings){.rando_enable=true,.rando_enemy_hp=200,.rando_enemy_atk=125};
  Randomizer_Apply();
  assert(Randomizer_LastSummary()->enemy_records==2);
  assert(rom[0x16cf+8]==4 && rom[0x16cf+7]==4 && rom[0x16df+8]==255 && !rom[0x16df+7]);
  ExpectBasis(0x16cf,2,3,200,125);ExpectBasis(0x16df,255,0,200,125);
  ExpectBasis(0x16ef,2,3,100,100);ExpectBasis(0x16ff,2,3,100,100);
  assert(!memcmp(rom+0x16ef,pristine+0x16ef,12) && !memcmp(rom+0x16ff,pristine+0x16ff,12));
  /* Pending settings do not change the applied generation. */
  g_settings.rando_enemy_hp=10;g_settings.rando_enemy_atk=50;
  ExpectBasis(0x16cf,2,3,200,125);
  Reject(0x16cf,2,4);Reject(0x16cf,4,3);Reject(0x16cf,256,4);
  Reject(0x7fff,1,1);Reject(UINT32_MAX,1,1);
  assert(!Randomizer_SpawnStatBasis(0x16cf,4,4,NULL));
  for(unsigned byte=0;byte<12;++byte) {
    rom[0x16cf+byte]^=1;Reject(0x16cf,4,4);rom[0x16cf+byte]^=1;
  }
  Randomizer_Apply();
  assert(rom[0x16cf+8]==1 && rom[0x16cf+7]==2 && rom[0x16df+8]==26);
  ExpectBasis(0x16cf,2,3,10,50);ExpectBasis(0x16df,255,0,10,50);
  Randomizer_Apply();ExpectBasis(0x16cf,2,3,10,50);
  g_settings.rando_enable=false;Randomizer_Apply();
  assert(!memcmp(rom,pristine,sizeof(rom)) && !Randomizer_LastSummary()->applied);
  ExpectBasis(0x16cf,2,3,100,100);
  assert(Randomizer_AppliedStatScale().hp_percent==100 && Randomizer_AppliedStatScale().attack_percent==100);
  /* A bound save ignores changing global preferences and rejects rerolls;
   * returning to title restores the original draft, not the loaded recipe. */
  recipe=RandomizerConfig_Default();recipe.enabled=true;recipe.seed=12345;recipe.hp_percent=300;recipe.attack_percent=200;
  assert(Randomizer_BindCampaign(&recipe) && Randomizer_CampaignBound());
  assert(g_settings.rando_seed==12345 && rom[0x16cf+8]==6 && rom[0x16cf+7]==6);
  g_settings.rando_seed=777;g_settings.rando_enable=false;g_settings.rando_enemy_hp=1000;
  Randomizer_Apply();Randomizer_Reroll();
  assert(Randomizer_LastSummary()->seed==12345 && Randomizer_CurrentConfig().seed==12345 && rom[0x16cf+8]==6);
  RandomizerConfig invalid=recipe;invalid.generator=255;
  assert(!Randomizer_BindCampaign(&invalid) && Randomizer_CurrentConfig().seed==12345);
  Randomizer_ReleaseCampaign();
  assert(!Randomizer_CampaignBound() && !g_settings.rando_enable && g_settings.rando_enemy_hp==10 && g_settings.rando_enemy_atk==50);
  assert(!memcmp(rom,pristine,sizeof(rom)));
  puts("randomizer stats: rounding/clamps, pristine basis, aliases, guards and applied-generation reset passed");
  return 0;
}
