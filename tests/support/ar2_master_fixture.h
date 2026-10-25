#ifndef AR_AR2_MASTER_FIXTURE_H
#define AR_AR2_MASTER_FIXTURE_H
/* Synthetic character packs shared by asset validation and runtime tests. */
#include "actraiser/actraiser_ar2_master_assets.h"
#include "byte_order.h"
#include "snesrecomp/support/digest.h"
#include "support/test_assert.h"
#include <string.h>

enum { kAr2MasterFixtureBytes = 16384 };

/* One shared pose, or two distinct one-tile poses per animation. The latter
 * marks each pose with its frame number in the part's x offset. No ROM data. */
static size_t Ar2MasterFixture(uint8_t data[kAr2MasterFixtureBytes], bool distinct_poses) {
  memset(data, 0, kAr2MasterFixtureBytes);
  memcpy(data, "AR2MASTR", 8);
  ByteOrder_WriteLe32(data + 8, 1);
  ByteOrder_WriteLe16(data + 50, 0x1234);
  size_t at = 80 + kAr2MasterMotionBytes;
  const unsigned frames = distinct_poses ? kAr2MasterAnimCount * 2 : 1;
  ByteOrder_WriteLe16(data + at, (uint16_t)frames);
  at += 2;
  for (unsigned i = 0; i < kAr2MasterAnimCount; i++) {
    /* Air-slash control resumes fall at step 2 after its four swing steps. */
    const unsigned steps = distinct_poses ? (i == 0x1B ? 4 : i == 0x10 ? 3 : 2) : 1;
    data[at++] = (uint8_t)steps;
    for (unsigned step = 0; step < steps; step++) {
      ByteOrder_WriteLe16(data + at, distinct_poses ? (i == 0x10 && step == 2 ? 254 : 2) : 8);
      data[at + 2] = distinct_poses ? (uint8_t)(i * 2 + step % 2) : 0;
      at += 3;
    }
  }
  for (unsigned frame = 0; frame < frames; frame++) {
    data[at] = data[at + 1] = 8;
    data[at + 2] = 48;
    data[at + 6] = 8;
    data[at + 7] = 48;
    if (distinct_poses) {
      ByteOrder_WriteLe16(data + at + 8, 8);
      ByteOrder_WriteLe16(data + at + 10, (uint16_t)-24);
      data[at + 12] = 24;
      data[at + 13] = 32;
    }
    data[at + 14] = 1;
    data[at + 15 + 1] = data[at + 15 + 2] = (uint8_t)frame;
    at += 15 + 7;
    ByteOrder_WriteLe16(data + at, 32);
    at += 2;
    memset(data + at, 0xA5, 32);
    at += 32;
  }
  ByteOrder_WriteLe32(data + 12, (uint32_t)(at - 48));
  assert(sr_support_sha256(data + 48, at - 48, data + 16));
  return at;
}
#endif
