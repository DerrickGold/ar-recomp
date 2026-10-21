#ifndef AR_GPU_PROTOTYPE_POLICY_H
#define AR_GPU_PROTOTYPE_POLICY_H
/* Default policy for the exact GPU shortcuts: the camera-guided search bound,
 * effect command batching and stationary actor rejection. Each produces pixels
 * identical to its reference path. The October 2026 evidence (1,152 CPU-oracle
 * checks, 134/134 whole-game frames, 50 minutes of 90 Hz Deck feedback) covers
 * Metal and Vulkan only, so D3D12 keeps the reference path until a Windows
 * runtime comparison exists.
 *   unset, empty or "auto": on for Metal and Vulkan, reference elsewhere
 *   "1": on for the running backend
 *   "0" or any other value: reference path
 * Phase: pure. Tests: tests/gpu_prototype_policy_test.c */
#include <SDL3/SDL.h>
#include <stdbool.h>

static inline bool ArGpuPrototype_ValidatedBackend(const char *driver) {
  return driver && (SDL_strcmp(driver, "metal") == 0 || SDL_strcmp(driver, "vulkan") == 0);
}

static inline bool ArGpuPrototype_Resolve(const char *driver, const char *value) {
  if (!value || !*value || SDL_strcmp(value, "auto") == 0)
    return ArGpuPrototype_ValidatedBackend(driver);
  return SDL_strcmp(value, "1") == 0;
}

static inline bool ArGpuPrototype_Enabled(SDL_GPUDevice *device, const char *variable) {
  return ArGpuPrototype_Resolve(device ? SDL_GetGPUDeviceDriver(device) : NULL,
                                SDL_getenv(variable));
}
#endif /* AR_GPU_PROTOTYPE_POLICY_H */
