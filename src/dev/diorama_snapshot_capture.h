#ifndef AR_DIORAMA_SNAPSHOT_CAPTURE_H
#define AR_DIORAMA_SNAPSHOT_CAPTURE_H
#include "present/present.h"
/* Opt-in diagnostic: AR_DIORAMA_SNAPSHOT=output.ardi and optional
 * AR_DIORAMA_SNAPSHOT_AFTER=N (default 120 uploaded Diorama frames).
 * Disabled path allocates/copies nothing. Never reads producer pixels at Draw. */
void DioramaSnapshotCapture_Retain(const FrameSlot *slot,
    const uint8_t *const *pixels, const size_t *pitches, uint32_t mask,
    const SrPpuSurfaceView *skybox);
void DioramaSnapshotCapture_Write(const FrameSlot *slot,
    const DioramaCapture *capture, const DioramaView *view,
    const DioramaScene *scene);
void DioramaSnapshotCapture_Reset(void);
#endif
