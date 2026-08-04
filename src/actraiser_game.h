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

  /* World-navigation ($18/$19 = $00/$09) Mode-7 camera contract. The focus is
   * the canonical world point under screen centre; the current and staged
   * matrices are the exact transforms uploaded by $02:8384. */
  kActRaiserWram_WorldFocusX = 0x0300,
  kActRaiserWram_WorldFocusY = 0x0302,
  kActRaiserWram_WorldMatrixA = 0x0304,
  kActRaiserWram_WorldMatrixB = 0x0306,
  kActRaiserWram_WorldMatrixC = 0x0308,
  kActRaiserWram_WorldMatrixD = 0x030A,
  kActRaiserWram_WorldNextMatrixA = 0x030C,
  kActRaiserWram_WorldNextMatrixB = 0x030E,
  kActRaiserWram_WorldNextMatrixC = 0x0310,
  kActRaiserWram_WorldNextMatrixD = 0x0312,
  kActRaiserWram_WorldRotation = 0x0314,
  kActRaiserWram_WorldZoomCurrent = 0x0316,
  kActRaiserWram_WorldZoomTarget = 0x0318,

  kActRaiserWram_DeathHeimEndingState = 0x0334,
  /* $01:B6CA clears this, then writes the 1-based entry selected from the
   * seven region table at $01:B73C. Zero means outside every border. The same
   * value owns the world-map label and destination. */
  kActRaiserWram_WorldLocation = 0x0341,
  kActRaiserWram_DeathHeimProgress = 0x0347,
  kActRaiserWram_CopRequest = 0x035A,
  kActRaiserWram_BrkSoundRequest = 0x035B,

  kActRaiserWram_ActionObjectTable = 0x06A0,
  kActRaiserWram_MagicController = 0x0860,
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
  /* World record indices 6 and 7. The angel is driven by its own subsystem
   * (class $0C is a no-op handler); the arrow is an ordinary world record
   * whose $01:B473 lifetime/culling remains authentic gameplay. */
  kActRaiserWram_SimAngelRecord = 0x0AE4,
  kActRaiserWram_SimAngelArrowRecord = 0x0B0A,

  /* $7F-bank simulation command state. g_ram mirrors $7E:0000-$7F:FFFF,
   * hence the explicit +$10000 host offsets. $7F:9215 is set by the still-
   * unclassified type-$0B picker ($01:93E4), Direct the People's on-screen
   * Building Direction picker ($01:9737), and the shared targeted-miracle
   * picker ($01:975C); their confirm/cancel paths clear it.
   * $7F:7CA1 is the pending world/structure type consumed by the allocation
   * path. The $01:93DC picker stages $000B while Direct the People and the
   * targeted-miracle path both stage $0009, so it is evidence but not a complete
   * picker-operation discriminator. */
  kActRaiserWram_SimPendingWorldType = 0x17CA1,
  kActRaiserWram_SimAimedMapCellX = 0x190E1,
  kActRaiserWram_SimAimedMapCellY = 0x190E5,
  /* Shared miracle lifecycle. These are full $7F mirror addresses, just like
   * the picker and kind fields around them. The presentation snapshot reads
   * them on the game thread; present.c must never read this live state. */
  kActRaiserWram_SimUserMiracleActive = 0x190E9,
  kActRaiserWram_SimMiracleKind = 0x190EB,
  kActRaiserWram_SimMiracleVisualComplete = 0x190F1,
  kActRaiserWram_SimMiracleActorDone = 0x190F3,
  kActRaiserWram_SimPostedMiracleActive = 0x190F5,
  kActRaiserWram_SimMapPickerFlag = 0x19215,
};

enum {
  kActRaiserAuthenticWidth = 256,
  kActRaiserAuthenticHeight = 224,
  kActRaiserTownWorldWidth = 512,
  kActRaiserTownCameraMaximumX = 256,
  kActRaiserTownCameraMaximumY = 0x011F,

  kActRaiserActionObjectStride = 0x40,
  kActRaiserActionObjectCount = 80,
  kActRaiserActionMagicCohortCount = 7,
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
  kActRaiserActionHudPlayerRowY = 20,
  kActRaiserActionHudEnemyRowY = 28,
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
  kActRaiserSkyPalaceMagicOamFirst = 6,
  kActRaiserSkyPalaceMagicOamCount = 4,
  kActRaiserSkyPalaceMagicX = 0x94,
  kActRaiserSkyPalaceMagicY = 0x0B,
  kActRaiserSkyPalaceMagicLeftAttr = 0x39,
  kActRaiserSkyPalaceMagicRightAttr = 0x79,

  kActRaiserTransitionRequestBit = 0x80,
  kActRaiserObjectStatus_InactiveMask = 0xC000,
  kActRaiserObjectStatus_End = 0x8000,
  kActRaiserObjectStatus_NoDraw = 0x2000,
  kActRaiserObjectStatus_IneligibleMask = 0x4C00,
  kActRaiserObjectFlag_OutsideActivation = 0x0400,
  kActRaiserObjectFlip_Horizontal = 0x4000,
  kActRaiserObjectFlip_Vertical = 0x8000,
  kActRaiserObjectFlip_Mask = 0xC000,
  kActRaiserPlayerFlag_Invulnerable = 0x2000,
  kActRaiserPlayerFlag_InvulnerableHighByte = 0x20,
  kActRaiserUnknownMapGroup = 0xFF,
};

