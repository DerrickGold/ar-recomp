#include "actraiser/actraiser_town_lair_bits.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpu_65816_math.h"

enum {
  kCurrentTownIndex = 0x7BFB,
  kTownLairByteScratch = 0x914F,
  kTownGlobalFlagsBase = 0x90FF,
  kTownLairBitMaskTableBank = 0x03,
  kTownLairBitMaskTableAddress = 0xF4D7,
  kBitsPerMaskByte = CHAR_BIT,
  kBitIndexMask = kBitsPerMaskByte - 1,
  kByteSignBit = 1u << (CHAR_BIT - 1),
  kAccumulatorHighByteMask = UINT16_MAX ^ UINT8_MAX,
  kWordBits = sizeof(uint16_t) * CHAR_BIT,
  kWordSignBit = 1u << (kWordBits - 1),

  kTownLairTestResolverReturnAddress = 0xF472,
  kTownLairSetResolverReturnAddress = 0xF47D,
  kTownLairClearResolverReturnAddress = 0xF48B,
  kTownGlobalTestResolverReturnAddress = 0xF4E3,
  kTownGlobalSetResolverReturnAddress = 0xF4EE,
  kTownGlobalClearResolverReturnAddress = 0xF4FC,
};

typedef enum TownBitAddressKind {
  kTownBitAddress_PerTownPointerTable = 0,
  kTownBitAddress_FixedGlobalFlags,
} TownBitAddressKind;

typedef enum TownBitOperation {
  kTownBitOperation_Test = 0,
  kTownBitOperation_Set,
  kTownBitOperation_Clear,
} TownBitOperation;

typedef struct TownBitResolution {
  uint16_t byte_address;
  uint16_t bit_index;
  uint16_t mask_table_word;
  uint8_t mask;
  uint8_t byte_value;
  bool address_overflow;
} TownBitResolution;

static void RequireTownBitEntryMode(CpuState *cpu,
                                    const char *routine_name) {
  if (cpu->m_flag && !cpu->x_flag && !cpu->emulation) return;
  fprintf(stderr,
          "FATAL: %s HLE requires native mode with 8-bit A and "
          "16-bit X/Y\n",
          routine_name);
  abort();
}

static void PushCpuWord(CpuState *cpu, uint16_t value) {
  cpu_write8(cpu, k65816StackBank, cpu->S,
             (uint8_t)(value >> CHAR_BIT));
  cpu->S = (uint16_t)(cpu->S - 1);
  cpu_write8(cpu, k65816StackBank, cpu->S, (uint8_t)value);
  cpu->S = (uint16_t)(cpu->S - 1);
}

static uint16_t PopCpuWord(CpuState *cpu) {
  cpu->S = (uint16_t)(cpu->S + 1);
  const uint16_t value = cpu_read16(cpu, k65816StackBank, cpu->S);
  cpu->S = (uint16_t)(cpu->S + 1);
  return value;
}

static void SetCpuByteNegativeZero(CpuState *cpu, uint8_t value) {
  cpu->_flag_Z = value == 0;
  cpu->_flag_N = (value & kByteSignBit) != 0;
  cpu->P = (uint8_t)((cpu->P & ~(CPU_P_N | CPU_P_Z)) |
                     (cpu->_flag_N ? CPU_P_N : 0) |
                     (cpu->_flag_Z ? CPU_P_Z : 0));
}

static void SetCpuWordNegativeZero(CpuState *cpu, uint16_t value) {
  cpu->_flag_Z = value == 0;
  cpu->_flag_N = (value & kWordSignBit) != 0;
  cpu->P = (uint8_t)((cpu->P & ~(CPU_P_N | CPU_P_Z)) |
                     (cpu->_flag_N ? CPU_P_N : 0) |
                     (cpu->_flag_Z ? CPU_P_Z : 0));
}

static void WriteAccumulatorLowByte(CpuState *cpu, uint8_t value) {
  cpu->A = (uint16_t)((cpu->A & kAccumulatorHighByteMask) | value);
}

/* $03:F497 and $03:F508 differ only in how they obtain the mask's base
 * address and in their final register arrangement. This model deliberately
 * retains the ROM's stack traffic: several unrelated routines inspect stack
 * RAM while debugging, and exact replays compare the full WRAM image. */
