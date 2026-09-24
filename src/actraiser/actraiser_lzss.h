#ifndef ACTRAISER_LZSS_H
#define ACTRAISER_LZSS_H

#include "snesrecomp/game/cpu.h"
#include <stddef.h>

/* Game-thread load-time observation for derived asset residency. The source
 * is the SNES address of the raw bitstream (after its size header), not a
 * host pointer. Bytes are immutable and borrowed only for the call. No CPU
 * is exposed; observers must not re-enter the decoder or mutate native state.
 * The boot owner registers one observer, or NULL to detach before shutdown. */
typedef void (*ActRaiserLzssObserver)(void *context,uint32_t source,
    uint16_t destination,const uint8_t *bytes,size_t size);
void ActRaiserLzss_SetObserver(ActRaiserLzssObserver observer,void *context);

/* Whole-body HLE for the stock Quintet LZSS driver at $02:C5C9. */
RecompReturn ActRaiser_LzssDecompress(CpuState *cpu);

#endif /* ACTRAISER_LZSS_H */
