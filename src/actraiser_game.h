#ifndef ACTRAISER_GAME_H
#define ACTRAISER_GAME_H

/* Semantic names for ActRaiser state that has already been established in
 * docs/ram-map.md, docs/SEAMS.md, and docs/rendering-engine.md. Keep uncertain
 * direct-page scratch and polymorphic object fields local to the routine that
 * interprets them; this header is only for meanings that are stable across the
 * handwritten game-specific code. */

#include "types.h"

enum {
  kActRaiserWramSize = 0x20000,
};

extern uint8 g_ram[kActRaiserWramSize];

typedef enum ActRaiserMapGroup {
  kActRaiserMapGroup_NonAction = 0x00,
  kActRaiserMapGroup_Fillmore = 0x01,
  kActRaiserMapGroup_Bloodpool = 0x02,
  kActRaiserMapGroup_Kasandora = 0x03,
  kActRaiserMapGroup_Aitos = 0x04,
  kActRaiserMapGroup_Marahna = 0x05,
  kActRaiserMapGroup_Northwall = 0x06,
  kActRaiserMapGroup_DeathHeim = 0x07,
  kActRaiserMapGroup_Ending = 0x08,
} ActRaiserMapGroup;

typedef enum ActRaiserNonActionMap {
  kActRaiserNonActionMap_Title = 0x00,
  kActRaiserNonActionMap_Fillmore = 0x01,
  kActRaiserNonActionMap_Bloodpool = 0x02,
  kActRaiserNonActionMap_Kasandora = 0x03,
  kActRaiserNonActionMap_Aitos = 0x04,
  kActRaiserNonActionMap_Marahna = 0x05,
  kActRaiserNonActionMap_Northwall = 0x06,
  kActRaiserNonActionMap_SkyPalace = 0x07,
  kActRaiserNonActionMap_Temple = 0x08,
  kActRaiserNonActionMap_WorldMap = 0x09,
} ActRaiserNonActionMap;

enum {
  kActRaiserActionMapGroup_First = kActRaiserMapGroup_Fillmore,
  kActRaiserActionMapGroup_Last = kActRaiserMapGroup_DeathHeim,
  kActRaiserSimulationTown_First = kActRaiserNonActionMap_Fillmore,
  kActRaiserSimulationTown_Last = kActRaiserNonActionMap_Northwall,

  kActRaiserDeathHeimMap_Hub = 0x01,
  kActRaiserDeathHeimMap_FirstBoss = 0x02,
  kActRaiserDeathHeimMap_LastBoss = 0x07,
  kActRaiserDeathHeimMap_FinalBoss = 0x08,
  kActRaiserDeathHeimProgress_FinalBossBeaten = 0x07,
  kActRaiserDeathHeimEndingState_SkySettled = 0x03,
};

/* Stable low-WRAM state addresses. These are offsets within g_ram's $7E bank
 * mirror, not general SNES bus addresses. */
enum {
  kActRaiserWram_MapGroup = 0x0018,
  kActRaiserWram_CurrentMap = 0x0019,
  kActRaiserWram_DestinationMap = 0x001A,
  kActRaiserWram_DestinationMapGroup = 0x001B,
  kActRaiserWram_Lives = 0x001C,
  kActRaiserWram_PlayerHp = 0x001D,
  kActRaiserWram_WorkingMagicPoints = 0x0021,
  kActRaiserWram_Bg1CameraX = 0x0022,
  kActRaiserWram_Bg1CameraY = 0x0024,
  kActRaiserWram_Bg2CameraX = 0x0026,
  kActRaiserWram_Bg2CameraY = 0x0028,
  kActRaiserWram_Bg1Width = 0x002E,
  kActRaiserWram_Bg1Height = 0x0030,
  kActRaiserWram_Bg2Width = 0x0032,
  kActRaiserWram_Bg2Height = 0x0034,
  kActRaiserWram_GameFrame = 0x0088,
  kActRaiserWram_InputHeldHigh = 0x00A0,
  kActRaiserWram_ActionTimerLow = 0x00E6,
  kActRaiserWram_ActionTimerHigh = 0x00E7,
  kActRaiserWram_RangedSwordFlag = 0x00E4,
  kActRaiserWram_InputEnableMask = 0x00F4,
  kActRaiserWram_MagicCastState = 0x00F8,
  kActRaiserWram_TransitionRequest = 0x00FB,

  kActRaiserWram_SelectedMagic = 0x02AC,
  kActRaiserWram_AngelCurrentSp = 0x0282,
  kActRaiserWram_AngelMaximumSp = 0x0284,
  kActRaiserWram_AngelCurrentHp = 0x0286,
  kActRaiserWram_AngelMaximumHp = 0x0287,
  kActRaiserWram_PersistentMagicPoints = 0x0295,
  kActRaiserWram_MagicInventory = 0x0299,

  kActRaiserWram_DeathHeimEndingState = 0x0334,
  kActRaiserWram_DeathHeimProgress = 0x0347,
  kActRaiserWram_CopRequest = 0x035A,
  kActRaiserWram_BrkSoundRequest = 0x035B,

