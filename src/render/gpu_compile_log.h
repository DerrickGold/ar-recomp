#ifndef AR_GPU_COMPILE_LOG_H
#define AR_GPU_COMPILE_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* Shader and pipeline creation is where GPU drivers compile: Metal when the
 * shader is created from source, Vulkan and D3D12 when the pipeline is. One
 * slow or failing compile on the frame that first needs it shows up only as an
 * unexplained stall (an Adreno driver spent 11 s on one compute pipeline), so
 * game-owned creations are reported here, from any backend. SDL also owns
 * internal drawing/blit pipelines: their preparation is reported as a group;
 * native API tracing is needed to count those individual creations.
 *
 * During boot preparation only builds of kGpuCompileLogBootThresholdMs or
 * more are printed, followed by one summary line; drivers that cache (Metal)
 * then stay quiet. Any build after preparation is printed as "late": it ran
 * during play and is a candidate for moving into preparation. */
enum { kGpuCompileLogBootThresholdMs = 5 };

void GpuCompileLog_Record(const char *label, const char *stage, uint64_t elapsed_ns,
                          bool succeeded);
/* Brackets RenderPreparation_Prepare; End prints the summary. The optional
 * observer runs after every build recorded during preparation, which is the
 * only point a long preparation can show that it is still working. */
typedef void (*GpuCompileLogObserver)(void *context);
void GpuCompileLog_BeginPreparation(GpuCompileLogObserver observer, void *context);
void GpuCompileLog_EndPreparation(void);

#endif /* AR_GPU_COMPILE_LOG_H */
