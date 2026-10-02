#ifndef AR_FRAME_QUEUE_H
#define AR_FRAME_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include "present/present.h"

/* Bounded SPSC handoff. A packet owns its pixel copies until the reader
 * releases it. Unlike FrameSlot alone, it may outlive a runner tick. Only the
 * separated action planes/HUD/skybox are supported; no SIM or authentic views. */
typedef struct HostFramePacket {
  FrameSlot frame;
  uint64_t source_ns, input_ns, started_ns, completed_ns, copy_ns;
  uint64_t copied_bytes;
  int tick;
  uint8_t *pixels;
  size_t capacity;
} HostFramePacket;
typedef struct HostFrameQueue HostFrameQueue;
/* Shared eligibility for retention and packet capture. Unsupported scenes
 * return to synchronous rendering without changing their visual settings. */
static inline bool HostFramePacket_Supports(const FrameSlot *frame) {
  return frame && frame->diorama_active && !frame->scene_inspector_enabled &&
      !frame->m7_active && frame->bg_mode != 7 && frame->sim.view == kSimView_None;
}
HostFrameQueue *HostFrameQueue_Create(void);
void HostFrameQueue_Destroy(HostFrameQueue *queue); /* Both owners stopped. */
HostFramePacket *HostFrameQueue_BeginWrite(HostFrameQueue *queue);
bool HostFramePacket_OwnPixels(HostFramePacket *packet);
void HostFrameQueue_Publish(HostFrameQueue *queue);
const HostFramePacket *HostFrameQueue_Read(HostFrameQueue *queue);
/* Reader-owned snapshot: bound one drain even if production continues. */
unsigned HostFrameQueue_ReadyCount(const HostFrameQueue *queue);
void HostFrameQueue_Release(HostFrameQueue *queue);
void HostFrameQueue_RequestPause(HostFrameQueue *queue);
bool HostFrameQueue_PauseRequested(const HostFrameQueue *queue);
void HostFrameQueue_Resume(HostFrameQueue *queue); /* Producer acknowledged stop. */

#endif