static void ResolveTownBit(CpuState *cpu,
                           TownBitAddressKind address_kind) {
  const uint16_t entry_accumulator = cpu->A;
  const bool decimal = cpu->_flag_D != 0;
  uint16_t mask_base;

  if (address_kind == kTownBitAddress_PerTownPointerTable) {
    PushCpuWord(cpu, entry_accumulator);
    const uint16_t encoded_town_index =
        cpu_read16(cpu, cpu->DB, kCurrentTownIndex);
    PushCpuWord(cpu, cpu->Y);
    const Cpu65816Add16Result pointer_address = Cpu65816_Add16(
        encoded_town_index, cpu->Y, false, decimal);
    cpu->X = pointer_address.value;
    mask_base = cpu_read16(cpu, kTownLairBitMaskTableBank,
                           pointer_address.value);
    cpu->Y = mask_base;
    cpu->A = PopCpuWord(cpu);
    cpu->A = PopCpuWord(cpu);
  } else {
    mask_base = kTownGlobalFlagsBase;
    cpu->Y = mask_base;
  }

  const uint16_t flag_id = entry_accumulator & UINT8_MAX;
  cpu->A = flag_id;
  PushCpuWord(cpu, flag_id);
  const uint16_t byte_offset = flag_id / kBitsPerMaskByte;
  cpu->A = byte_offset;
  PushCpuWord(cpu, mask_base);
  const Cpu65816Add16Result address = Cpu65816_Add16(
      byte_offset, mask_base, false, decimal);
  cpu->X = PopCpuWord(cpu);
  cpu->A = address.value;
  cpu->Y = address.value;

  TownBitResolution resolution;
  resolution.byte_address = address.value;
  resolution.byte_value = cpu_read8(cpu, cpu->DB, address.value);
  cpu_write8(cpu, cpu->DB, kTownLairByteScratch,
             resolution.byte_value);
  cpu->A = PopCpuWord(cpu);
  resolution.bit_index = flag_id & kBitIndexMask;
  cpu->A = resolution.bit_index;
  cpu->X = resolution.bit_index;
  resolution.mask_table_word = cpu_read16(
      cpu, kTownLairBitMaskTableBank,
      (uint16_t)(kTownLairBitMaskTableAddress + resolution.bit_index));
  resolution.mask = (uint8_t)resolution.mask_table_word;
  resolution.address_overflow = address.overflow;

  if (address_kind == kTownBitAddress_PerTownPointerTable) {
    cpu->A = resolution.mask;
    PushCpuWord(cpu, resolution.bit_index);
    cpu->X = resolution.byte_address;
    cpu->Y = PopCpuWord(cpu);
  } else {
    cpu->A = resolution.mask_table_word;
    cpu->X = resolution.bit_index;
    cpu->Y = resolution.byte_address;
  }

  const bool final_zero =
      address_kind == kTownBitAddress_PerTownPointerTable
          ? resolution.bit_index == 0
          : resolution.mask_table_word == 0;
  const bool final_negative =
      address_kind == kTownBitAddress_FixedGlobalFlags &&
      (resolution.mask_table_word & kWordSignBit) != 0;
  cpu->P = (uint8_t)((cpu->P & (CPU_P_I | CPU_P_D | CPU_P_X)) |
                     CPU_P_M |
                     (final_zero ? CPU_P_Z : 0) |
                     (resolution.address_overflow ? CPU_P_V : 0) |
                     (final_negative ? CPU_P_N : 0));
  cpu_p_to_mirrors(cpu);
}