/* Stable layout of one $40-byte action object. Some fields are polymorphic
 * outside the animation/sprite pipeline; these names describe the established
 * rendering contract shared by the authentic OAM builder and host observers. */
typedef enum ActRaiserActionObjectField {
  kActRaiserActionObject_Status = 0x00,
  kActRaiserActionObject_WorldX = 0x02,
  kActRaiserActionObject_WorldY = 0x04,
  kActRaiserActionObject_VelocityX = 0x06,
  kActRaiserActionObject_VelocityY = 0x08,
  kActRaiserActionObject_LeftExtent = 0x0A,
  kActRaiserActionObject_TopExtent = 0x0C,
  kActRaiserActionObject_RightExtent = 0x0E,
  kActRaiserActionObject_BottomExtent = 0x10,
  kActRaiserActionObject_Handler = 0x12,
  kActRaiserActionObject_AnimationAddress = 0x16,
  /* BYTE. +$16..+$18 is one 24-bit pointer (addr16 + bank8); the byte at +$19
   * is the record's base OAM attribute, not the pointer's high half. Read this
   * field 8-bit — a 16-bit read silently returns bank | attributes<<8. */
  kActRaiserActionObject_AnimationBank = 0x18,
  kActRaiserActionObject_BaseAttributes = 0x19,   /* byte; mirrors +$29 */
  kActRaiserActionObject_AnimationState = 0x1A,
  kActRaiserActionObject_AnimationIndex = 0x1C,
  kActRaiserActionObject_Composition = 0x20,
  kActRaiserActionObject_Visual = 0x22,
  kActRaiserActionObject_Wait = 0x24,
  kActRaiserActionObject_FlipAttributes = 0x28,
  kActRaiserActionObject_Flags = 0x30,
  kActRaiserActionObject_LocalCounter = 0x38,
} ActRaiserActionObjectField;

/* Low-WRAM (bank $7E, addresses $0000-$FFFF) 16-bit access.
 *
 * The uint16 parameter is deliberate and load-bearing: passing a bank-$7F
 * semantic constant here is a TRUNCATION BUG, and the narrow type is what makes
 * the compiler say so. It has caught one real instance -- the sim map picker flag
 * at $7F:9215 (constant 0x19215) was read through this helper and silently became
 * 0x9215, so the picker never activated; -Wall reported "changes value from 102933
 * to 37397" and it went unnoticed among the pre-existing warnings in that file.
 *
 * If the address you have is >= 0x10000, you want ActRaiser_ReadWramMirror16
 * below. Do NOT widen this parameter to make a call compile. */
static inline uint16 ActRaiser_ReadWram16(uint16 address) {
  return (uint16)(g_ram[address] |
                  (g_ram[(uint16)(address + 1)] << 8));
}

static inline void ActRaiser_WriteWram16(uint16 address, uint16 value) {
  g_ram[address] = (uint8)value;
  g_ram[(uint16)(address + 1)] = (uint8)(value >> 8);
}

/* Full 128-KiB $7E/$7F mirror access. Keep this distinct from the established
 * low-WRAM helper so a bank-$7F semantic address cannot silently truncate to
 * 16 bits at the call boundary. */
static inline uint16 ActRaiser_ReadWramMirror16(uint32 address) {
  return (uint16)(g_ram[address] | (g_ram[address + 1] << 8));
}

static inline void ActRaiser_WriteWramMirror16(uint32 address, uint16 value) {
  g_ram[address] = (uint8)value;
  g_ram[address + 1] = (uint8)(value >> 8);
}

static inline int ActRaiser_IsActionMapGroup(uint8 map_group) {
  return map_group >= kActRaiserActionMapGroup_First &&
         map_group <= kActRaiserActionMapGroup_Last;
}

/* Live sprite-drawable margins either side of the authentic 256-pixel window,
 * in authentic pixels. This is the OAM emitter's own horizontal predicate, so
 * a presentation layer asking "where can a sprite actually appear?" gets the
 * same answer the emitter gives rather than re-deriving it. Zero outside a
 * simulation town or when widescreen sprite widening is off. */
void ActRaiser_SimSpriteMargins(int *left, int *right);

static inline int ActRaiser_IsSimulationTown(uint8 map_group,
                                             uint8 map_number) {
  return map_group == kActRaiserMapGroup_NonAction &&
         map_number >= kActRaiserSimulationTown_First &&
         map_number <= kActRaiserSimulationTown_Last;
}

/* Keep the pure form available to the capture and feature-mask tests. The ROM
 * uses the full word at $7F:9215, so any nonzero value means the authentic
 * top-down position-picker view is required. */
static inline int ActRaiser_SimMapPickerActiveForState(uint8 map_group,
                                                       uint8 map_number,
                                                       uint16 picker_flag) {
  return ActRaiser_IsSimulationTown(map_group, map_number) &&
         picker_flag != 0;
}

static inline int ActRaiser_SimMapPickerActive(void) {
  return ActRaiser_SimMapPickerActiveForState(
      g_ram[kActRaiserWram_MapGroup],
      g_ram[kActRaiserWram_CurrentMap],
      ActRaiser_ReadWramMirror16(kActRaiserWram_SimMapPickerFlag));
}

#endif  /* ACTRAISER_GAME_H */
