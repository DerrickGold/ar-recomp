#include "diorama/diorama_capture_hdma.h"

static void TestIndependentSubscreenHdma(void) {
  for(unsigned mode=0;mode<8;++mode) {
    /* The supported action-room rasters modify scroll or mosaic, not math. */
    CHECK(DioramaCaptureHdma_ChannelSafe(0x0f,mode));
    CHECK(DioramaCaptureHdma_ChannelSafe(0x06,mode));
    CHECK(!DioramaCaptureHdma_ChannelSafe(0x00,mode));
    CHECK(!DioramaCaptureHdma_ChannelSafe(0x05,mode));
    for(unsigned target=0x23;target<=0x33;++target)
      CHECK(!DioramaCaptureHdma_ChannelSafe((uint8_t)target,mode));
  }
  /* Mode0/2/6 repeat the same register. Modes1/3/5/7 also touch base+1. */
  const uint8_t same_register[]={0,2,6},paired_registers[]={1,3,5,7};
  for(unsigned i=0;i<sizeof(same_register);++i) {
    const uint8_t mode=same_register[i];
    CHECK(DioramaCaptureHdma_ChannelSafe(0x22,mode));
    CHECK(DioramaCaptureHdma_ChannelSafe(0x04,mode));
    CHECK(DioramaCaptureHdma_ChannelSafe(0xff,mode));
  }
  for(unsigned i=0;i<sizeof(paired_registers);++i) {
    const uint8_t mode=paired_registers[i];
    CHECK(DioramaCaptureHdma_ChannelSafe(0x21,mode)); /* CGADD+CGDATA only. */
    CHECK(!DioramaCaptureHdma_ChannelSafe(0x22,mode)); /* Reaches W12SEL. */
    CHECK(!DioramaCaptureHdma_ChannelSafe(0x04,mode)); /* Reaches BGMODE. */
    CHECK(!DioramaCaptureHdma_ChannelSafe(0xff,mode)); /* Wraps to INIDISP. */
  }
  /* Mode4 has four distinct destinations; test hazards outside its base. */
  CHECK(DioramaCaptureHdma_ChannelSafe(0x1f,4));
  CHECK(!DioramaCaptureHdma_ChannelSafe(0x20,4)); /* Fourth byte reaches$23. */
  CHECK(!DioramaCaptureHdma_ChannelSafe(0x21,4));
  CHECK(!DioramaCaptureHdma_ChannelSafe(0x02,4)); /* Fourth byte reaches$05. */
  CHECK(DioramaCaptureHdma_ChannelSafe(0xfc,4));
  CHECK(!DioramaCaptureHdma_ChannelSafe(0xfd,4)); /* Fourth byte wraps to$00. */
  CHECK(!DioramaCaptureHdma_ChannelSafe(0xfe,4));
  CHECK(!DioramaCaptureHdma_ChannelSafe(0xff,4));
  CHECK(!DioramaCaptureHdma_ChannelSafe(0x0f,8));
  CHECK(!DioramaCaptureHdma_ChannelSafe(0x0f,255));
}
