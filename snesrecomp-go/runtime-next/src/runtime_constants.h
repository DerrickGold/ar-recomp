#ifndef SNESRECOMP_NEXT_RUNTIME_CONSTANTS_H
#define SNESRECOMP_NEXT_RUNTIME_CONSTANTS_H

/* Hardware and runtime-wide constants shared with generated game code. */
#define kSnesLowWramBank 0x7E
#define kSnesHighWramBank 0x7F
#define kSnesWramSize 0x20000
#define kSnesWramMask (kSnesWramSize - 1)

#define k65816StackBank 0x00
#define k65816RtsStackBytes 2
#define k65816RtlStackBytes 3

#define kRuntimeBlockTraceRingCapacity 1024
#define kRuntimeBlockTraceRingMask (kRuntimeBlockTraceRingCapacity - 1)

#endif