  kActRaiserWram_ActionObjectTable = 0x06A0,
  kActRaiserWram_PlayerObject = 0x08A0,
  kActRaiserWram_PlayerPositionX = 0x08A2,
  kActRaiserWram_PlayerPositionY = 0x08A4,
  kActRaiserWram_PlayerVelocityX = 0x08A6,
  kActRaiserWram_PlayerVelocityY = 0x08A8,
  kActRaiserWram_PlayerHandler = 0x08B2,
  kActRaiserWram_PlayerCrest = 0x08BC,
  kActRaiserWram_PlayerBoost = 0x08C4,
  kActRaiserWram_PlayerInvulnerabilityTimer = 0x08C6,
  kActRaiserWram_PlayerFlags = 0x08D0,

  kActRaiserWram_SimFixedRecords = 0x06A0,
  kActRaiserWram_SimWorldRecords = 0x0A00,
  kActRaiserWram_SimCameraTargetX = 0x0AEE,
  kActRaiserWram_SimCameraTargetY = 0x0AF0,
};

enum {
  kActRaiserAuthenticWidth = 256,
  kActRaiserAuthenticHeight = 224,
  kActRaiserTownWorldWidth = 512,
  kActRaiserTownCameraMaximumX = 256,
  kActRaiserTownCameraMaximumY = 0x011F,

  kActRaiserActionObjectStride = 0x40,
  kActRaiserActionObjectCount = 80,
  kActRaiserSimFixedRecordStride = 0x12,
  kActRaiserSimFixedRecordCount = 48,
  kActRaiserSimWorldRecordStride = 0x26,
  kActRaiserSimWorldRecordCount = 44,

  kActRaiserOamShadow = 0x0380,
  kActRaiserOamLowTableBytes = 0x0200,
  kActRaiserOamHighTable = 0x0580,

  kActRaiserBg1TilemapVram = 0x6000,
  kActRaiserBg2TilemapVram = 0x7000,
  kActRaiserTilemapWords = 0x1000,
  kActRaiserBgLayerStateStride = 4,

  kActRaiserBg1ColumnRecord = 0x3900,
  kActRaiserBg1RowRecord = 0x3A02,
  kActRaiserBg2ColumnRecord = 0x3B04,
  kActRaiserBg2RowRecord = 0x3C06,

  kActRaiserBgLayerMask_Bg1 = 0x01,
  kActRaiserBgLayerMask_Bg2 = 0x02,
  kActRaiserBgLayerMask_Bg1AndBg2 = 0x03,
  kActRaiserPpuLayer_Bg1 = 0,
  kActRaiserPpuLayer_Bg2 = 1,
  kActRaiserPpuLayer_Bg3 = 2,

  kActRaiserActionHudHeight = 40,
  kActRaiserActionHudLeftEnd = 88,
  kActRaiserActionHudRightStart = 168,
  kActRaiserActionHudLowerRowY = 28,
  kActRaiserSimulationHudHeight = 32,
  kActRaiserSimulationHudSplit = 168,
  kActRaiserHudObjOamFirst = 0,
  kActRaiserHudObjOamCount = 4,
  kActRaiserMagicHudFirstTile = 0xD4,
  kActRaiserHudObjUpperY = 0x0B,
  kActRaiserHudObjLowerY = 0x13,
  kActRaiserSimulationHourglassLeftX = 0x94,
  kActRaiserSimulationHourglassRightX = 0x9B,
  kActRaiserSimulationHourglassFirstUpperTile = 0xEC,
  kActRaiserSimulationHourglassFrameCount = 4,
  kActRaiserSimulationHourglassLowerTileOffset = 0x10,
  kActRaiserSimulationHourglassLeftAttr = 0x31,
  kActRaiserSimulationHourglassRightAttr = 0x71,

  kActRaiserTransitionRequestBit = 0x80,
  kActRaiserObjectStatus_End = 0x8000,
  kActRaiserObjectStatus_NoDraw = 0x2000,
  kActRaiserObjectStatus_IneligibleMask = 0x4C00,
  kActRaiserObjectFlag_OutsideActivation = 0x0400,
  kActRaiserPlayerFlag_Invulnerable = 0x2000,
  kActRaiserPlayerFlag_InvulnerableHighByte = 0x20,
  kActRaiserUnknownMapGroup = 0xFF,
};

static inline uint16 ActRaiser_ReadWram16(uint16 address) {
  return (uint16)(g_ram[address] |
                  (g_ram[(uint16)(address + 1)] << 8));
}

static inline void ActRaiser_WriteWram16(uint16 address, uint16 value) {
  g_ram[address] = (uint8)value;
  g_ram[(uint16)(address + 1)] = (uint8)(value >> 8);
}

static inline int ActRaiser_IsActionMapGroup(uint8 map_group) {
  return map_group >= kActRaiserActionMapGroup_First &&
         map_group <= kActRaiserActionMapGroup_Last;
}

static inline int ActRaiser_IsSimulationTown(uint8 map_group,
                                             uint8 map_number) {
  return map_group == kActRaiserMapGroup_NonAction &&
         map_number >= kActRaiserSimulationTown_First &&
         map_number <= kActRaiserSimulationTown_Last;
}

#endif  /* ACTRAISER_GAME_H */
