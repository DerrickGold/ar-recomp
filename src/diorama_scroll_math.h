#ifndef DIORAMA_SCROLL_MATH_H
#define DIORAMA_SCROLL_MATH_H
#include "present.h"   /* FrameSlot, DioramaScrollSnapshot, DioramaScrollDelta */
/* Pure interpolation math with the wall-clock injected, for testability. */
DioramaScrollDelta ComputeDioramaScrollDeltaAt(
    const FrameSlot *curr, const DioramaScrollSnapshot *prev, uint64_t now_ns);
#endif
