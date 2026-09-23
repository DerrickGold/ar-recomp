#include "actraiser/actraiser_miracle_text.h"

void ActRaiserMiracle_UpdateNativeDigit(CpuState *cpu,
                                       const ArRegionalCostSnapshot *prices) {
  if (!cpu || !prices || cpu->PB != 1 || cpu->DB != 1 || cpu->D ||
      !cpu->m_flag || cpu->x_flag || cpu->emulation || cpu->X < 2 ||
      cpu->X > 0x1000 || (cpu->X & 1) ||
      cpu_read16(cpu, 0, cpu->S + 1) != 0x9026) return;
  static const struct { uint16_t cursor; uint8_t width; ArRegionalCostRule rule; } fields[] = {
    {0xfcd5, 2, kArRegionalCost_Lightning}, {0xfd76, 2, kArRegionalCost_Rain},
    {0xff13, 2, kArRegionalCost_Sunlight}, {0xfde9, 2, kArRegionalCost_Wind},
    {0xfe71, 3, kArRegionalCost_Earthquake},
  };
  /* The literal reader and its $9003 word loop both advance Y and X before
   * $901C. Dictionary tokens can bypass the reader entry, so adapting that
   * entry alone would leave the second/third digit unchanged. */
  const unsigned source = (uint16_t)(cpu->Y - 1);
  for (unsigned i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i) {
    const unsigned index = source - fields[i].cursor;
    if (index >= fields[i].width) continue;
    const unsigned price = prices->price[fields[i].rule];
    const unsigned native = ArRegionalCosts_Descriptor(fields[i].rule)->price[kArRegionalSource_US];
    if (price == native || !price || price >= (fields[i].width == 2 ? 100u : 1000u)) return;
    unsigned divisor = 1;
    for (unsigned j = index + 1; j < fields[i].width; ++j) divisor *= 10;
    const uint8_t old = '0' + (native / divisor) % 10;
    const uint16_t tile = (uint16_t)(0xb000u + cpu->X - 2);
    if (cpu_read8(cpu, 1, (uint16_t)source) != old || cpu_read8(cpu, 0x7f, tile) != old) return;
    /* A shorter price is space-padded, preserving the native word geometry
     * and every following tile. Enhanced text uses an unpadded placeholder. */
    cpu_write8(cpu, 0x7f, tile, price < divisor ? ' ' : '0' + (price / divisor) % 10);
    return;
  }
}
