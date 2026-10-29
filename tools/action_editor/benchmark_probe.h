#ifndef AR_ROOM_BENCHMARK_PROBE_H
#define AR_ROOM_BENCHMARK_PROBE_H

#include <limits.h>

/* Optional diagnostic-library ABI, not a production renderer dependency.
 * Negative frames are warmup; nonnegative frames are measured. Close only
 * after the last GPU idle/collection, before capture readback or teardown. */
enum { kArRoomBenchmarkProbeClosed = INT_MIN };
typedef void (*ArRoomBenchmarkFrameProbe)(int frame);

#endif
