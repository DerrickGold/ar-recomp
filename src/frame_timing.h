#ifndef FRAME_TIMING_H
#define FRAME_TIMING_H

/* Presentation observers intentionally do not replay an unbounded backlog
 * after a stall, settings edit, or discontinuity. Keep the emulator-frame and
 * action-gameplay clocks on one bounded-delta policy. */
enum {
  kFrameTimingMaximumElapsedTicks = 8,
};

#endif  /* FRAME_TIMING_H */
