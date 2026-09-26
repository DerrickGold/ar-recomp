#ifndef AR_HOST_PPU_OUTPUT_H
#define AR_HOST_PPU_OUTPUT_H
/* Runner output bindings and demand for native comparison frames. Storage
 * lives in host_frame_surfaces.c; HD-specific surfaces remain with replacements.
 * Main-thread binding and completed-frame tracking share this owner. */
#include <stdbool.h>
#include "snesrecomp/runner.h"

/* One runner lifetime snapshot shared by a batch of output bindings. Feature
 * adapters may bind their own surfaces through this checked ABI contract. */
typedef struct HostPpuOutputControl {
  const SnesRunnerApi *api;
  SrRunnerHandle *runner;
  uint64_t lifetime_generation;
} HostPpuOutputControl;
bool HostPpuOutputControl_Begin(HostPpuOutputControl *control);
SrResult HostPpuOutputControl_Bind(
    const HostPpuOutputControl *control, SrPpuOutputKind kind,
    uint32_t source, uint32_t band, uint32_t scale, uint8_t *pixels,
    uint64_t pixel_byte_size, uint64_t pitch_bytes, uint32_t height_pixels,
    uint32_t flags);

/* Rebind common and feature surfaces after a geometry change. Completed
 * authentic frames are invalid until a full pass reaches the new geometry. */
void HostPpuOutput_Rebind(void);
void HostPpuOutput_SetAuthenticEnabled(bool enabled);
bool HostPpuOutput_AuthenticEnabled(void);
void HostPpuOutput_AuthenticFrameCompleted(bool frame_valid);
uint64_t HostPpuOutput_AuthenticFrameSerial(void);
/* Reset demand and serials at session shutdown, after rendering has stopped. */
void HostPpuOutput_Reset(void);
#endif
