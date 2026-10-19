#ifndef AR_ACTRAISER_CHURCH_CAPTURE_H
#define AR_ACTRAISER_CHURCH_CAPTURE_H

#include "actraiser/actraiser_sprite_ownership.h"
#include "sim/church/church_art.h"
#include "snesrecomp/runner.h"

/* Game-owner capture. Publish a value-owned mask only after matching scanout. */
void ActRaiserChurchCapture_BeginFrame(void);
void ActRaiserChurchCapture_Prepare(const SnesRunnerApi *api, SrRunnerHandle *runner,
                                    const SrPpuFrameTransactionContext *frame,
                                    const ActRaiserSpriteOwnership *ownership);
void ActRaiserChurchCapture_Complete(SrResult status, const SrPpuScanoutResult *result);
ChurchActorMask ActRaiserChurchCapture_Mask(void);

#endif
