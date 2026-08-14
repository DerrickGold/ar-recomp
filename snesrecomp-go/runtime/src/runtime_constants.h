#ifndef SNESRECOMP_RUNTIME_CONSTANTS_H
#define SNESRECOMP_RUNTIME_CONSTANTS_H

/* Cross-cutting runtime constants only. This header deliberately owns no
 * functions, types, globals, or game-specific policy. */

/* The SNES exposes two contiguous 64-KiB WRAM banks ($7E/$7F). */
#define kSnesLowWramBank 0x7E
#define kSnesHighWramBank 0x7F
#define kSnesWramSize 0x20000
#define kSnesWramMask (kSnesWramSize - 1)

/* Stack bytes consumed when an HLE replaces the matching return opcode. */
#define k65816RtsStackBytes 2
#define k65816RtlStackBytes 3

/* Optional diagnostic block history. Keep the mask derived so ring storage
 * and every wrapped index cannot silently diverge. */
#define kRuntimeBlockTraceRingCapacity 1024
#define kRuntimeBlockTraceRingMask (kRuntimeBlockTraceRingCapacity - 1)

#endif  /* SNESRECOMP_RUNTIME_CONSTANTS_H */
