#include "actraiser/actraiser_scroll_cast.h"
#include "actraiser/actraiser_cpu_hle_internal.h"
#include "actraiser/actraiser_hle_fatal.h"
#include "actraiser/actraiser_regional_runtime.h"

bool ActRaiserScrollCast_Entry(const CpuState *cpu) {
  return cpu && cpu->PB == 0 && cpu->DB == 0 && cpu->D == 0 &&
      !cpu->m_flag && !cpu->x_flag && !cpu->emulation;
}

static void Load(CpuState *cpu, uint16_t value) {
  cpu->A = value;
  ActRaiserCpuHle_SetNegativeZero16(cpu, value);
}

uint16_t ActRaiserScrollCast_Gate(CpuState *cpu,
                                 const ArRegionalCostSnapshot *prices) {
  if (!ActRaiserScrollCast_Entry(cpu) || !prices)
    ActRaiserHleFatal("Invalid generic-scroll cast gate");
  Load(cpu, cpu_read8(cpu, 0, 0x00f8)); /* LDA word / AND #$00FF */
  if (cpu->A) return 0x984e;
  const ActRaiserInventoryView inventory=ActRaiserRegional_InventoryView();
  if(inventory.enabled && cpu_read8(cpu,0,0x349)) {
    Load(cpu,cpu_read8(cpu,0,0x21));
    if(cpu->A!=inventory.count)ActRaiserHleFatal("Action inventory count diverged before cast");
    if(!cpu->A || inventory.casting)return 0x984e;
    Load(cpu,cpu_read16(cpu,0,cpu->X+0x30));
    cpu->_flag_Z=!(cpu->A&0x2008);
    cpu->P=(cpu->P&~CPU_P_Z)|(cpu->_flag_Z?CPU_P_Z:0);
    if(!cpu->_flag_Z)return 0x984e;
    uint8_t spell;
    if(!ActRaiserRegional_BeginSpell(&spell))ActRaiserHleFatal("Cannot select Action spell");
    Load(cpu,spell);cpu_write16(cpu,0,0x2ac,spell);
    return 0x9e0e; /* No debit until the native effect completes. */
  }
  Load(cpu, cpu_read16(cpu, 0, 0x02ac));
  const unsigned spell = cpu->A;
  if (!spell) return 0x984e;
  static const ArRegionalCostRule rules[] = {
      kArRegionalCost_Fire, kArRegionalCost_Stardust,
      kArRegionalCost_Aura, kArRegionalCost_Light};
  if (spell > sizeof(rules)/sizeof(rules[0]))
    ActRaiserHleFatal("Invalid equipped spell at generic-scroll cast gate: %u", spell);
  Load(cpu, cpu_read16(cpu, 0, (uint16_t)(cpu->X + 0x30)));
  /* BIT immediate changes Z only; carry/overflow must not be fabricated. */
  cpu->_flag_Z = !(cpu->A & 0x2008);
  cpu->P = (uint8_t)((cpu->P & ~CPU_P_Z) | (cpu->_flag_Z ? CPU_P_Z : 0));
  if (!cpu->_flag_Z) return 0x984e;
  Load(cpu, cpu_read8(cpu, 0, 0x0021));
  const unsigned price = prices->price[rules[spell - 1]];
  if (!price || price > 255) ActRaiserHleFatal("Invalid generic-scroll price");
  if (cpu->A < price) return 0x984e;
  const uint8_t remaining = (uint8_t)(cpu->A - price);
  cpu_write8(cpu, 0, 0x0021, remaining);
  /* SEP / DEC byte / REP preserves high stock byte, A and final M0. */
  ActRaiserCpuHle_SetNegativeZero8(cpu, remaining);
  return 0x9e0e;
}
