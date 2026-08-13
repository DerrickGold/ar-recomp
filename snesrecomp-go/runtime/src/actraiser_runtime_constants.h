#ifndef ACTRAISER_RUNTIME_CONSTANTS_H
#define ACTRAISER_RUNTIME_CONSTANTS_H

/* ActRaiser-specific WRAM offsets used by optional runtime diagnostics. Game
 * code exposes its richer semantic map through actraiser_game.h, deriving the
 * overlapping entries from these literals so the two layers cannot drift. */
#define kActRaiserRuntimeWram_MapGroup 0x0018
#define kActRaiserRuntimeWram_CurrentMap 0x0019
#define kActRaiserRuntimeWram_GameFrame 0x0088

#endif  /* ACTRAISER_RUNTIME_CONSTANTS_H */
