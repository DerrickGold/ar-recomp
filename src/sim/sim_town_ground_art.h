#ifndef SIM_TOWN_GROUND_ART_H
#define SIM_TOWN_GROUND_ART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sim_world_map.h"

/* Immutable native SIM terrain art, independent of the resident town/PPU.
 * The two development CHR banks and temperate/snow palettes form four small
 * lazily decoded atlases. Init copies only bounded immutable source data;
 * disabling detailed ground avoids the decoding/allocation entirely.
 * This is source artwork, not a camera-projected canvas:
 * consumers must separate upright mountain/forest/building art from ground. */
bool SimTownGroundArt_Init(const uint8_t *rom, size_t rom_size);
void SimTownGroundArt_Shutdown(void);
bool SimTownGroundArt_Available(void);

enum { kSimTownGroundAnimationFrames = 4, kSimTownGroundAnimationTicks = 8 };
/* Stock SIM profile 2 uploads eight 4bpp characters per phase. An unknown
 * profile keeps static art rather than interpreting unrelated ROM bytes. */
bool SimTownGroundArt_AnimationAvailable(void);
/* Navigation continues the native four-phase/eight-tick cadence from its
 * captured game clock, not wall time or the last resident town's DMA state. */
uint8_t SimTownGroundArt_AnimationPhase(uint16_t game_frame);

/* A tightly packed 16x16 ARGB8888 terrain metatile. Index zero remains
 * transparent so an absent source cannot paint a black hole. Borrowed until
 * shutdown/reinitialization. No emulated memory or GPU resources are exposed. */
const uint32_t *SimTownGroundArt_Metatile(
    uint8_t town, uint8_t development_tier, uint8_t tile);

/* Static source-palette identity, not a comparison of expanded RGB colours.
 * Writes a tightly packed 16x16 byte mask (0 or 1). Transparent pixels never
 * match. Invalid inputs leave the destination unchanged. */
bool SimTownGroundArt_ColorIndexMask(
    uint8_t town, uint8_t development_tier, uint8_t tile,
    uint8_t color_index, uint8_t *mask);
/* Conservative semantic full-water classification. Every texel in every
 * authored animation phase must use water/wave palette identities; shore,
 * marsh, transparent and land pixels reject it. Cached without pixel atlases. */
bool SimTownGroundArt_IsOpenWater(uint8_t town, uint8_t development_tier, uint8_t tile);
/* Stable 16x16 byte mask (0 or 1), including water pixels in mixed shores.
 * Uses the same all-phase rule as IsOpenWater. Invalid inputs leave output
 * unchanged. Lazy classification is owner-thread work, not a worker call. */
bool SimTownGroundArt_OpenWaterMask(uint8_t town, uint8_t development_tier,
    uint8_t tile, uint8_t mask[256]);

/* Immutable, lazily decoded phase variant. Unaffected metatiles retain the
 * static pointer; only metatiles referencing the animated CHR strip allocate
 * additional pixels. Animation allocation failure keeps that variant static
 * until reinitialization. Phase zero is exactly Metatile(). Invalid phase returns
 * NULL; an unsupported animation profile returns static art for every phase.
 * Resolves the whole requested phase before returning even an ordinary tile,
 * allowing callers to validate allocations before changing an output atlas. */
const uint32_t *SimTownGroundArt_AnimatedMetatile(
    uint8_t town, uint8_t development_tier, uint8_t tile, uint8_t phase);

#endif
