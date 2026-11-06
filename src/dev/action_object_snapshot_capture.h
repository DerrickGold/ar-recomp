#ifndef AR_ACTION_OBJECT_SNAPSHOT_CAPTURE_H
#define AR_ACTION_OBJECT_SNAPSHOT_CAPTURE_H

struct FrameSlot;

/* Opt-in editor data acquisition on the producer, immediately after the
 * complete FrameSlot capture. No GPU state is read and no borrowed pointer
 * escapes. AR_EDITOR_OBJECT_CAPTURE_DIR selects an existing output directory;
 * FROM/TO/EVERY select inclusive native game frames (defaults 0/65534/1).
 * Disabled capture performs no allocation, pixel copy, or filesystem work. */
void ActionObjectSnapshotCapture_Capture(const struct FrameSlot *slot);

#endif
