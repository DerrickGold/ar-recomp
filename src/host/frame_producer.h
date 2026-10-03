#ifndef AR_FRAME_PRODUCER_H
#define AR_FRAME_PRODUCER_H

#include <stdbool.h>
#include <stdint.h>

/* Persistent game-state owner. Submit transfers the job and every runner
 * borrow to the worker until Poll succeeds or Wait returns. During that time
 * main may consume independently owned frame packets and publish atomic input,
 * but must not access the runner, mutate settings, reset or destroy resources.
 * Jobs cannot queue or overwrite an unconsumed completion. All methods are
 * main-thread only; work and cleanup run on the same persistent worker. */
typedef void (*HostFrameProducerWork)(void *context);
bool HostFrameProducer_Init(HostFrameProducerWork cleanup, void *context);
bool HostFrameProducer_Enabled(void);
bool HostFrameProducer_Submit(HostFrameProducerWork work, void *context);
bool HostFrameProducer_Poll(void);
void HostFrameProducer_Wait(void);
void HostFrameProducer_Shutdown(void);
/* Diagnostic CPU clock of the calling thread, not elapsed wall time. Zero
 * means unavailable. Call on the producer around measured work only. */
uint64_t HostFrameProducer_ThreadCpuTimeNs(void);

#endif
