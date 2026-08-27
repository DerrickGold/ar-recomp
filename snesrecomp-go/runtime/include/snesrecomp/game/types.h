#ifndef SNESRECOMP_GAME_TYPES_H
#define SNESRECOMP_GAME_TYPES_H

#include "snesrecomp/game/runtime_constants.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _MSC_VER
#define countof(value) _countof(value)
#define NORETURN __declspec(noreturn)
#define SNES_FORCEINLINE __forceinline
#define NOINLINE __declspec(noinline)
#else
#define countof(value) (sizeof(value) / sizeof((value)[0]))
#define NORETURN _Noreturn
#define SNES_FORCEINLINE inline
#define NOINLINE __attribute__((noinline))
#endif

#define arraysize(value) (sizeof(value) / sizeof((value)[0]))

#ifdef _DEBUG
#define kDebugFlag 1
#else
#define kDebugFlag 0
#endif

typedef uint8_t uint8;
typedef int8_t int8;
typedef uint16_t uint16;
typedef int16_t int16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;
typedef unsigned int uint;
typedef uint16 VoidP;

static SNES_FORCEINLINE int IntMin(int left, int right) {
    return left < right ? left : right;
}

static SNES_FORCEINLINE int IntMax(int left, int right) {
    return left > right ? left : right;
}

static SNES_FORCEINLINE uint16 swap16(uint16 value) {
    return (uint16)((value << 8) | (value >> 8));
}

void NORETURN Die(const char *error);

#pragma pack(push, 1)
typedef struct LongPtr {
    VoidP addr;
    uint8 bank;
} LongPtr;
#pragma pack(pop)

typedef struct PairU16 {
    uint16 first;
    uint16 second;
} PairU16;

typedef struct RetAY {
    uint8 a;
    uint8 y;
} RetAY;

typedef struct RetY {
    uint8 y;
} RetY;

typedef struct RetAXY {
    uint8 a;
    uint8 x;
    uint8 y;
} RetAXY;

typedef struct PointU16 {
    uint16 x;
    uint16 y;
} PointU16;

typedef struct PointU8 {
    uint8 x;
    uint8 y;
} PointU8;

typedef struct OamEnt {
    uint8 xpos;
    uint8 ypos;
    uint8 charnum;
    uint8 flags;
} OamEnt;

typedef void FuncV(void);
typedef void FuncU8(uint8 kk);
typedef void FuncU8J(uint8 kk, uint8 jj);
typedef void FuncU8A(uint8 kk, uint8 aa);
typedef void FuncU8JA(uint8 kk, uint8 jj, uint8 aa);

#ifdef HIBYTE
#undef HIBYTE
#endif
#define BYTEn(value, index) (*((uint8 *)&(value) + (index)))
#define HIBYTE(value) BYTEn(value, 1)
#define PAIR16(high, low) ((uint16)(((uint16)(high) << 8) | (uint8)(low)))

static SNES_FORCEINLINE PairU16 MakePairU16(uint16 first, uint16 second) {
    PairU16 result = {first, second};
    return result;
}

typedef struct MemBlk {
    const uint8 *ptr;
    size_t size;
} MemBlk;

MemBlk FindIndexInMemblk(MemBlk data, size_t index);
const uint8 *FindAddrInMemblk(MemBlk data, uint32 addr);

#endif
