#ifndef AR_FRAME_SLOT_H
#define AR_FRAME_SLOT_H

/* Internal dependencies of the sole FrameSlot producer. The public capture API
 * and FrameSlot type remain in present.h. */

#include "present/present.h"   /* FrameSlot */
#include "sim/sim3d/sim3d.h"     /* Sim3DTuning */

/* Shared by DrawAndPresentFrame's canonical annotation and FrameSlot_Capture's
 * fallback annotation. */
Sim3DTuning BuildSim3DTuning(void);

/* Clear presentation-only action-effect lifecycle history at discontinuities
 * such as savestate loads. The next capture starts fresh from restored WRAM. */
void FrameSlot_ResetActionEffects(void);

#endif /* AR_FRAME_SLOT_H */
