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
 * Slow builds and failures are printed immediately. Preparation and gameplay
 * have separate summaries. AR_GPU_COMPILE_TRACE=1 also prints fast late builds. */
enum { kGpuCompileLogThresholdMs = 5 };

void GpuCompileLog_Record(const char *label, const char *stage, uint64_t elapsed_ns,
                          bool succeeded);
/* Brackets RenderPreparation_Prepare; End prints the summary. The optional
 * observer runs after every build recorded during preparation, which is the
 * only point a long preparation can show that it is still working. */
typedef void (*GpuCompileLogObserver)(void *context);
void GpuCompileLog_BeginPreparation(GpuCompileLogObserver observer, void *context);
void GpuCompileLog_EndPreparation(void);
/* Count all late builds, including successes too fast to log individually. */
unsigned GpuCompileLog_LateBuildCount(void);
/* Report and reset the late-build totals at session shutdown. */
void GpuCompileLog_ReportLate(void);

#endif /* AR_GPU_COMPILE_LOG_H */
