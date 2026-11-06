/* Compile the same implementation with its capability fallback forced only
 * in this test translation unit. No production runtime override is exposed. */
#define AR_DIORAMA_ALPHA_TEST_FORCE_RGBA32 1
#define ArGpuDioramaDepthAlpha_Destroy TestDioramaDepthAlphaLegacy_Destroy
#define ArGpuDioramaDepthPass_PrepareHybrid TestDioramaDepthAlphaLegacy_Prepare
#define ArGpuDioramaDepthPass_EncodeHybridMaterials TestDioramaDepthAlphaLegacy_Encode
#include "../src/platform/sdl/diorama_depth_alpha_sdl.c"