static RecompReturn ResolveTownBitHle(CpuState *cpu,
                                      TownBitAddressKind address_kind,
                                      const char *routine_name) {
  if (!cpu) return RECOMP_RETURN_NORMAL;
  cpu_mirrors_to_p(cpu);
  RequireTownBitEntryMode(cpu, routine_name);
  ResolveTownBit(cpu, address_kind);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_TownLairMaskResolveBit(CpuState *cpu) {
  return ResolveTownBitHle(cpu, kTownBitAddress_PerTownPointerTable,
                           "$03:F497");
}

RecompReturn ActRaiser_TownGlobalFlagResolveBit(CpuState *cpu) {
  return ResolveTownBitHle(cpu, kTownBitAddress_FixedGlobalFlags,
                           "$03:F508");
}

static RecompReturn ApplyTownBitOperation(
    CpuState *cpu, TownBitAddressKind address_kind,
    TownBitOperation operation, uint16_t resolver_return_address,
    const char *routine_name) {
  if (!cpu) return RECOMP_RETURN_NORMAL;
  cpu_mirrors_to_p(cpu);
  RequireTownBitEntryMode(cpu, routine_name);

  const uint16_t saved_x = cpu->X;
  const uint16_t saved_y = cpu->Y;
  PushCpuWord(cpu, saved_x);
  PushCpuWord(cpu, saved_y);
  PushCpuWord(cpu, resolver_return_address);
  cpu->host_return_valid = 1;

  uint16_t byte_address;
  RecompReturn nested_result;
  if (address_kind == kTownBitAddress_PerTownPointerTable) {
    nested_result = ActRaiser_TownLairMaskResolveBit(cpu);
    byte_address = cpu->X;
  } else {
    nested_result = ActRaiser_TownGlobalFlagResolveBit(cpu);
    byte_address = cpu->Y;
  }
  if (nested_result != RECOMP_RETURN_NORMAL) return nested_result;
  const uint8_t mask = (uint8_t)cpu->A;
  const uint8_t byte_value = cpu_read8(
      cpu, cpu->DB, kTownLairByteScratch);

  uint8_t result;
  switch (operation) {
    case kTownBitOperation_Test:
      cpu->Y = PopCpuWord(cpu);
      cpu->X = PopCpuWord(cpu);
      result = mask & byte_value;
      WriteAccumulatorLowByte(cpu, result);
      SetCpuByteNegativeZero(cpu, result);
      break;

    case kTownBitOperation_Set:
      result = mask | byte_value;
      WriteAccumulatorLowByte(cpu, result);
      SetCpuByteNegativeZero(cpu, result);
      cpu_write8(cpu, cpu->DB, byte_address, result);
      cpu->Y = PopCpuWord(cpu);
      cpu->X = PopCpuWord(cpu);
      SetCpuWordNegativeZero(cpu, cpu->X);
      break;

    case kTownBitOperation_Clear:
      result = (uint8_t)(~mask & byte_value);
      WriteAccumulatorLowByte(cpu, result);
      SetCpuByteNegativeZero(cpu, result);
      cpu_write8(cpu, cpu->DB, byte_address, result);
      cpu->Y = PopCpuWord(cpu);
      cpu->X = PopCpuWord(cpu);
      SetCpuWordNegativeZero(cpu, cpu->X);
      break;
  }

  cpu_p_to_mirrors(cpu);
  cpu->S = (uint16_t)(cpu->S + k65816RtsStackBytes);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiser_TownLairMaskTest(CpuState *cpu) {
  return ApplyTownBitOperation(
      cpu, kTownBitAddress_PerTownPointerTable, kTownBitOperation_Test,
      kTownLairTestResolverReturnAddress, "$03:F46E");
}

RecompReturn ActRaiser_TownLairMaskSet(CpuState *cpu) {
  return ApplyTownBitOperation(
      cpu, kTownBitAddress_PerTownPointerTable, kTownBitOperation_Set,
      kTownLairSetResolverReturnAddress, "$03:F479");
}

RecompReturn ActRaiser_TownLairMaskClear(CpuState *cpu) {
  return ApplyTownBitOperation(
      cpu, kTownBitAddress_PerTownPointerTable, kTownBitOperation_Clear,
      kTownLairClearResolverReturnAddress, "$03:F487");
}

RecompReturn ActRaiser_TownGlobalFlagTest(CpuState *cpu) {
  return ApplyTownBitOperation(
      cpu, kTownBitAddress_FixedGlobalFlags, kTownBitOperation_Test,
      kTownGlobalTestResolverReturnAddress, "$03:F4DF");
}

RecompReturn ActRaiser_TownGlobalFlagSet(CpuState *cpu) {
  return ApplyTownBitOperation(
      cpu, kTownBitAddress_FixedGlobalFlags, kTownBitOperation_Set,
      kTownGlobalSetResolverReturnAddress, "$03:F4EA");
}

RecompReturn ActRaiser_TownGlobalFlagClear(CpuState *cpu) {
  return ApplyTownBitOperation(
      cpu, kTownBitAddress_FixedGlobalFlags, kTownBitOperation_Clear,
      kTownGlobalClearResolverReturnAddress, "$03:F4F8");
}
