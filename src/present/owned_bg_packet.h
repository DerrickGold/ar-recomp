#ifndef AR_OWNED_BG_PACKET_H
#define AR_OWNED_BG_PACKET_H

#include <stdatomic.h>
#include <stdlib.h>
#include "snesrecomp/runner/ppu.h"

typedef struct OwnedBgPacket {
  atomic_uint references;
  SrPpuBgPacket packet;
} OwnedBgPacket;

static inline void OwnedBgPacket_Retain(OwnedBgPacket *owned) {
  if (owned) atomic_fetch_add_explicit(&owned->references, 1, memory_order_relaxed);
}

static inline void OwnedBgPacket_Release(OwnedBgPacket *owned) {
  if (owned && atomic_fetch_sub_explicit(&owned->references, 1, memory_order_acq_rel) == 1)
    free(owned);
}

#endif
