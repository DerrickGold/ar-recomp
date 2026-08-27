#ifndef CONSTANTS_H
#define CONSTANTS_H

#include "snesrecomp/game/runtime_constants.h"

/* Cross-cutting compile-time constants only. This header deliberately owns no
 * functions, types, globals, or subsystem policy, so low-level and pure-math
 * modules can include it without pulling runtime state across boundaries. */

/* Stable game-space contracts. */
#define kActRaiserWramSize kSnesWramSize

/* Native display dimensions. */
#define kActRaiserAuthenticWidth 256
#define kActRaiserAuthenticHeight 224
/* The native action camera reserves row 224 in addition to the 224 rendered
 * rows, so its vertical clamp uses a 225-row viewport. */
#define kActRaiserActionCameraViewportHeight \
  (kActRaiserAuthenticHeight + 1)

/* Stable save-format cardinalities shared by the settings model, editor, and
 * SRAM codec. Keep these here so each layer cannot silently grow a different
 * view of the same on-cartridge layout. */
#define kActRaiserSaveRegionCount 6
#define kActRaiserSaveActCount 2
#define kActRaiserSaveMagicSlotCount 4
#define kActRaiserSaveItemSlotCount 8
#define kActRaiserMagicSpellCount 4
#define kActRaiserPlayerNameCharacterLimit 8
#define kActRaiserPlayerNameStorageBytes \
  (kActRaiserPlayerNameCharacterLimit + 1)

/* SPC sample-directory entries below this boundary are the shared sound-effect
 * bank; this entry and above belong to replaceable per-song music banks. */
#define kActRaiserSpcMusicSourceMinimum 0x0C

/* Integer wall-clock unit conversions. */
#define kMillisecondsPerSecond 1000
#define kNanosecondsPerMillisecond 1000000
#define kNanosecondsPerSecond \
  (kMillisecondsPerSecond * kNanosecondsPerMillisecond)

/* Standard capacity for resolved host filesystem paths. This is a buffer
 * contract, not a claim about the host OS's maximum path length; narrowly
 * scoped tools may retain a smaller named capacity. */
#define kHostPathCapacity 1024

/* Whole-value percentage scale shared by settings, UI, input, and tuning
 * calculations. Subsystems still own their allowed percentage ranges. */
#define kPercentScale 100

/* Whole-value thousandths used by camera angles, manual zoom, and fine UI
 * tuning. This is deliberately distinct from the millisecond conversion even
 * though both currently have the same numeric value. */
#define kPermilleScale 1000

#endif  /* CONSTANTS_H */
