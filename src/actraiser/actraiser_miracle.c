#include "actraiser/actraiser_miracle.h"

#include "actraiser/actraiser_cpu_hle_internal.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser/actraiser_native_call.h"

#define LEAF(bank, pc) extern RecompReturn bank_##bank##_##pc##_M1X0(CpuState *)
LEAF(01, 8E29); LEAF(01, 8D92); LEAF(01, 8CB6); LEAF(01, 9754);
LEAF(03, CA5E); LEAF(01, 97E5); LEAF(01, 93B4); LEAF(01, B1FE);
LEAF(01, 9270); LEAF(01, 8CCE);
#undef LEAF

/* Native command bodies $01:8290..8490. The first three have identical
 * relative layouts; wind/quake omit targeting. Preserve exact caller PCs:
 * text routes and selector ownership use them, not just the string pointer. */
typedef struct Miracle {
  uint16_t entry, description, question, target, cancel, insufficient;
  uint8_t effect;
  ArRegionalCostRule rule;
} Miracle;
static const Miracle kMiracles[] = {
  {0x8290, 0xfc9c, 0xfd15, 0xfce8, 0xfd01, 0xfcce, 1, kArRegionalCost_Lightning},
  {0x82fb, 0xfd25, 0xfdb9, 0xfd8e, 0xfda9, 0xfd6f, 2, kArRegionalCost_Rain},
  {0x8366, 0xfedc, 0xff57, 0xff26, 0xff3f, 0xff0c, 3, kArRegionalCost_Sunlight},
  {0x8431, 0xfdc8, 0xfe2a, 0, 0xfe11, 0xfde2, 5, kArRegionalCost_Wind},
  {0x83d1, 0xfe3a, 0xfec7, 0, 0xfeb5, 0xfe6a, 4, kArRegionalCost_Earthquake},
};

bool ActRaiserMiracle_Rule(unsigned action, ArRegionalCostRule *rule) {
  if (action < 5 || action > 9 || !rule) return false;
  *rule = kMiracles[action - 5].rule;
  return true;
}

bool ActRaiserMiracle_Entry(const CpuState *cpu, unsigned action) {
  return cpu && action >= 5 && action <= 9 && cpu->PB == 1 && cpu->DB == 1 &&
      cpu->D == 0 && cpu->m_flag && !cpu->x_flag && !cpu->emulation;
}

static void Carry(CpuState *cpu, bool value) {
  cpu->_flag_C = value;
  cpu->P = (uint8_t)((cpu->P & ~1u) | value);
}
static void LoadA(CpuState *cpu, uint8_t value) {
  cpu->A = (uint16_t)((cpu->A & 0xff00u) | value);
  ActRaiserCpuHle_SetNegativeZero8(cpu, value);
}
static void LoadY(CpuState *cpu, uint16_t value) {
  cpu->Y = value;
  ActRaiserCpuHle_SetNegativeZero16(cpu, value);
}

/* Only these audited native leaves are called. Keep their real JSR/JSL
 * frames and return ownership; do not restore architectural state or discard
 * nonlocal control flow. No synthesized resource values or ROM mutation. */
static RecompReturn Call(CpuState *cpu, ActRaiserNativeLeaf leaf, uint8_t bank,
                         uint16_t caller, bool long_call) {
  RecompReturn result = ActRaiserNativeCall(cpu, leaf, bank, caller, long_call);
  if (result != RECOMP_RETURN_NORMAL) {
    /* Match the native direct caller's one-level SKIP propagation. A SKIP_1
     * leaves this activation with NORMAL; it must not resume its next step. */
    return result;
  }
  if (!cpu->m_flag || cpu->x_flag ||
      cpu->DB != 1 || cpu->D || cpu->emulation)
    ActRaiserHleFatal("Miracle leaf at $%02X:%04X returned outside its contract", bank, caller);
  return RECOMP_RETURN_NORMAL;
}

RecompReturn ActRaiserMiracle_Run(CpuState *cpu, unsigned action,
                                 const ArRegionalCostSnapshot *quote) {
  if (!ActRaiserMiracle_Entry(cpu, action) || !quote)
    ActRaiserHleFatal("Invalid miracle entry/quote");
  const Miracle *m = &kMiracles[action - 5];
  const unsigned price = quote->price[m->rule];
  if (!price || price > 255) ActRaiserHleFatal("Miracle price exceeds native debit width");
  const uint16_t base = m->entry;
  const unsigned short_layout = m->target ? 0 : 11;
  RecompReturn result;
#define CALL(leaf, bank, offset, is_long) do { \
    result = Call(cpu, leaf, bank, (uint16_t)(base + (offset)), is_long); \
    if (result != RECOMP_RETURN_NORMAL) goto nonlocal; \
  } while (0)
#define TEXT(source, offset) do { LoadY(cpu, source); \
    CALL(bank_01_8E29_M1X0, 1, offset, false); } while (0)
  /* $81D7's DEC-A chain reaches every selected body with low A=0. */
  LoadA(cpu, 0);
  TEXT(m->description, 5);
  LoadA(cpu, cpu_read8(cpu, 0x7f, 0x9217));
  if (!cpu->_flag_Z) {
    /* REP/LDA/CMP/SEP leaves the full SP value in A, including its high byte. */
    cpu->A = cpu_read16(cpu, 0, 0x0282);
    ActRaiserCpuHle_SetNegativeZero16(cpu, (uint16_t)(cpu->A - price));
    Carry(cpu, cpu->A >= price);
    if (cpu->_flag_C) {
      TEXT(m->question, 0x1e);
      CALL(bank_01_8D92_M1X0, 1, 0x21, false);
      if (!cpu->_flag_C) goto cancelled;
      if (m->target) {
        TEXT(m->target, 0x29);
        CALL(bank_01_8CB6_M1X0, 1, 0x2c, false);
        CALL(bank_01_9754_M1X0, 1, 0x2f, false);
        if (cpu->_flag_C) goto cancelled;
      } else CALL(bank_01_8CB6_M1X0, 1, 0x26, false);
      LoadA(cpu, (uint8_t)price);
      CALL(bank_03_CA5E_M1X0, 3, 0x37 - short_layout, true);
      LoadA(cpu, m->effect);
      CALL(bank_01_97E5_M1X0, 1, 0x3c - short_layout, false);
      if (cpu->_flag_C) {
        LoadY(cpu, 0xfc7e);
        CALL(bank_01_93B4_M1X0, 1, 0x44 - short_layout, false);
        CALL(bank_01_B1FE_M1X0, 1, 0x48 - short_layout, true);
      }
      goto acknowledge;
    }
    TEXT(m->insufficient, 0x5b - short_layout);
  } else TEXT(0xfc4f, 0x53 - short_layout);
  CALL(bank_01_8CCE_M1X0, 1, 0x68 - short_layout, false);
  Carry(cpu, true);
  goto returned;
cancelled:
  CALL(bank_01_8CB6_M1X0, 1, 0x60 - short_layout, false);
  LoadY(cpu, m->cancel);
acknowledge:
  CALL(bank_01_9270_M1X0, 1, 0x4b - short_layout, false);
  Carry(cpu, false);
returned:
  cpu->S = (uint16_t)(cpu->S + 2); /* selected command's original RTS frame */
  return RECOMP_RETURN_NORMAL;
nonlocal:
  return result >= RECOMP_RETURN_TAILCALL ? result : (RecompReturn)(result - 1);
#undef TEXT
#undef CALL
}
