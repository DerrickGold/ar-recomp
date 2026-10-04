#ifndef AR_SIM_FRAME_CAPTURE_H
#define AR_SIM_FRAME_CAPTURE_H
/* SIM capture and resource preparation. Synchronous scenes run both after
 * the PPU draw. Streaming towns capture immutable metadata/source memory on
 * the producer; the presentation owner builds derived world/town/voxel caches
 * from the acquired packet before upload. Inspector work stays synchronous. */

#include "sim/sim_render_metadata.h"
#include "actraiser_game.h"
#include "snesrecomp/runner.h"

/* Small owned source snapshot, not a copy of the derived multi-megabyte
 * canvas/world/voxel caches. Only the presentation owner builds those caches. */
typedef struct SimFrameInputs {
  SrPpuStateSnapshot ppu;
  uint8_t wram[kActRaiserWramSize];
  uint16_t vram[SR_PPU_VRAM_WORD_COUNT];
  uint16_t cgram[SR_PPU_CGRAM_WORD_COUNT];
  bool ppu_valid;
} SimFrameInputs;

/* Streaming producer: captures metadata and source memory without touching
 * presentation caches or host camera controls. Called only for town scenes. */
void SimFrameCapture_ProduceOwned(SimFrameData *sim, SimFrameInputs *inputs,
                                 uint32_t underlay_serial);
/* Presentation owner, before upload, while the packet is still acquired. */
void SimFrameCapture_PrepareOwned(SimFrameData *sim, const SimFrameInputs *inputs);

void SimFrameCapture_Produce(SimFrameData *sim);
/* Screenshots and paused redraws capture current metadata/tuning without
 * rebuilding the world map or town canvas, or advancing traces. The canvas
 * serials refer to the last produced frame. Normal submission instead copies
 * Produce's completed snapshot, including its newly rendered canvas serials. */
void SimFrameCapture_RefreshMetadata(SimFrameData *sim);
/* Releases the town canvas worker group. */
void SimFrameCapture_Shutdown(void);

#endif  /* AR_SIM_FRAME_CAPTURE_H */
