#include "diorama/diorama_gpu_policy.h"
#include "support/test_assert.h"

int main(void) {
  const char *overrides[] = {NULL, "", "auto", "1", "0", "invalid"};
  for (unsigned mode = kDioramaGpuCapture_Off; mode <= kDioramaGpuCapture_TilesValidate; ++mode)
    for (unsigned i = 0; i < sizeof(overrides)/sizeof(*overrides); ++i) {
      const uint32_t flags = DioramaGpuPolicy_CaptureFlags((DioramaGpuCaptureMode)mode, overrides[i]);
      if (mode == kDioramaGpuCapture_Owned || mode == kDioramaGpuCapture_TilesValidate) {
        assert(flags & SR_PPU_BG_PACKET_TILES);
        assert(!!(flags & SR_PPU_BG_PACKET_FULL_ADD) == (i < 4));
        assert(!!(flags & SR_PPU_BG_PACKET_VALIDATE_TILES) == (mode == kDioramaGpuCapture_TilesValidate));
      } else assert(flags == 0);
    }
  const DioramaGpuPolicy normal = DioramaGpuPolicy_Resolve(5, 1, NULL, NULL, NULL, NULL, NULL);
  assert(DioramaGpuPolicy_CaptureFlags(normal.capture, NULL) ==
      (SR_PPU_BG_PACKET_TILES | SR_PPU_BG_PACKET_FULL_ADD));
  const DioramaGpuPolicy off = DioramaGpuPolicy_Resolve(5, 1, "reference", NULL, NULL, NULL, NULL);
  assert(!DioramaGpuPolicy_CaptureFlags(off.capture, "1"));
  const DioramaGpuPolicy sim = DioramaGpuPolicy_Resolve(0, 1, NULL, NULL, NULL, NULL, NULL);
  assert(!DioramaGpuPolicy_CaptureFlags(sim.capture, NULL));
  return 0;
}
