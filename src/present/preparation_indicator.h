#ifndef AR_PREPARATION_INDICATOR_H
#define AR_PREPARATION_INDICATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dev/dev_tools_readback.h"
#include "render/render_device.h"

/* What the screen shows while boot preparation compiles GPU pipelines: the
 * angel's HUD health bar with a red "lost health" window sweeping across it
 * and the angel flapping above its centre, all decoded from the player's ROM. A ROM that
 * fails validation keeps a plain bar. Nothing here touches game state.
 *
 * AR_PREPARATION_INDICATOR=force shows it immediately and holds it for one
 * full sweep at the end of preparation, so it can be inspected on hosts whose
 * drivers cache every pipeline; =off disables it. */

enum {
  kPreparationIndicatorDelayMs = 300,
  kPreparationIndicatorIntervalMs = 66, /* each present waits for vsync */
  /* One way; a full cycle (twice this) is exactly ten angel flap cycles of
   * four 67 ms poses, so the animation loops without a seam. */
  kPreparationIndicatorSweepMs = 1340,
  kPreparationIndicatorForcedHoldMs = 2 * kPreparationIndicatorSweepMs,
  /* Atlas: blue pill, red pill, then four angel cells. */
  kPreparationPillWidth = 4,
  kPreparationPillHeight = 8,
  kPreparationAngelFrames = 4,
  kPreparationAngelCell = 24,
  kPreparationAtlasWidth = 2 * kPreparationPillWidth +
                           kPreparationAngelFrames * kPreparationAngelCell,
  kPreparationAtlasHeight = kPreparationAngelCell,
  kPreparationBarPills = 16,
  kPreparationLostPills = 4,
  kPreparationSpriteCapacity = kPreparationBarPills + 1,
};

typedef struct PreparationSprite {
  ArRenderRectF source;      /* atlas pixels */
  ArRenderRectF destination; /* output pixels */
} PreparationSprite;

/* Decodes the atlas from a US ROM image; false keeps the plain bar. Copies
 * everything it needs, so the ROM may be released afterwards. */
bool PreparationIndicator_DecodeRom(const uint8_t *rom, size_t size);
bool PreparationIndicator_HasArt(void);
/* ARGB8888 atlas, kPreparationAtlasWidth x kPreparationAtlasHeight. */
const uint32_t *PreparationIndicator_AtlasPixels(void);

/* Pure placement of the bar and angel for an output size and elapsed time.
 * Returns the sprite count written (at most kPreparationSpriteCapacity). */
int PreparationIndicator_Layout(int width, int height, uint64_t elapsed_ms,
                                PreparationSprite *sprites);

/* Developer capture (AR_PREPARATION_INDICATOR_CAPTURE=<directory>): every
 * presented indicator frame is also read back and written there as
 * NNNN-<elapsed ms>.ppm. The host supplies the readback; an empty directory disables. */
void PreparationIndicator_SetCapture(DevToolsReadbackProvider readback,
                                     const char *directory);

/* Owner-thread drawing around RenderPreparation_Prepare. Tick draws and
 * presents only past the delay, at the throttle interval, and only while the
 * window is the current target. */
void PreparationIndicator_Begin(ArRenderDevice *device);
void PreparationIndicator_Tick(ArRenderDevice *device);
void PreparationIndicator_End(ArRenderDevice *device);

#endif /* AR_PREPARATION_INDICATOR_H */
