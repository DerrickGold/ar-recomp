#ifndef AR_ACTION_OBJECT_SNAPSHOT_CAPTURE_H
#define AR_ACTION_OBJECT_SNAPSHOT_CAPTURE_H
#include <stdbool.h>

struct FrameSlot;

/* Opt-in editor data acquisition on the producer, immediately after the
 * complete FrameSlot capture. No GPU state is read and no borrowed pointer
 * escapes. AR_EDITOR_OBJECT_CAPTURE_DIR selects an existing output directory;
 * FROM/TO/EVERY select inclusive native game frames (defaults 0/65534/1).
 * Disabled capture performs no allocation, pixel copy, or filesystem work. */
void ActionObjectSnapshotCapture_Capture(const struct FrameSlot *slot);
/* Diagnostic export additionally acquires bounded independent artwork for
 * otherwise-flat native actors. Ordinary gameplay pays no raster cost. */
bool ActionObjectSnapshotCapture_Enabled(void);

#endif
