/* AR2 viewport hooks and exact OBJ positions, owned by the native module. */
#ifndef AR_AR2_SPRITES_H
#define AR_AR2_SPRITES_H
#include "snesrecomp/game/cpu.h"
#include "snesrecomp/runner.h"
#include "actraiser2/imported_game_api.h"

void Ar2Sprites_Reset(void);
void Ar2Sprites_ScreenRange(unsigned *first, unsigned *count);
void Ar2Sprites_SetView(ArImportedGameView view);
void Ar2Sprites_Checkpoint(uint32_t pc);
bool Ar2Sprites_Publish(SrRunnerHandle *runner, const SnesRunnerApi *api, int camera_shift);
bool Ar2Sprites_Wide(CpuState *cpu);
RecompReturn Ar2Sprites_CullActor(CpuState *cpu);
RecompReturn Ar2Sprites_CullActorVertical(CpuState *cpu);
RecompReturn Ar2Sprites_CullPieceVertical(CpuState *cpu);
RecompReturn Ar2Sprites_CullEffectVertical(CpuState *cpu);
RecompReturn Ar2Sprites_CullPiece(CpuState *cpu);
RecompReturn Ar2Sprites_CullEffect(CpuState *cpu);
#endif
