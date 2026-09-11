#ifndef AR_HOST_PARALLEL_WORK_H
#define AR_HOST_PARALLEL_WORK_H

#include <stddef.h>

typedef struct HostParallelWork HostParallelWork;
typedef void (*HostParallelWorkRange)(void *context, size_t first, size_t end);

/* Caller-owned, synchronous fork/join for independent CPU array work. All
 * control calls belong to one owner thread. Each index runs exactly once;
 * callbacks receive disjoint ranges and finish before Run returns. Inputs
 * must remain immutable until that join, and output ranges must not overlap.
 * Callbacks must not access live game state, shared caches, render resources,
 * profiling globals, or re-enter this group. There is no detached work.
 *
 * Create caps helpers to available cores (and a small implementation limit).
 * Zero helpers or any setup failure returns NULL. Run(NULL, ...) executes
 * serially with the identical callback. Idle helpers sleep; Run allocates
 * nothing. AR_RENDER_WORKERS=0..3 is a startup diagnostic override, bounded by
 * the caller's limit; it is not a graphics-quality or game-state setting. */
HostParallelWork *HostParallelWork_Create(unsigned maximum_helpers);
void HostParallelWork_Run(HostParallelWork *work, size_t count,
    size_t minimum_per_part, HostParallelWorkRange range, void *context);
void HostParallelWork_Destroy(HostParallelWork *work);

#endif
