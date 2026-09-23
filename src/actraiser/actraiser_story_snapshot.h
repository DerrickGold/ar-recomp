#ifndef ACTRAISER_STORY_SNAPSHOT_H
#define ACTRAISER_STORY_SNAPSHOT_H

#include "save_system.h"
#include "snesrecomp/game/cpu.h"

/* Read-only projection of the US $03:A656 native save writer. The caller must
 * hold a quiescent story boundary with all town actor caches already flushed;
 * this does NOT run $03:8168, complete construction, or make a live SIM frame
 * safe to save. No CPU/WRAM/SRAM writes, call frames, policy or file operations.
 * Start with current SRAM so reserved bytes and native extensions survive.
 * out must be separate from live game memory. NULL arguments leave it alone.
 * Native saves still use the original writer; original-CPU parity tests guard
 * this snapshot projection against its copy/checksum contract. */
bool ActRaiserStorySnapshot_Capture(CpuState *cpu,
                                   uint8_t out[kActRaiserSramSize]);

#endif
