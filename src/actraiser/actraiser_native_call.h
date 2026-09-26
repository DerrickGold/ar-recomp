#ifndef AR_ACTRAISER_NATIVE_CALL_H
#define AR_ACTRAISER_NATIVE_CALL_H
/* ActRaiserNativeCall: calls an audited generated routine from a game HLE with
 * the native call frame, including the caller's real return address.
 * Phase: game (HLE). */

#include "snesrecomp/game/cpu.h"

typedef RecompReturn (*ActRaiserNativeLeaf)(CpuState *);

/* Game HLE -> audited generated leaf. caller is the last byte of the native
 * JSR/JSL (the pushed return word), not a synthetic callback address.
 * Preserves native call-frame/return-scope ownership. Does not restore CPU
 * registers, erase escaped control flow, or decrement a SKIP token: callers
 * must stop their own body and propagate it exactly once. Normal leaves must
 * balance S/PB; each controller checks its own remaining ABI requirements. */
RecompReturn ActRaiserNativeCall(CpuState *cpu, ActRaiserNativeLeaf leaf,
    uint8_t bank, uint16_t caller, bool long_call);

#endif
