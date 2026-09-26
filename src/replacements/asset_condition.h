#ifndef ASSET_CONDITION_H
#define ASSET_CONDITION_H

#include <stdbool.h>
#include "snesrecomp/game/types.h"
#include "snesrecomp/runner.h"

/* Shared manifest gates for graphics and music. Parsing mutates its text.
 * Matching a supplied snapshot does not query or modify the running game. */
typedef enum AssetConditionKind {
  kAssetCondition_WramByte = 0,  /* g_ram[address] vs value */
  kAssetCondition_BgMode = 1,    /* (bgmode & 7) vs value */
  kAssetCondition_M7Element = 2, /* (uint16)m7matrix[address] vs value */
  kAssetCondition_M7Identity = 3,
} AssetConditionKind;

typedef struct AssetCondition {
  uint8 kind;
  uint8 negate; /* 0: ==, 1: != */
  uint16 address;
  uint16 value;
} AssetCondition;


enum { kAssetMaxConditions = 8 };
bool AssetCondition_Parse(char *term, AssetCondition *condition);
bool AssetConditions_ParseWhen(char *text, AssetCondition *conditions, int max, int *count);
bool AssetCondition_Matches(const AssetCondition *condition, const uint8 *wram,
                            const SrPpuStateSnapshot *ppu);

/* Live music selection uses the current runner. Bind/clear with its lifecycle;
 * graphics capture passes its coherent frame snapshot to Matches directly. */
void AssetConditions_BindRunner(SrRunnerHandle *runner);
bool AssetCondition_Passes(const AssetCondition *condition);
#endif
