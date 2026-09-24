#include "randomizer_config.h"
#include "byte_order.h"
#include <string.h>

RandomizerConfig RandomizerConfig_Default(void) {
  return (RandomizerConfig){.generator=kRandomizerGenerator,.seed=1,
      .hp_percent=100,.attack_percent=100,.enemy_scope=1};
}
bool RandomizerConfig_Valid(const RandomizerConfig *c) {
  if(!c)return false;
  if(!c->generator)
    return !c->seed && !c->hp_percent && !c->attack_percent && !c->enabled &&
        !c->enemy_types && !c->enemy_scope && !c->statue_drops && !c->statue_spots &&
        !c->lair_spots && !c->lair_types && !c->regional_action && !c->regional_towns;
  return c->generator==kRandomizerGenerator && c->seed<=999999999 &&
      c->hp_percent>=10 && c->hp_percent<=1000 && c->attack_percent>=10 && c->attack_percent<=1000 &&
      c->enemy_types<=1 && c->enemy_scope<=1 && c->statue_drops<=2 && c->statue_spots<=1 &&
      c->lair_spots<=1 && c->lair_types<=2;
}
bool RandomizerConfig_Encode(const RandomizerConfig *c,uint8_t out[kRandomizerConfigBytes]) {
  if(!out || !RandomizerConfig_Valid(c))return false;
  uint8_t bytes[kRandomizerConfigBytes]="ARRANDO1";
  bytes[8]=c->generator;bytes[9]=c->enabled;
  ByteOrder_WriteLe32(bytes+10,c->seed);
  ByteOrder_WriteLe16(bytes+14,c->hp_percent);ByteOrder_WriteLe16(bytes+16,c->attack_percent);
  bytes[18]=c->enemy_types;bytes[19]=c->enemy_scope;
  bytes[20]=c->statue_drops;bytes[21]=c->statue_spots;
  bytes[22]=c->lair_spots;bytes[23]=c->lair_types;
  bytes[24]=c->regional_action;bytes[25]=c->regional_towns;
  memcpy(out,bytes,sizeof(bytes));return true;
}
bool RandomizerConfig_Decode(const uint8_t *bytes,size_t size,RandomizerConfig *out) {
  if(!bytes || !out || size!=kRandomizerConfigBytes || memcmp(bytes,"ARRANDO1",8) ||
      bytes[9]>1 || bytes[24]>1 || bytes[25]>1 || bytes[26] || bytes[27])return false;
  RandomizerConfig c={.generator=bytes[8],.enabled=bytes[9],.seed=ByteOrder_ReadLe32(bytes+10),
      .hp_percent=ByteOrder_ReadLe16(bytes+14),.attack_percent=ByteOrder_ReadLe16(bytes+16),
      .enemy_types=bytes[18],.enemy_scope=bytes[19],.statue_drops=bytes[20],.statue_spots=bytes[21],
      .lair_spots=bytes[22],.lair_types=bytes[23],.regional_action=bytes[24],.regional_towns=bytes[25]};
  if(!RandomizerConfig_Valid(&c))return false;
  *out=c;return true;
}
