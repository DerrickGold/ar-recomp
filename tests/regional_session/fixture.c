#include "support/regional_test_values.h"
#include "regional_session_test.h"

#include <string.h>

void RegionalSessionTest_MakeImage(uint8_t *image, unsigned marker) {
  memset(image, 0, kActRaiserSramSize);
  image[0x100] = (uint8_t)marker;
  memcpy(image + 0x1439, "MASTER", 6);
  Save_RecomputeChecksum(image);
}

bool RegionalSessionTest_Equal(const ArRegionalSession *a, const ArRegionalSession *b) {
  return TestRegional_EqualSession(a, b);
}

SaveCheckpointStatus RegionalSessionTest_AcceptOpaque(const uint8_t *bytes, size_t size,
                                                      void *context) {
  (void)context;
  return bytes && size ? kSaveCheckpoint_Ready : kSaveCheckpoint_Invalid;
}
